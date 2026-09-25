#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dc {

std::string sha256_hex(const std::string& data);
std::string sha256_hex(const std::vector<std::string>& parts);  // hash of length-prefixed parts

bool read_file(const std::string& path, std::string* out);
// write to a temp file next to `path`, then rename: readers never see half a file
bool write_file_atomic(const std::string& path, const std::string& data);
bool make_dirs(const std::string& path);
std::string basename_of(const std::string& path);

struct ProcResult {
  int exit_code = -1;  // -1: could not start or timed out
  std::string output;  // stdout (plus stderr unless split_stderr)
  std::string err;     // stderr when split_stderr
  double ms = 0;
};
// fork/exec argv[0] (PATH lookup) in `cwd`, capture output, kill after timeout_s.
ProcResult run_process(const std::vector<std::string>& argv, const std::string& cwd = "", int timeout_s = 300,
                       bool split_stderr = false);

std::vector<std::string> split_ws(const std::string& s);
std::string trim(const std::string& s);
long peak_rss_kb();

inline double now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Thread-safe counters: how many times, how long, how many bytes.
class Stats {
 public:
  struct Entry {
    int64_t count = 0;
    double ms = 0;
    int64_t bytes = 0;
  };
  void add(const std::string& name, double ms, int64_t bytes = 0) {
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = m_[name];
    e.count++;
    e.ms += ms;
    e.bytes += bytes;
  }
  std::map<std::string, Entry> snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return m_;
  }
  std::string report(const std::string& title) const;

 private:
  mutable std::mutex mu_;
  std::map<std::string, Entry> m_;
};

Stats& stats();  // process-wide registry

class Timer {
 public:
  explicit Timer(std::string name, int64_t bytes = 0) : name_(std::move(name)), bytes_(bytes), t0_(now_ms()) {}
  ~Timer() { stats().add(name_, now_ms() - t0_, bytes_); }
  void set_bytes(int64_t b) { bytes_ = b; }

 private:
  std::string name_;
  int64_t bytes_;
  double t0_;
};

}  // namespace dc
