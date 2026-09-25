#include "util.h"

#include <fcntl.h>
#include <openssl/evp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dc {

namespace {

std::string to_hex(const unsigned char* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s(n * 2, ' ');
  for (size_t i = 0; i < n; i++) {
    s[2 * i] = d[p[i] >> 4];
    s[2 * i + 1] = d[p[i] & 15];
  }
  return s;
}

}  // namespace

std::string sha256_hex(const std::string& data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  EVP_Digest(data.data(), data.size(), md, &n, EVP_sha256(), nullptr);
  return to_hex(md, n);
}

std::string sha256_hex(const std::vector<std::string>& parts) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  for (const auto& p : parts) {
    // length prefix so ("ab","c") and ("a","bc") can't collide
    uint64_t len = p.size();
    EVP_DigestUpdate(ctx, &len, sizeof len);
    EVP_DigestUpdate(ctx, p.data(), p.size());
  }
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  EVP_DigestFinal_ex(ctx, md, &n);
  EVP_MD_CTX_free(ctx);
  return to_hex(md, n);
}

bool read_file(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

bool make_dirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i <= path.size(); i++) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty() && cur != "/") mkdir(cur.c_str(), 0755);
    }
    if (i < path.size()) cur += path[i];
  }
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool write_file_atomic(const std::string& path, const std::string& data) {
  auto slash = path.rfind('/');
  if (slash != std::string::npos) make_dirs(path.substr(0, slash));
  char suffix[64];
  std::snprintf(suffix, sizeof suffix, ".tmp.%d.%lx", getpid(), (unsigned long)pthread_self());
  std::string tmp = path + suffix;
  {
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    if (!f) return false;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

std::string basename_of(const std::string& path) {
  auto p = path.rfind('/');
  return p == std::string::npos ? path : path.substr(p + 1);
}

ProcResult run_process(const std::vector<std::string>& argv, const std::string& cwd, int timeout_s,
                       bool split_stderr) {
  ProcResult r;
  double t0 = now_ms();
  int out[2], err[2] = {-1, -1};
  if (pipe(out) != 0) return r;
  if (split_stderr && pipe(err) != 0) {
    close(out[0]);
    close(out[1]);
    return r;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(out[0]);
    close(out[1]);
    if (split_stderr) {
      close(err[0]);
      close(err[1]);
    }
    return r;
  }
  if (pid == 0) {
    dup2(out[1], 1);
    dup2(split_stderr ? err[1] : out[1], 2);
    close(out[0]);
    close(out[1]);
    if (split_stderr) {
      close(err[0]);
      close(err[1]);
    }
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(out[1]);
  if (split_stderr) close(err[1]);

  // read both pipes until both hit EOF, so a chatty stderr can't block the child
  pollfd fds[2] = {{out[0], POLLIN, 0}, {split_stderr ? err[0] : -1, POLLIN, 0}};
  std::string* sink[2] = {&r.output, &r.err};
  int open_fds = split_stderr ? 2 : 1;
  char buf[65536];
  double deadline = t0 + timeout_s * 1000.0;
  bool timed_out = false;
  while (open_fds > 0) {
    int wait = (int)std::max(0.0, deadline - now_ms());
    int pr = poll(fds, 2, wait);
    if (pr == 0) {
      timed_out = true;
      kill(pid, SIGKILL);
      break;
    }
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int k = 0; k < 2; k++) {
      if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      ssize_t n = read(fds[k].fd, buf, sizeof buf);
      if (n > 0) {
        sink[k]->append(buf, (size_t)n);
      } else {
        close(fds[k].fd);
        fds[k].fd = -1;
        open_fds--;
      }
    }
  }
  for (auto& p : fds)
    if (p.fd >= 0) close(p.fd);
  int status = 0;
  waitpid(pid, &status, 0);
  if (!timed_out && WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
  if (timed_out) r.output += "\n[timed out]";
  r.ms = now_ms() - t0;
  return r;
}

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream is(s);
  for (std::string w; is >> w;) out.push_back(w);
  return out;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

long peak_rss_kb() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
}

Stats& stats() {
  static Stats s;
  return s;
}

std::string Stats::report(const std::string& title) const {
  auto snap = snapshot();
  std::string out = title + "\n";
  char line[256];
  std::snprintf(line, sizeof line, "  %-28s %8s %11s %11s %12s\n", "", "count", "total ms", "avg ms", "bytes");
  out += line;
  for (const auto& [name, e] : snap) {
    std::snprintf(line, sizeof line, "  %-28s %8lld %11.1f %11.3f %12lld\n", name.c_str(), (long long)e.count, e.ms,
                  e.count ? e.ms / e.count : 0.0, (long long)e.bytes);
    out += line;
  }
  return out;
}

}  // namespace dc
