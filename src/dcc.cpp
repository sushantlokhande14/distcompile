// dcc: the client. Preprocesses locally, asks the coordinator what it already
// has, uploads the rest, submits the dependency graph and collects the outputs.

#include <sys/stat.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "graph.h"
#include "manifest.h"
#include "preprocess.h"
#include "rpc.h"

namespace dc {

namespace {

struct Options {
  std::string cmd;
  std::string root = ".";
  std::string addr = "localhost:7070";
  std::string out = "out";
  std::string json;
  int jobs = 0;
  int partitions = 0;  // 0: coordinator picks (one per live worker)
  bool stats = false;
};

// ---- incremental state ------------------------------------------------------
//
// For every translation unit we remember its cache key and the (mtime, size)
// of every file the preprocessor read. If none of them changed, the key is
// still valid and we skip running the preprocessor at all. If a header
// changed, only the units that actually included it are redone.

struct FileStamp {
  int64_t mtime_ns = 0, size = -1;
  std::string path;
};

struct Unit {
  std::string src;
  CompileConfig cfg;
  std::string key, digest, text;
  int64_t cost = 0;
  std::vector<FileStamp> deps;
  bool fresh = false;  // preprocessed this run
  std::string error;
};

bool stamp(const std::string& root, const std::string& path, FileStamp* fs) {
  struct stat st;
  std::string full = path.size() && path[0] == '/' ? path : root + "/" + path;
  if (stat(full.c_str(), &st) != 0) return false;
  fs->path = path;
  fs->mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
  fs->size = st.st_size;
  return true;
}

std::map<std::string, Unit> load_state(const std::string& file, const std::string& config) {
  std::map<std::string, Unit> m;
  std::ifstream in(file);
  std::string header, ver, cfg;
  if (!(in >> header >> ver >> cfg) || header != "dcstate" || ver != "1" || cfg != config) return m;
  std::string tag;
  while (in >> tag && tag == "tu") {
    Unit u;
    size_t n = 0;
    in >> u.src >> u.key >> u.digest >> u.cost >> n;
    for (size_t i = 0; i < n; i++) {
      FileStamp f;
      in >> f.mtime_ns >> f.size;
      std::getline(in >> std::ws, f.path);
      u.deps.push_back(f);
    }
    m[u.src] = u;
  }
  return m;
}

void save_state(const std::string& file, const std::string& config, const std::vector<Unit>& units) {
  std::ostringstream os;
  os << "dcstate 1 " << config << "\n";
  for (const auto& u : units) {
    if (!u.error.empty() || u.key.empty()) continue;
    os << "tu " << u.src << " " << u.key << " " << u.digest << " " << u.cost << " " << u.deps.size() << "\n";
    for (const auto& f : u.deps) os << f.mtime_ns << " " << f.size << " " << f.path << "\n";
  }
  write_file_atomic(file, os.str());
}

bool still_valid(const std::string& root, const Unit& old) {
  if (old.deps.empty()) return false;
  for (const auto& f : old.deps) {
    FileStamp now;
    if (!stamp(root, f.path, &now) || now.mtime_ns != f.mtime_ns || now.size != f.size) return false;
  }
  return true;
}

void run_preprocess(const std::string& root, Unit& u) {
  Preprocessed p = preprocess(root, u.src, u.cfg);
  if (!p.ok) {
    u.error = p.error;
    return;
  }
  u.key = p.key;
  u.digest = p.digest;
  u.cost = (int64_t)p.text.size();
  u.text = std::move(p.text);
  u.fresh = true;
  u.deps.clear();
  for (const auto& d : p.deps) {
    FileStamp f;
    if (stamp(root, d, &f)) u.deps.push_back(f);
  }
}

template <class Fn>
void parallel_for(size_t n, int jobs, Fn&& fn) {
  std::atomic<size_t> next{0};
  std::vector<std::thread> th;
  for (int t = 0; t < jobs; t++)
    th.emplace_back([&] {
      for (size_t i; (i = next++) < n;) fn(i);
    });
  for (auto& t : th) t.join();
}

std::string lang_of(const std::string& src) {
  auto dot = src.rfind('.');
  std::string ext = dot == std::string::npos ? "" : src.substr(dot + 1);
  return (ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "C") ? "c++" : "c";
}

// ---- build --------------------------------------------------------------------

int build(const Options& o) {
  double t_start = now_ms();
  Manifest m = load_manifest(o.root);
  auto stub = connect(o.addr);

  std::string toolchain = toolchain_id(m.cc);
  if (toolchain.empty()) throw std::runtime_error("can't run " + m.cc + " --version");

  std::string config_words = toolchain + "\n" + m.cc + "\n" + m.cxx;
  for (const auto& f : m.cflags) config_words += "\n" + f;
  std::string config = sha256_hex(config_words).substr(0, 16);
  std::string state_file = o.root + "/.dcstate";
  auto old = load_state(state_file, config);

  // one unit per distinct source file
  std::vector<Unit> units;
  std::map<std::string, int> unit_of;
  for (const auto& t : m.targets)
    for (const auto& s : t.sources)
      if (!unit_of.count(s)) {
        unit_of[s] = (int)units.size();
        Unit u;
        u.src = s;
        std::string lang = lang_of(s);
        u.cfg = make_config(lang == "c" ? m.cc : m.cxx, lang, toolchain, m.cflags);
        units.push_back(std::move(u));
      }

  // 1. keys: reuse where nothing changed, preprocess the rest (in parallel)
  double t0 = now_ms();
  int jobs = o.jobs > 0 ? o.jobs : (int)std::max(1u, std::thread::hardware_concurrency());
  parallel_for(units.size(), jobs, [&](size_t i) {
    Unit& u = units[i];
    auto it = old.find(u.src);
    if (it != old.end() && still_valid(o.root, it->second)) {
      u.key = it->second.key;
      u.digest = it->second.digest;
      u.cost = it->second.cost;
      u.deps = it->second.deps;
    } else {
      run_preprocess(o.root, u);
    }
  });
  int fresh = 0;
  for (const auto& u : units) {
    if (!u.error.empty()) {
      std::fprintf(stderr, "%s: preprocessing failed:\n%s\n", u.src.c_str(), u.error.c_str());
      return 1;
    }
    fresh += u.fresh;
  }
  double ms_keys = now_ms() - t0;

  // 2. which results exist already
  KeyList keys, hits;
  for (const auto& u : units) keys.add_keys(u.key);
  auto st = with_retry("CheckCache", [&](grpc::ClientContext& c) { return stub->CheckCache(&c, keys, &hits); });
  if (!st.ok()) throw std::runtime_error("coordinator: " + st.error_message());
  std::set<std::string> cached(hits.keys().begin(), hits.keys().end());

  // 3. upload preprocessed sources the coordinator needs and doesn't have
  t0 = now_ms();
  std::vector<int> need;
  for (int i = 0; i < (int)units.size(); i++)
    if (!cached.count(units[i].key)) need.push_back(i);
  parallel_for(need.size(), jobs, [&](size_t k) {
    Unit& u = units[need[k]];
    if (!u.fresh) run_preprocess(o.root, u);  // key was reused but the result isn't cached: need the text
  });
  DigestList want, missing;
  for (int i : need) want.add_hashes(units[i].digest);
  st = with_retry("FindMissing", [&](grpc::ClientContext& c) { return stub->FindMissing(&c, want, &missing); });
  if (!st.ok()) throw std::runtime_error("coordinator: " + st.error_message());
  std::set<std::string> to_send(missing.hashes().begin(), missing.hashes().end());
  int64_t sent_bytes = 0, sent_blobs = 0;
  BlobBatch batch;
  int64_t batch_bytes = 0;
  auto flush = [&] {
    if (batch.blobs_size() == 0) return;
    PutReply pr;
    auto s = with_retry("PutBlobs", [&](grpc::ClientContext& c) { return stub->PutBlobs(&c, batch, &pr); });
    if (!s.ok()) throw std::runtime_error("upload: " + s.error_message());
    stats().add("client.upload", 0, batch_bytes);
    batch.Clear();
    batch_bytes = 0;
  };
  for (int i : need) {
    Unit& u = units[i];
    if (!to_send.erase(u.digest)) continue;
    Blob* b = batch.add_blobs();
    b->set_hash(u.digest);
    b->set_data(u.text);
    batch_bytes += (int64_t)u.text.size();
    sent_bytes += (int64_t)u.text.size();
    sent_blobs++;
    if (batch_bytes > (32 << 20)) flush();
  }
  flush();
  double ms_upload = now_ms() - t0;

  // 4. the graph: compile -> archive -> link
  BuildSpec spec;
  spec.set_toolchain(toolchain);
  spec.set_partitions(o.partitions);
  std::map<std::string, int> task_of_key;  // identical units compile once
  std::vector<int> unit_task(units.size());
  for (size_t i = 0; i < units.size(); i++) {
    const Unit& u = units[i];
    auto it = task_of_key.find(u.key);
    if (it != task_of_key.end()) {
      unit_task[i] = it->second;
      continue;
    }
    TaskSpec* t = spec.add_tasks();
    t->set_id(spec.tasks_size() - 1);
    t->set_kind(COMPILE);
    t->set_name("obj/" + u.src + ".o");
    t->set_key(u.key);
    t->set_input(u.digest);
    t->set_cost(u.cost);
    t->add_args(u.cfg.driver);
    t->add_args("-x");
    t->add_args(u.cfg.lang == "c" ? "cpp-output" : "c++-cpp-output");
    for (const auto& f : u.cfg.cc_flags) t->add_args(f);
    task_of_key[u.key] = unit_task[i] = t->id();
  }
  std::map<std::string, int> lib_task;
  std::map<int, std::string> output_file;  // task id -> file under out/
  for (const auto& tg : m.targets) {
    if (!tg.is_lib) continue;
    TaskSpec* t = spec.add_tasks();
    t->set_id(spec.tasks_size() - 1);
    t->set_kind(ARCHIVE);
    t->set_name("lib" + tg.name + ".a");
    t->set_cost(1000);
    std::set<int> seen;
    for (const auto& s : tg.sources)
      if (seen.insert(unit_task[unit_of[s]]).second) t->add_deps(unit_task[unit_of[s]]);
    lib_task[tg.name] = t->id();
    output_file[t->id()] = t->name();
  }
  for (const auto& tg : m.targets) {
    if (tg.is_lib) continue;
    TaskSpec* t = spec.add_tasks();
    t->set_id(spec.tasks_size() - 1);
    t->set_kind(LINK);
    t->set_name(tg.name);
    t->set_cost(5000);
    bool cxx = false;
    std::set<int> seen;
    for (const auto& s : tg.sources) {
      cxx |= lang_of(s) == "c++";
      if (seen.insert(unit_task[unit_of[s]]).second) t->add_deps(unit_task[unit_of[s]]);
    }
    for (const auto& l : tg.libs) {
      t->add_deps(lib_task[l]);
      for (const auto& lt : m.targets)
        if (lt.is_lib && lt.name == l)
          for (const auto& s : lt.sources) cxx |= lang_of(s) == "c++";
    }
    t->add_args(cxx ? m.cxx : m.cc);
    for (const auto& f : m.ldflags) t->add_args(f);
    output_file[t->id()] = t->name();
  }

  // 5. submit and wait
  t0 = now_ms();
  SubmitReply sub;
  {
    std::string wire;
    Timer ser("client.serialize");
    spec.SerializeToString(&wire);
    ser.set_bytes((int64_t)wire.size());
  }
  st = with_retry("SubmitBuild", [&](grpc::ClientContext& c) { return stub->SubmitBuild(&c, spec, &sub); });
  if (!st.ok()) throw std::runtime_error("submit: " + st.error_message());
  BuildRef ref;
  ref.set_build_id(sub.build_id());
  BuildStatus bs;
  std::string last_line;
  for (;;) {
    bs.Clear();
    st = with_retry("GetBuild", [&](grpc::ClientContext& c) { return stub->GetBuild(&c, ref, &bs); });
    if (!st.ok()) throw std::runtime_error("status: " + st.error_message());
    char line[160];
    std::snprintf(line, sizeof line, "[dcc] build %lld: %d/%d finished (%d from cache), %d running",
                  (long long)bs.build_id(), bs.finished(), bs.total(), bs.cached(), bs.running());
    if (line != last_line) {
      std::fprintf(stderr, "%s\n", line);
      last_line = line;
    }
    if (bs.state() != "running") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  double ms_remote = now_ms() - t0;
  ref.set_full(true);  // now the per-task detail: outputs, logs, who ran what
  bs.Clear();
  st = with_retry("GetBuild", [&](grpc::ClientContext& c) { return stub->GetBuild(&c, ref, &bs); });
  if (!st.ok()) throw std::runtime_error("status: " + st.error_message());

  if (bs.state() == "failed") {
    for (const auto& ts : bs.tasks())
      if (ts.state() == "failed")
        std::fprintf(stderr, "FAILED %s (attempts %d):\n%s\n", spec.tasks(ts.id()).name().c_str(), ts.attempts(),
                     ts.log().c_str());
    save_state(state_file, config, units);  // the keys are still right
    return 1;
  }

  // 6. fetch outputs we don't have yet
  t0 = now_ms();
  make_dirs(o.out);
  int64_t got_bytes = 0;
  for (const auto& [id, file] : output_file) {
    const std::string& digest = bs.tasks(id).output();
    std::string path = o.out + "/" + file, have;
    if (read_file(path, &have) && sha256_hex(have) == digest) continue;
    Digest d;
    d.set_hash(digest);
    Blob b;
    st = with_retry("GetBlob", [&](grpc::ClientContext& c) { return stub->GetBlob(&c, d, &b); });
    if (!st.ok() || sha256_hex(b.data()) != digest) throw std::runtime_error("download of " + file + " failed");
    write_file_atomic(path, b.data());
    chmod(path.c_str(), spec.tasks(id).kind() == LINK ? 0755 : 0644);
    got_bytes += (int64_t)b.data().size();
  }
  double ms_fetch = now_ms() - t0;
  save_state(state_file, config, units);

  // 7. report
  int executed[3] = {0, 0, 0}, from_cache = 0;
  double exec_ms = 0;
  std::set<std::string> workers;
  for (const auto& ts : bs.tasks()) {
    if (ts.state() == "cached") from_cache++;
    if (ts.state() == "done") {
      executed[spec.tasks(ts.id()).kind()]++;
      exec_ms += ts.exec_ms();
      workers.insert(ts.worker());
    }
  }
  double total = now_ms() - t_start;
  std::printf("dcc: %zu units, %d preprocessed, %zu reused unchanged\n", units.size(), fresh, units.size() - fresh);
  std::printf("     %d tasks: %d from cache, ran %d compile / %d archive / %d link on %zu worker(s)\n",
              bs.tasks_size(), from_cache, executed[0], executed[1], executed[2], workers.size());
  std::printf("     uploaded %lld blobs (%.1f KB), downloaded %.1f KB\n", (long long)sent_blobs, sent_bytes / 1024.0,
              got_bytes / 1024.0);
  std::printf("     keys %.0f ms, upload %.0f ms, remote %.0f ms, fetch %.0f ms, total %.0f ms\n", ms_keys, ms_upload,
              ms_remote, ms_fetch, total);
  if (o.stats) std::printf("%s  peak rss %ld KB\n", stats().report("client profile:").c_str(), peak_rss_kb());

  if (!o.json.empty()) {
    std::ostringstream js;
    js << "{\"build_id\": " << bs.build_id() << ", \"units\": " << units.size() << ", \"preprocessed\": " << fresh
       << ", \"tasks\": " << bs.tasks_size() << ", \"cached\": " << from_cache << ", \"compiled\": " << executed[0]
       << ", \"archived\": " << executed[1] << ", \"linked\": " << executed[2] << ", \"exec_ms\": " << exec_ms
       << ", \"uploaded_bytes\": " << sent_bytes << ", \"total_ms\": " << total << ", \"keys_ms\": " << ms_keys
       << ", \"upload_ms\": " << ms_upload << ", \"remote_ms\": " << ms_remote << "}\n";
    write_file_atomic(o.json, js.str());
  }
  return 0;
}

int show_stats(const Options& o) {
  auto stub = connect(o.addr);
  Empty e;
  StatsReply s;
  auto st = with_retry("GetStats", [&](grpc::ClientContext& c) { return stub->GetStats(&c, e, &s); });
  if (!st.ok()) {
    std::fprintf(stderr, "dcc: %s\n", st.error_message().c_str());
    return 1;
  }
  std::printf("  %-30s %8s %11s %10s %12s\n", "coordinator", "count", "total ms", "avg ms", "bytes");
  for (const auto& x : s.stats())
    std::printf("  %-30s %8lld %11.1f %10.3f %12lld\n", x.name().c_str(), (long long)x.count(), x.total_ms(),
                x.count() ? x.total_ms() / x.count() : 0.0, (long long)x.bytes());
  return 0;
}

}  // namespace

}  // namespace dc

int main(int argc, char** argv) {
  dc::Options o;
  if (const char* e = std::getenv("DC_COORDINATOR")) o.addr = e;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "-C") o.root = next();
    else if (a == "--coordinator") o.addr = next();
    else if (a == "--out") o.out = next();
    else if (a == "-j") o.jobs = std::atoi(next().c_str());
    else if (a == "--partitions") o.partitions = std::atoi(next().c_str());
    else if (a == "--stats") o.stats = true;
    else if (a == "--json") o.json = next();
    else if (o.cmd.empty() && a[0] != '-') o.cmd = a;
    else {
      o.cmd = "help";
      break;
    }
  }
  if (o.out == "out") o.out = o.root + "/out";
  try {
    if (o.cmd == "build") return dc::build(o);
    if (o.cmd == "stats") return dc::show_stats(o);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "dcc: %s\n", e.what());
    return 1;
  }
  std::fprintf(stderr,
               "usage: dcc build [-C dir] [--coordinator host:port] [-j N] [--out dir] [--stats] [--json file]\n"
               "                 [--partitions N]   (1 = no locality grouping, for comparison)\n"
               "       dcc stats [--coordinator host:port]\n");
  return 1;
}
