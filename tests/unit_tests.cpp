// Unit tests for the pieces that don't need a running cluster.

#include <cstdio>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <atomic>
#include <thread>

#include "cas.h"
#include "db.h"
#include "graph.h"
#include "manifest.h"
#include "preprocess.h"
#include "util.h"

using namespace dc;

static int failures = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      failures++;                                                         \
    }                                                                     \
  } while (0)

static void test_sha256() {
  CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // length-prefixed parts: moving a boundary changes the hash
  CHECK(sha256_hex(std::vector<std::string>{"ab", "c"}) != sha256_hex(std::vector<std::string>{"a", "bc"}));
}

static void test_cas() {
  std::string dir = "/tmp/dc-unit-cas";
  std::filesystem::remove_all(dir);
  Cas cas(dir);
  std::string h = cas.put("hello");
  CHECK(h == sha256_hex("hello"));
  CHECK(cas.has(h));
  std::string back;
  CHECK(cas.get(h, &back) && back == "hello");
  CHECK(cas.size_of(h) == 5);
  CHECK(cas.put("hello", sha256_hex("other")).empty());  // lying about the hash is rejected
  CHECK(!cas.has("not-a-hash"));
  CHECK(!cas.get("../../etc/passwd", &back));

  // many writers of the same blob at once: all succeed, one file, right bytes
  std::string big(1 << 20, 'x');
  std::vector<std::thread> th;
  std::atomic<int> ok{0};
  for (int i = 0; i < 8; i++) th.emplace_back([&] { ok += !cas.put(big).empty(); });
  for (auto& t : th) t.join();
  CHECK(ok == 8);
  CHECK(cas.get(sha256_hex(big), &back) && back == big);
}

static void test_markers_and_keys() {
  std::string a = "# 1 \"main.c\"\n# 1 \"inc/a.h\" 1\nint x;\n# 3 \"main.c\" 2\nint main(){return x;}\n";
  std::string b = "# 1 \"main.c\"\n# 1 \"inc/a.h\" 1\nint x;\n# 7 \"main.c\" 2\nint main(){return x;}\n";
  CHECK(strip_line_markers(a) == "int x;\nint main(){return x;}\n");
  auto files = marker_files(a);
  CHECK(files.size() == 2 && files[0] == "main.c" && files[1] == "inc/a.h");

  CompileConfig c = make_config("gcc", "c", "tc", {"-O2", "-Iinc", "-D", "X=1", "-Wall"});
  CHECK(c.pp_flags.size() == 3 && c.cc_flags.size() == 2 && !c.debug);
  // only line numbers moved (e.g. a comment was added above): same key
  CHECK(compile_key(c, a) == compile_key(c, b));
  // with -g the line numbers end up in the object, so they must count
  CompileConfig g = make_config("gcc", "c", "tc", {"-O2", "-g"});
  CHECK(g.debug && compile_key(g, a) != compile_key(g, b));
  // different optimization level or toolchain: different key
  CHECK(compile_key(make_config("gcc", "c", "tc", {"-O0"}), a) != compile_key(c, a));
  CHECK(compile_key(make_config("gcc", "c", "tc2", {"-O2", "-Wall"}), a) != compile_key(c, a));
  // -I/-D only affect preprocessing, which already happened
  CHECK(compile_key(make_config("gcc", "c", "tc", {"-O2", "-Wall", "-Iother"}), a) == compile_key(c, a));
  // a comment line becomes a blank line in gcc -E output: must not matter for C
  std::string blank = "# 1 \"main.c\"\n\n# 1 \"inc/a.h\" 1\nint x;\n\n\n# 7 \"main.c\" 2\nint main(){return x;}\n";
  CHECK(compile_key(c, blank) == compile_key(c, a));
  // ...but C++ keeps the exact text (raw string literals can span lines)
  CompileConfig cxx = make_config("g++", "c++", "tc", {"-O2"});
  CHECK(compile_key(cxx, blank) != compile_key(cxx, a));
}

static void test_manifest() {
  std::string dir = "/tmp/dc-unit-proj";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir + "/src/m");
  for (const char* f : {"src/m/a.c", "src/m/b.c", "src/main.c"}) write_file_atomic(dir + "/" + f, "int x;\n");
  Manifest m = parse_manifest(
      "# comment\ncc gcc\ncflags -O2 -Iinc\nldflags -lm\nlib m src/m/*.c\nbin app src/main.c : m\n", dir);
  CHECK(m.targets.size() == 2);
  CHECK(m.targets[0].is_lib && m.targets[0].sources.size() == 2 && m.targets[0].sources[0] == "src/m/a.c");
  CHECK(!m.targets[1].is_lib && m.targets[1].libs.size() == 1 && m.targets[1].libs[0] == "m");
  CHECK(m.cflags.size() == 2 && m.ldflags.size() == 1);

  auto throws = [&](const std::string& text) {
    try {
      parse_manifest(text, dir);
    } catch (const std::runtime_error&) {
      return true;
    }
    return false;
  };
  CHECK(throws("frobnicate yes\n"));
  CHECK(throws("bin app src/main.c : nosuchlib\n"));
  CHECK(throws("lib m src/m/a.c\nlib m src/m/b.c\n"));
  CHECK(throws("lib m src/none/*.c\n"));
}

static void test_graph() {
  // two objects -> archive -> link, plus one object straight into the link
  std::vector<GraphTask> t = {
      {0, kCompile, 100, {}}, {1, kCompile, 300, {}}, {2, kCompile, 50, {}},
      {3, kArchive, 10, {0, 1}}, {4, kLink, 20, {2, 3}},
  };
  std::string err;
  auto order = topo_order(t, &err);
  CHECK(order.size() == 5);
  std::vector<int> pos(5);
  for (int i = 0; i < 5; i++) pos[order[i]] = i;
  CHECK(pos[0] < pos[3] && pos[1] < pos[3] && pos[3] < pos[4] && pos[2] < pos[4]);

  auto rank = upward_rank(t);
  CHECK(rank[4] == 20);
  CHECK(rank[3] == 30);
  CHECK(rank[1] == 330);  // the long chain through the archive ranks highest
  CHECK(rank[2] == 70);

  auto part = partition(t, 2);
  CHECK(part[0] == part[3] && part[1] == part[3]);  // archive stays with its objects
  CHECK(part[2] == part[4]);

  std::vector<GraphTask> cyc = {{0, kCompile, 1, {1}}, {1, kCompile, 1, {0}}};
  CHECK(topo_order(cyc, &err).empty() && err == "dependency cycle");
  std::vector<GraphTask> bad = {{0, kCompile, 1, {7}}};
  CHECK(topo_order(bad, &err).empty());
}

static void test_pg_array() {
  std::vector<std::string> v = {"a", "b c", "q\"uote", "back\\slash", ""};
  CHECK(parse_pg_array(pg_array(v)) == v);
  CHECK(pg_array({}) == "{}");
}

static void test_process() {
  auto r = run_process({"sh", "-c", "echo out; echo err >&2; exit 3"}, "", 10, true);
  CHECK(r.exit_code == 3 && r.output == "out\n" && r.err == "err\n");
  r = run_process({"sh", "-c", "echo both >&2"}, "", 10, false);
  CHECK(r.exit_code == 0 && r.output == "both\n");
  r = run_process({"sleep", "5"}, "", 1);
  CHECK(r.exit_code == -1);  // killed on timeout
  r = run_process({"/no/such/binary"}, "", 5);
  CHECK(r.exit_code == 127);
}

int main() {
  test_sha256();
  test_cas();
  test_markers_and_keys();
  test_manifest();
  test_graph();
  test_pg_array();
  test_process();
  std::printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
