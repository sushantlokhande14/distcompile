#pragma once

#include <string>
#include <vector>

namespace dc {

struct CompileConfig {
  std::string driver;                   // gcc or g++
  std::string lang;                     // "c" or "c++"
  std::string toolchain;                // e.g. "gcc 13.3.0 x86_64-linux-gnu"
  std::vector<std::string> pp_flags;    // -I -D -U -include ...: only matter to the preprocessor
  std::vector<std::string> cc_flags;    // everything else: these reach the worker
  bool debug = false;                   // -g: line markers end up in the object
};

// Splits a cflags list into the two groups above.
CompileConfig make_config(const std::string& driver, const std::string& lang, const std::string& toolchain,
                          const std::vector<std::string>& cflags);

struct Preprocessed {
  bool ok = false;
  std::string error;
  std::string text;               // exactly what the worker compiles
  std::string digest;             // sha256(text): its name in the blob store
  std::string key;                // action cache key
  std::vector<std::string> deps;  // every file the preprocessor read
};

// Runs `driver -E` in `root` and derives the cache key.
Preprocessed preprocess(const std::string& root, const std::string& src, const CompileConfig& cfg);

// Line markers ("# 12 \"foo.h\" 2") and blank lines only carry positions.
// Without -g they can't change a C object file, so they're left out of the
// key: a comment-only edit to a header then invalidates nothing.
std::string strip_line_markers(const std::string& text);
std::vector<std::string> marker_files(const std::string& text);

std::string compile_key(const CompileConfig& cfg, const std::string& text);

// First line of `driver --version` (without the driver's own name) plus the target triple.
std::string toolchain_id(const std::string& driver);

}  // namespace dc
