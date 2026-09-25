#pragma once

#include <string>
#include <vector>

namespace dc {

// DCBUILD, one directive per line:
//
//   cc      gcc                 # C compiler driver (also: cxx g++)
//   cflags  -O2 -Wall -Iinclude
//   ldflags -lm
//   lib     mathx src/mathx/*.c
//   bin     calc  src/main.c : mathx strx
//
// Globs are expanded relative to the project root and sorted.
struct Target {
  bool is_lib = false;
  std::string name;
  std::vector<std::string> sources;
  std::vector<std::string> libs;  // bin only
};

struct Manifest {
  std::string cc = "gcc";
  std::string cxx = "g++";
  std::vector<std::string> cflags;
  std::vector<std::string> ldflags;
  std::vector<Target> targets;
};

Manifest parse_manifest(const std::string& text, const std::string& root);  // throws std::runtime_error
Manifest load_manifest(const std::string& root);

}  // namespace dc
