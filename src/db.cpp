#include "db.h"

#include <libpq-fe.h>

#include "util.h"

namespace dc {

Conn::Conn(const std::string& conninfo) : pg_(PQconnectdb(conninfo.c_str())) {
  if (PQstatus(pg_) != CONNECTION_OK) {
    std::string m = PQerrorMessage(pg_);
    PQfinish(pg_);
    pg_ = nullptr;
    throw DbError("connect: " + m, "");
  }
}

Conn::~Conn() {
  if (pg_) PQfinish(pg_);
}

bool Conn::ok() const { return pg_ && PQstatus(pg_) == CONNECTION_OK; }

Rows Conn::exec(const char* tag, const std::string& sql, const std::vector<Param>& params) {
  Timer t(std::string("db.") + tag);
  std::vector<const char*> vals;
  for (const auto& p : params) vals.push_back(p ? p->c_str() : nullptr);
  PGresult* res = PQexecParams(pg_, sql.c_str(), (int)vals.size(), nullptr, vals.data(), nullptr, nullptr, 0);
  ExecStatusType st = PQresultStatus(res);
  if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
    const char* code = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    std::string m = std::string(tag) + ": " + PQresultErrorMessage(res);
    std::string state = code ? code : "";
    PQclear(res);
    throw DbError(m, state);
  }
  Rows rows;
  int n = PQntuples(res), m = PQnfields(res);
  rows.v.resize(n);
  rows.null.resize(n);
  for (int r = 0; r < n; r++) {
    rows.v[r].resize(m);
    rows.null[r].resize(m);
    for (int c = 0; c < m; c++) {
      rows.null[r][c] = PQgetisnull(res, r, c);
      rows.v[r][c] = PQgetvalue(res, r, c);
    }
  }
  PQclear(res);
  return rows;
}

void Conn::exec_script(const std::string& sql) {
  PGresult* res = PQexec(pg_, sql.c_str());
  ExecStatusType st = PQresultStatus(res);
  std::string m = PQresultErrorMessage(res);
  PQclear(res);
  if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) throw DbError("script: " + m, "");
}

Pool::Pool(const std::string& conninfo, int size) : conninfo_(conninfo) {
  for (int i = 0; i < size; i++) idle_.push_back(std::make_unique<Conn>(conninfo));
}

Pool::Lease Pool::get() {
  Timer t("db.pool_wait");
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait(lk, [&] { return !idle_.empty(); });
  auto c = std::move(idle_.back());
  idle_.pop_back();
  lk.unlock();
  if (!c->ok()) c = std::make_unique<Conn>(conninfo_);  // reconnect after a server restart
  return Lease(this, std::move(c));
}

void Pool::give_back(std::unique_ptr<Conn> c) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    idle_.push_back(std::move(c));
  }
  cv_.notify_one();
}

std::string pg_array(const std::vector<std::string>& items, bool empty_is_null) {
  std::string s = "{";
  for (size_t i = 0; i < items.size(); i++) {
    if (i) s += ',';
    if (empty_is_null && items[i].empty()) {
      s += "NULL";
      continue;
    }
    s += '"';
    for (char c : items[i]) {
      if (c == '"' || c == '\\') s += '\\';
      s += c;
    }
    s += '"';
  }
  return s + "}";
}

std::vector<std::string> parse_pg_array(const std::string& lit) {
  std::vector<std::string> out;
  if (lit.size() < 2) return out;
  std::string cur;
  bool quoted = false, in_item = false;
  for (size_t i = 1; i + 1 < lit.size(); i++) {
    char c = lit[i];
    if (quoted) {
      if (c == '\\' && i + 2 < lit.size()) cur += lit[++i];
      else if (c == '"') quoted = false;
      else cur += c;
    } else if (c == '"') {
      quoted = in_item = true;
    } else if (c == ',') {
      out.push_back(cur);
      cur.clear();
      in_item = false;
    } else {
      cur += c;
      in_item = true;
    }
  }
  if (in_item || !cur.empty() || lit.size() > 2) out.push_back(cur);
  return out;
}

}  // namespace dc
