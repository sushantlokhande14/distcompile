#pragma once

#include <cstdint>
#include <string>

namespace dc {

// Content-addressed blob store on disk: <root>/ab/cdef0123... where the name
// is the sha256 of the bytes. Writes are temp-file + rename, so concurrent
// writers of the same blob are harmless (same bytes, last rename wins) and a
// reader never sees a partial file.
class Cas {
 public:
  explicit Cas(std::string root);

  std::string path_of(const std::string& hash) const;
  bool has(const std::string& hash) const;
  // Stores data. If `expect` is given it must match the data's hash, so a
  // corrupted upload can't poison the store. Returns the hash, or "" on error.
  std::string put(const std::string& data, const std::string& expect = "");
  bool get(const std::string& hash, std::string* out) const;
  int64_t size_of(const std::string& hash) const;  // -1 if missing

 private:
  std::string root_;
};

bool valid_hash(const std::string& h);

}  // namespace dc
