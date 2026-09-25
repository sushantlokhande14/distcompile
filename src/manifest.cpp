#include "manifest.h"

#include <glob.h>

#include <set>
#include <sstream>
#include <stdexcept>

#include "util.h"

namespace dc {

namespace {

std::vector<std::string> expand(const std::string& root, const std::string& pattern, int line) {
  if (pattern.find_first_of("*?[") == std::string::npos) return {pattern};
  glob_t g{};
  std::string full = root + "/" + pattern;
  std::vector<std::string> out;
  if (glob(full.c_str(), 0, nullptr, &g) == 0)
    for (size_t i = 0; i < g.gl_pathc; i++) out.push_back(std::string(g.gl_pathv[i]).substr(root.size() + 1));
  globfree(&g);
  if (out.empty()) throw std::runtime_error("DCBUILD:" + std::to_string(line) + ": '" + pattern + "' matches nothing");
  return out;
}

}  // namespace

Manifest parse_manifest(const std::string& text, const std::string& root) {
  Manifest m;
  std::istringstream in(text);
  std::string raw;
  std::set<std::string> names;
  int line = 0;
  while (std::getline(in, raw)) {
    line++;
    auto hash = raw.find('#');
    if (hash != std::string::npos) raw = raw.substr(0, hash);
    auto w = split_ws(raw);
    if (w.empty()) continue;
    auto fail = [&](const std::string& msg) { throw std::runtime_error("DCBUILD:" + std::to_string(line) + ": " + msg); };
    const std::string& d = w[0];
    if (d == "cc" || d == "cxx") {
      if (w.size() != 2) fail(d + " takes one argument");
      (d == "cc" ? m.cc : m.cxx) = w[1];
    } else if (d == "cflags") {
      m.cflags.insert(m.cflags.end(), w.begin() + 1, w.end());
    } else if (d == "ldflags") {
      m.ldflags.insert(m.ldflags.end(), w.begin() + 1, w.end());
    } else if (d == "lib" || d == "bin") {
      if (w.size() < 3) fail(d + " needs a name and at least one source");
      Target t;
      t.is_lib = d == "lib";
      t.name = w[1];
      if (!names.insert(t.name).second) fail("duplicate target '" + t.name + "'");
      bool after_colon = false;
      for (size_t i = 2; i < w.size(); i++) {
        if (w[i] == ":") {
          if (t.is_lib) fail("libraries can't depend on libraries here");
          after_colon = true;
        } else if (after_colon) {
          t.libs.push_back(w[i]);
        } else {
          for (auto& s : expand(root, w[i], line)) t.sources.push_back(s);
        }
      }
      if (t.sources.empty()) fail("target '" + t.name + "' has no sources");
      m.targets.push_back(std::move(t));
    } else {
      fail("unknown directive '" + d + "'");
    }
  }
  for (const auto& t : m.targets)
    for (const auto& l : t.libs) {
      bool found = false;
      for (const auto& u : m.targets) found |= u.is_lib && u.name == l;
      if (!found) throw std::runtime_error("DCBUILD: bin '" + t.name + "' links unknown lib '" + l + "'");
    }
  return m;
}

Manifest load_manifest(const std::string& root) {
  std::string text;
  if (!read_file(root + "/DCBUILD", &text)) throw std::runtime_error("no DCBUILD in " + root);
  return parse_manifest(text, root);
}

}  // namespace dc
