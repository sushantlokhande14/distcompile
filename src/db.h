#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

struct pg_conn;
typedef struct pg_conn PGconn;

namespace dc {

using Param = std::optional<std::string>;  // nullopt = SQL NULL

struct Rows {
  std::vector<std::vector<std::string>> v;
  std::vector<std::vector<bool>> null;
  size_t size() const { return v.size(); }
  const std::string& at(size_t r, size_t c) const { return v[r][c]; }
  bool is_null(size_t r, size_t c) const { return null[r][c]; }
};

struct DbError : std::runtime_error {
  std::string sqlstate;
  DbError(const std::string& m, std::string st) : std::runtime_error(m), sqlstate(std::move(st)) {}
  // deadlock or serialization failure: safe to rerun the whole transaction
  bool retryable() const { return sqlstate == "40P01" || sqlstate == "40001"; }
};

class Conn {
 public:
  explicit Conn(const std::string& conninfo);
  ~Conn();
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  // `tag` names the query in the stats ("db.<tag>"), so the profile shows
  // which statements the time goes to.
  Rows exec(const char* tag, const std::string& sql, const std::vector<Param>& params = {});
  void exec_script(const std::string& sql);  // several statements, no parameters (schema setup)
  bool ok() const;

 private:
  PGconn* pg_;
};

// Fixed-size pool. The coordinator's RPC handlers run on many gRPC threads,
// each borrowing a connection for one transaction.
class Pool {
 public:
  Pool(const std::string& conninfo, int size);

  class Lease {
   public:
    Lease(Pool* p, std::unique_ptr<Conn> c) : p_(p), c_(std::move(c)) {}
    Lease(Lease&&) = default;
    ~Lease() {
      if (c_) p_->give_back(std::move(c_));
    }
    Conn* operator->() { return c_.get(); }
    Conn& operator*() { return *c_; }

   private:
    Pool* p_;
    std::unique_ptr<Conn> c_;
  };

  Lease get();

 private:
  void give_back(std::unique_ptr<Conn> c);
  std::string conninfo_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::unique_ptr<Conn>> idle_;
};

// Runs fn(conn) inside BEGIN/COMMIT, retrying on deadlock/serialization errors.
template <class Fn>
auto transaction(Pool& pool, Fn&& fn) -> decltype(fn(std::declval<Conn&>())) {
  for (int attempt = 0;; attempt++) {
    auto c = pool.get();
    try {
      c->exec("begin", "BEGIN");
      if constexpr (std::is_void_v<decltype(fn(*c))>) {
        fn(*c);
        c->exec("commit", "COMMIT");
        return;
      } else {
        auto r = fn(*c);
        c->exec("commit", "COMMIT");
        return r;
      }
    } catch (const DbError& e) {
      try {
        c->exec("rollback", "ROLLBACK");
      } catch (...) {
      }
      if (!e.retryable() || attempt >= 5) throw;
    } catch (...) {
      try {
        c->exec("rollback", "ROLLBACK");
      } catch (...) {
      }
      throw;
    }
  }
}

// {"a","b"} array literal for array parameters; with empty_is_null, "" becomes NULL
std::string pg_array(const std::vector<std::string>& items, bool empty_is_null = false);
std::vector<std::string> parse_pg_array(const std::string& lit);

}  // namespace dc
