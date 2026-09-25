#include "cas.h"

#include <sys/stat.h>

#include "util.h"

namespace dc {

bool valid_hash(const std::string& h) {
  if (h.size() != 64) return false;
  for (char c : h)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

Cas::Cas(std::string root) : root_(std::move(root)) { make_dirs(root_); }

std::string Cas::path_of(const std::string& hash) const { return root_ + "/" + hash.substr(0, 2) + "/" + hash.substr(2); }

bool Cas::has(const std::string& hash) const {
  if (!valid_hash(hash)) return false;
  struct stat st;
  return stat(path_of(hash).c_str(), &st) == 0;
}

std::string Cas::put(const std::string& data, const std::string& expect) {
  Timer t("cas.put", (int64_t)data.size());
  std::string h = sha256_hex(data);
  if (!expect.empty() && expect != h) return "";
  if (has(h)) return h;
  return write_file_atomic(path_of(h), data) ? h : "";
}

int64_t Cas::size_of(const std::string& hash) const {
  struct stat st;
  if (!valid_hash(hash) || stat(path_of(hash).c_str(), &st) != 0) return -1;
  return (int64_t)st.st_size;
}

bool Cas::get(const std::string& hash, std::string* out) const {
  if (!valid_hash(hash)) return false;
  Timer t("cas.get");
  bool ok = read_file(path_of(hash), out);
  if (ok) t.set_bytes((int64_t)out->size());
  return ok;
}

}  // namespace dc
