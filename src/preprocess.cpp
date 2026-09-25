#include "preprocess.h"

#include <set>
#include <sstream>

#include "util.h"

namespace dc {

CompileConfig make_config(const std::string& driver, const std::string& lang, const std::string& toolchain,
                          const std::vector<std::string>& cflags) {
  CompileConfig c;
  c.driver = driver;
  c.lang = lang;
  c.toolchain = toolchain;
  for (size_t i = 0; i < cflags.size(); i++) {
    const std::string& f = cflags[i];
    bool pp = f.rfind("-I", 0) == 0 || f.rfind("-D", 0) == 0 || f.rfind("-U", 0) == 0 || f == "-include" ||
              f == "-isystem" || f == "-iquote";
    if (pp) {
      c.pp_flags.push_back(f);
      // "-I dir" with a space: the next word belongs to it
      if ((f == "-I" || f == "-D" || f == "-U" || f == "-include" || f == "-isystem" || f == "-iquote") &&
          i + 1 < cflags.size())
        c.pp_flags.push_back(cflags[++i]);
    } else {
      c.cc_flags.push_back(f);
      if (f.rfind("-g", 0) == 0 && f != "-g0") c.debug = true;
    }
  }
  return c;
}

static bool is_marker(const std::string& s, size_t b) {
  // "# <digits> ..." at the start of a line
  return s.compare(b, 2, "# ") == 0 && b + 2 < s.size() && s[b + 2] >= '0' && s[b + 2] <= '9';
}

std::string strip_line_markers(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t b = 0;
  while (b < text.size()) {
    size_t e = text.find('\n', b);
    if (e == std::string::npos) e = text.size();
    bool blank = text.find_first_not_of(" \t\r", b) >= e;
    // gcc -E leaves a blank line for every comment/directive line, so without
    // this a comment-only edit would still change the key
    if (!is_marker(text, b) && !blank) {
      out.append(text, b, e - b);
      out += '\n';
    }
    b = e + 1;
  }
  return out;
}

std::vector<std::string> marker_files(const std::string& text) {
  std::set<std::string> seen;
  std::vector<std::string> out;
  size_t b = 0;
  while (b < text.size()) {
    size_t e = text.find('\n', b);
    if (e == std::string::npos) e = text.size();
    if (is_marker(text, b)) {
      size_t q1 = text.find('"', b), q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
      if (q2 != std::string::npos && q2 < e) {
        std::string f = text.substr(q1 + 1, q2 - q1 - 1);
        if (!f.empty() && f[0] != '<' && seen.insert(f).second) out.push_back(f);
      }
    }
    b = e + 1;
  }
  return out;
}

std::string compile_key(const CompileConfig& cfg, const std::string& text) {
  std::string flags;
  for (const auto& f : cfg.cc_flags) flags += f + '\n';
  // Only C gets the normalized text. A C++ raw string literal can span lines,
  // so a blank line (or one that looks like a marker) can be part of a token
  // there, and dropping it could make two different programs share a key.
  bool normalize = !cfg.debug && cfg.lang == "c";
  return sha256_hex({"dc-compile-v1", cfg.toolchain, cfg.lang, flags, normalize ? strip_line_markers(text) : text});
}

Preprocessed preprocess(const std::string& root, const std::string& src, const CompileConfig& cfg) {
  Preprocessed p;
  std::vector<std::string> argv = {cfg.driver, "-E", "-x", cfg.lang};
  argv.insert(argv.end(), cfg.pp_flags.begin(), cfg.pp_flags.end());
  argv.insert(argv.end(), cfg.cc_flags.begin(), cfg.cc_flags.end());
  argv.push_back(src);
  ProcResult r;
  {
    Timer t("client.preprocess");
    r = run_process(argv, root, 120, /*split_stderr=*/true);  // warnings must not end up in the source
    t.set_bytes((int64_t)r.output.size());
  }
  if (r.exit_code != 0) {
    p.error = r.err.empty() ? r.output : r.err;
    return p;
  }
  Timer t("client.hash");
  p.text = std::move(r.output);
  p.digest = sha256_hex(p.text);
  p.key = compile_key(cfg, p.text);
  p.deps = marker_files(p.text);
  p.ok = true;
  return p;
}

std::string toolchain_id(const std::string& driver) {
  ProcResult v = run_process({driver, "--version"}, "", 30);
  ProcResult m = run_process({driver, "-dumpmachine"}, "", 30);
  if (v.exit_code != 0) return "";
  // "gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0": drop the driver name so gcc
  // and g++ from the same install compare equal
  std::string first = trim(v.output.substr(0, v.output.find('\n')));
  first = first.substr(first.find(' ') + 1);
  return first + " " + trim(m.output);
}

}  // namespace dc
