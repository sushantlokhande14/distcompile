// dc-coordinator: owns the task table (PostgreSQL) and the blob store, and
// hands out work. Workers pull; nothing is pushed.

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>

#include "cas.h"
#include "db.h"
#include "graph.h"
#include "rpc.h"
#include "schema_sql.h"

using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

namespace dc {

namespace {

struct Options {
  std::string listen = "0.0.0.0:7070";
  std::string db = "host=localhost dbname=distcompile user=postgres";
  std::string cas = "/var/lib/distcompile/cas";
  int lease_s = 20;
  int max_attempts = 3;
  int pool = 8;
  bool durable = false;  // fsync every commit (see docs/performance.md for why it is off)
};

std::string join_lines(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); i++) s += (i ? "\n" : "") + v[i];
  return s;
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  size_t b = 0;
  for (;;) {
    size_t e = s.find('\n', b);
    out.push_back(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
    if (e == std::string::npos) break;
    b = e + 1;
  }
  return out;
}

// Worker-side file name for a dependency's output: obj/src/a.o -> obj_src_a.o
std::string flat_name(const std::string& n) {
  std::string s = n;
  for (char& c : s)
    if (c == '/') c = '_';
  return s;
}

// Key for archive/link actions: what runs, on which inputs, under which names.
std::string action_key(int kind, const std::string& toolchain, const std::string& args,
                       const std::vector<std::string>& names, const std::vector<std::string>& digests) {
  std::vector<std::string> parts = {"dc-action-v1", std::to_string(kind), toolchain, args};
  for (size_t i = 0; i < names.size(); i++) {
    parts.push_back(names[i]);
    parts.push_back(digests[i]);
  }
  return sha256_hex(parts);
}

const char* kKindName[] = {"compile", "archive", "link"};

}  // namespace

class CoordinatorService final : public Coordinator::Service {
 public:
  explicit CoordinatorService(const Options& o) : o_(o), pool_(o.db, o.pool), cas_(o.cas) {
    auto c = pool_.get();
    c->exec_script(kSchemaSql);
    reaper_ = std::thread([this] { reap_loop(); });
  }

  // ---- client side ---------------------------------------------------------

  Status CheckCache(ServerContext*, const KeyList* req, KeyList* rep) override {
    Timer t("rpc.CheckCache", (int64_t)req->ByteSizeLong());
    std::vector<std::string> keys(req->keys().begin(), req->keys().end());
    auto c = pool_.get();
    auto rows = c->exec("cache_check", "SELECT key FROM cache WHERE key = ANY($1::text[])", {pg_array(keys)});
    for (size_t i = 0; i < rows.size(); i++) rep->add_keys(rows.at(i, 0));
    return Status::OK;
  }

  Status FindMissing(ServerContext*, const DigestList* req, DigestList* rep) override {
    Timer t("rpc.FindMissing", (int64_t)req->ByteSizeLong());
    for (const auto& h : req->hashes())
      if (!cas_.has(h)) rep->add_hashes(h);
    return Status::OK;
  }

  Status PutBlobs(ServerContext*, const BlobBatch* req, PutReply* rep) override {
    Timer t("rpc.PutBlobs", (int64_t)req->ByteSizeLong());
    for (const auto& b : req->blobs()) {
      if (cas_.put(b.data(), b.hash()).empty())
        return Status(StatusCode::INVALID_ARGUMENT, "blob " + b.hash() + " doesn't match its contents");
      rep->set_stored(rep->stored() + 1);
      rep->set_bytes(rep->bytes() + (int64_t)b.data().size());
    }
    return Status::OK;
  }

  Status GetBlob(ServerContext*, const Digest* req, Blob* rep) override {
    Timer t("rpc.GetBlob");
    if (!cas_.get(req->hash(), rep->mutable_data())) return Status(StatusCode::NOT_FOUND, "no blob " + req->hash());
    rep->set_hash(req->hash());
    t.set_bytes((int64_t)rep->data().size());
    return Status::OK;
  }

  Status SubmitBuild(ServerContext*, const BuildSpec* spec, SubmitReply* rep) override {
    Timer t("rpc.SubmitBuild", (int64_t)spec->ByteSizeLong());
    std::vector<GraphTask> g;
    for (const auto& ts : spec->tasks()) {
      if (ts.kind() == COMPILE && (!valid_hash(ts.key()) || !valid_hash(ts.input())))
        return Status(StatusCode::INVALID_ARGUMENT, "compile task " + ts.name() + " needs a key and an input");
      g.push_back({ts.id(), (int)ts.kind(), ts.cost(), {ts.deps().begin(), ts.deps().end()}});
    }
    std::string err;
    std::vector<int> order = topo_order(g, &err);
    if (order.empty() && !g.empty()) return Status(StatusCode::INVALID_ARGUMENT, err);
    std::vector<int64_t> rank = upward_rank(g);
    std::vector<int> part = partition(g, spec->partitions() > 0 ? spec->partitions() : live_partitions());

    try {
      int64_t build = transaction(pool_, [&](Conn& c) { return submit(c, *spec, order, rank, part, rep); });
      rep->set_build_id(build);
    } catch (const std::exception& e) {
      return Status(StatusCode::FAILED_PRECONDITION, e.what());
    }
    wake_workers();
    return Status::OK;
  }

  Status GetBuild(ServerContext*, const BuildRef* req, BuildStatus* rep) override {
    Timer t("rpc.GetBuild");
    auto c = pool_.get();
    std::string id = std::to_string(req->build_id());
    auto b = c->exec("build_state", "SELECT state FROM builds WHERE id = $1", {id});
    if (b.size() == 0) return Status(StatusCode::NOT_FOUND, "no build " + id);
    rep->set_build_id(req->build_id());
    rep->set_state(b.at(0, 0));
    auto n = c->exec("build_counts",
                     "SELECT count(*), count(*) FILTER (WHERE state IN ('done', 'cached')), "
                     "count(*) FILTER (WHERE state = 'cached'), count(*) FILTER (WHERE state = 'running') "
                     "FROM tasks WHERE build_id = $1",
                     {id});
    rep->set_total(std::stoi(n.at(0, 0)));
    rep->set_finished(std::stoi(n.at(0, 1)));
    rep->set_cached(std::stoi(n.at(0, 2)));
    rep->set_running(std::stoi(n.at(0, 3)));
    // polling used to ship every task row 10x a second; now only the final call does
    if (!req->full()) return Status::OK;
    auto rows = c->exec("build_tasks",
                        "SELECT id, state, coalesce(output, ''), coalesce(lease_owner, ''), attempts, coalesce(log, ''), "
                        "coalesce(exec_ms, 0) FROM tasks WHERE build_id = $1 ORDER BY id",
                        {id});
    for (size_t i = 0; i < rows.size(); i++) {
      auto* ts = rep->add_tasks();
      ts->set_id(std::stoi(rows.at(i, 0)));
      ts->set_state(rows.at(i, 1));
      ts->set_output(rows.at(i, 2));
      ts->set_worker(rows.at(i, 3));
      ts->set_attempts(std::stoi(rows.at(i, 4)));
      ts->set_log(rows.at(i, 5));
      ts->set_exec_ms(std::stod(rows.at(i, 6)));
    }
    return Status::OK;
  }

  Status GetStats(ServerContext*, const Empty*, StatsReply* rep) override {
    for (const auto& [name, e] : stats().snapshot()) {
      auto* s = rep->add_stats();
      s->set_name(name);
      s->set_count(e.count);
      s->set_total_ms(e.ms);
      s->set_bytes(e.bytes);
    }
    auto* m = rep->add_stats();
    m->set_name("mem.peak_rss_kb");
    m->set_count(1);
    m->set_bytes(peak_rss_kb());
    return Status::OK;
  }

  // ---- worker side ---------------------------------------------------------

  Status RegisterWorker(ServerContext*, const WorkerInfo* w, Empty*) override {
    auto c = pool_.get();
    auto r = c->exec("register",
                     "INSERT INTO workers (id, toolchain, slots, part) "
                     "VALUES ($1, $2, $3, (SELECT count(*) FROM workers)) "
                     "ON CONFLICT (id) DO UPDATE SET toolchain = EXCLUDED.toolchain, slots = EXCLUDED.slots, "
                     "last_seen = now() RETURNING part",
                     {w->id(), w->toolchain(), std::to_string(w->slots())});
    std::lock_guard<std::mutex> lk(parts_mu_);
    worker_part_[w->id()] = std::stoi(r.at(0, 0));
    std::printf("worker %s registered (partition %s, %d slots)\n", w->id().c_str(), r.at(0, 0).c_str(), w->slots());
    std::fflush(stdout);
    return Status::OK;
  }

  Status Heartbeat(ServerContext*, const WorkerInfo* w, Empty*) override {
    auto c = pool_.get();
    c->exec("heartbeat",
            "UPDATE tasks SET lease_until = now() + make_interval(secs => $2) "
            "WHERE lease_owner = $1 AND state = 'running'",
            {w->id(), std::to_string(o_.lease_s)});
    c->exec("seen", "UPDATE workers SET last_seen = now() WHERE id = $1", {w->id()});
    return Status::OK;
  }

  // Long-polls: if nothing is ready, wait (up to wait_ms) for a wake-up and
  // look again, instead of making idle workers hammer the database.
  Status ClaimTask(ServerContext* ctx, const ClaimRequest* req, ClaimReply* rep) override {
    double deadline = now_ms() + std::min(std::max(req->wait_ms(), 0), 10000);
    int part = partition_of(req->worker());
    for (;;) {
      uint64_t seen;
      {
        std::lock_guard<std::mutex> lk(ready_mu_);
        seen = ready_gen_;
      }
      if (try_claim(req->worker(), req->toolchain(), part, rep)) return Status::OK;
      double left = deadline - now_ms();
      if (left <= 0 || ctx->IsCancelled()) return Status::OK;
      std::unique_lock<std::mutex> lk(ready_mu_);
      ready_cv_.wait_for(lk, std::chrono::milliseconds((int)std::min(left, 250.0)),
                         [&] { return ready_gen_ != seen; });
    }
  }

  Status CompleteTask(ServerContext*, const TaskResult* r, CompleteReply* rep) override {
    Timer t("rpc.CompleteTask", (int64_t)r->ByteSizeLong());
    stats().add("worker.exec", r->exec_ms());
    stats().add("worker.fetch", r->fetch_ms());
    stats().add("worker.upload", r->upload_ms());
    std::string uid = std::to_string(r->task_uid());
    try {
      if (r->ok() && !cas_.has(r->output())) {
        // claims success but the output never arrived: treat as a transient failure
        return CompleteTask(nullptr, &retry_copy(*r, "output blob missing"), rep);
      }
      bool accepted = transaction(pool_, [&](Conn& c) {
        if (r->ok()) {
          auto row = c.exec("complete",
                            "UPDATE tasks SET state = 'done', output = $2, exec_ms = $3, lease_owner = $4 "
                            "WHERE uid = $1 AND state IN ('ready', 'running') RETURNING build_id, id, key",
                            {uid, r->output(), std::to_string(r->exec_ms()), r->worker()});
          if (row.size() == 0) return false;  // already finished by another attempt: fine
          c.exec("cache_put",
                 "INSERT INTO cache (key, output, size) VALUES ($1, $2, $3) ON CONFLICT (key) DO NOTHING",
                 {row.at(0, 2), r->output(), std::to_string(cas_.size_of(r->output()))});
          finished(c, std::stoll(row.at(0, 0)), std::stoi(row.at(0, 1)));
          return true;
        }
        if (r->retryable()) {
          auto row = c.exec("retry",
                            "UPDATE tasks SET state = CASE WHEN attempts >= $3 THEN 'failed' ELSE 'ready' END, "
                            "lease_owner = NULL, log = $4 "
                            "WHERE uid = $1 AND state = 'running' AND attempts = $2 RETURNING build_id, state",
                            {uid, std::to_string(r->attempt()), std::to_string(o_.max_attempts), r->log()});
          if (row.size() == 0) return false;
          stats().add("sched.retry", 0);
          if (row.at(0, 1) == "failed") maybe_finish(c, std::stoll(row.at(0, 0)));
          return true;
        }
        auto row = c.exec("fail",
                          "UPDATE tasks SET state = 'failed', log = $2, exec_ms = $3 "
                          "WHERE uid = $1 AND state IN ('ready', 'running') RETURNING build_id",
                          {uid, r->log(), std::to_string(r->exec_ms())});
        if (row.size() == 0) return false;
        maybe_finish(c, std::stoll(row.at(0, 0)));
        return true;
      });
      rep->set_accepted(accepted);
      if (!accepted) stats().add("sched.duplicate_completion", 0);
    } catch (const std::exception& e) {
      return Status(StatusCode::INTERNAL, e.what());
    }
    wake_workers();
    return Status::OK;
  }

 private:
  static TaskResult& retry_copy(const TaskResult& r, const std::string& why) {
    thread_local TaskResult copy;
    copy = r;
    copy.set_ok(false);
    copy.set_retryable(true);
    copy.set_log(why);
    return copy;
  }

  int64_t submit(Conn& c, const BuildSpec& spec, const std::vector<int>& order, const std::vector<int64_t>& rank,
                 const std::vector<int>& part, SubmitReply* rep) {
    int n = spec.tasks_size();
    auto b = c.exec("new_build", "INSERT INTO builds (toolchain, total) VALUES ($1, $2) RETURNING id",
                    {spec.toolchain(), std::to_string(n)});
    int64_t build = std::stoll(b.at(0, 0));
    std::string bid = std::to_string(build);

    // Resolve as much as possible from the cache before anything is queued.
    // Walk in dependency order: a task whose inputs are all known gets its key
    // computed now, and if the key is cached, it's done before it started.
    std::vector<std::string> state(n), output(n), key(n), inputs(n), names(n);
    std::vector<int> pending(n, 0);
    std::vector<std::string> compile_keys;
    for (const auto& ts : spec.tasks())
      if (ts.kind() == COMPILE) compile_keys.push_back(ts.key());
    std::unordered_map<std::string, std::string> hits;
    auto rows = c.exec("cache_lookup", "SELECT key, output FROM cache WHERE key = ANY($1::text[])",
                       {pg_array(compile_keys)});
    for (size_t i = 0; i < rows.size(); i++) hits[rows.at(i, 0)] = rows.at(i, 1);

    for (int id : order) {
      const TaskSpec& ts = spec.tasks(id);
      bool deps_known = true;
      std::vector<std::string> in, nm;
      for (int d : ts.deps()) {
        if (output[d].empty()) {
          deps_known = false;
          pending[id]++;
        } else {
          in.push_back(output[d]);
          nm.push_back(flat_name(spec.tasks(d).name()));
        }
      }
      if (ts.kind() == COMPILE) {
        key[id] = ts.key();
        inputs[id] = ts.input();
        names[id] = "in.i";
      } else if (deps_known) {
        key[id] = action_key(ts.kind(), spec.toolchain(), join_lines({ts.args().begin(), ts.args().end()}), nm, in);
        inputs[id] = join_lines(in);
        names[id] = join_lines(nm);
        if (!hits.count(key[id])) {
          auto r = c.exec("cache_get", "SELECT output FROM cache WHERE key = $1", {key[id]});
          if (r.size()) hits[key[id]] = r.at(0, 0);
        }
      }
      if (!key[id].empty() && hits.count(key[id])) {
        state[id] = "cached";
        output[id] = hits[key[id]];
        rep->set_cached(rep->cached() + 1);
      } else if (deps_known) {
        if (ts.kind() == COMPILE && !cas_.has(ts.input()))
          throw std::runtime_error("input of " + ts.name() + " not uploaded");
        state[id] = "ready";
      } else {
        state[id] = "waiting";
      }
    }
    std::vector<std::string> hit_keys;
    for (int i = 0; i < n; i++)
      if (state[i] == "cached") hit_keys.push_back(key[i]);
    if (!hit_keys.empty())
      c.exec("cache_hits", "UPDATE cache SET hits = hits + 1 WHERE key = ANY($1::text[])", {pg_array(hit_keys)});

    // One INSERT per table: every column goes over as an array and unnest()
    // turns them back into rows. The first version did one INSERT per task and
    // per edge, which was ~0.4 s of round trips for a 2000-file build.
    std::vector<std::string> ids, kinds, tnames, args, costs, prios, parts, pends, inputs_c;
    std::vector<std::string> dep_task, dep_dep, dep_pos;
    for (int id = 0; id < n; id++) {
      const TaskSpec& ts = spec.tasks(id);
      ids.push_back(std::to_string(id));
      kinds.push_back(std::to_string((int)ts.kind()));
      tnames.push_back(ts.name());
      args.push_back(join_lines({ts.args().begin(), ts.args().end()}));
      costs.push_back(std::to_string(ts.cost()));
      prios.push_back(std::to_string(rank[id]));
      parts.push_back(std::to_string(part[id]));
      pends.push_back(std::to_string(pending[id]));
      inputs_c.push_back(ts.input());
      for (int k = 0; k < ts.deps_size(); k++) {
        dep_task.push_back(std::to_string(id));
        dep_dep.push_back(std::to_string(ts.deps(k)));
        dep_pos.push_back(std::to_string(k));
      }
    }
    c.exec("insert_tasks",
           "INSERT INTO tasks (build_id, toolchain, id, kind, name, key, input, args, cost, priority, part, state, "
           "pending_deps, output, inputs, input_names) "
           "SELECT $1, $2, u.* FROM unnest($3::int[], $4::smallint[], $5::text[], $6::text[], $7::text[], $8::text[], "
           "$9::bigint[], $10::bigint[], $11::int[], $12::text[], $13::int[], $14::text[], $15::text[], $16::text[]) "
           "AS u",
           {bid, spec.toolchain(), pg_array(ids), pg_array(kinds), pg_array(tnames), pg_array(key, true),
            pg_array(inputs_c, true), pg_array(args), pg_array(costs), pg_array(prios), pg_array(parts),
            pg_array(state), pg_array(pends), pg_array(output, true), pg_array(inputs, true), pg_array(names, true)});
    if (!dep_task.empty())
      c.exec("insert_deps",
             "INSERT INTO task_deps (build_id, task, dep, pos) "
             "SELECT $1, * FROM unnest($2::int[], $3::int[], $4::int[])",
             {bid, pg_array(dep_task), pg_array(dep_dep), pg_array(dep_pos)});
    maybe_finish(c, build);
    return build;
  }

  // A task produced its output: count down its dependents and queue any that
  // now have everything they need.
  void finished(Conn& c, int64_t build, int id) {
    std::string bid = std::to_string(build);
    // lock dependents in id order so two completions can't deadlock each other
    auto deps = c.exec("dependents",
                       "SELECT t.uid FROM task_deps d JOIN tasks t ON t.build_id = d.build_id AND t.id = d.task "
                       "WHERE d.build_id = $1 AND d.dep = $2 ORDER BY t.id FOR UPDATE OF t",
                       {bid, std::to_string(id)});
    for (size_t i = 0; i < deps.size(); i++) {
      auto r = c.exec("count_down",
                      "UPDATE tasks SET pending_deps = pending_deps - 1 WHERE uid = $1 RETURNING pending_deps, id",
                      {deps.at(i, 0)});
      if (std::stoi(r.at(0, 0)) == 0) make_ready(c, build, std::stoi(r.at(0, 1)));
    }
    // Whatever finishes last has nothing waiting on it (anything with a
    // dependent finishes before that dependent). So only sinks need the
    // "is the whole build done?" count, instead of every single completion.
    if (deps.size() == 0) maybe_finish(c, build);
  }

  void make_ready(Conn& c, int64_t build, int id) {
    std::string bid = std::to_string(build), tid = std::to_string(id);
    auto me = c.exec("task_info", "SELECT kind, args, toolchain FROM tasks WHERE build_id = $1 AND id = $2", {bid, tid});
    auto deps = c.exec("dep_outputs",
                       "SELECT t.name, t.output FROM task_deps d JOIN tasks t ON t.build_id = d.build_id AND t.id = d.dep "
                       "WHERE d.build_id = $1 AND d.task = $2 ORDER BY d.pos",
                       {bid, tid});
    std::vector<std::string> names, digests;
    for (size_t i = 0; i < deps.size(); i++) {
      names.push_back(flat_name(deps.at(i, 0)));
      digests.push_back(deps.at(i, 1));
    }
    std::string key = action_key(std::stoi(me.at(0, 0)), me.at(0, 2), me.at(0, 1), names, digests);
    auto hit = c.exec("cache_hit", "UPDATE cache SET hits = hits + 1 WHERE key = $1 RETURNING output", {key});
    if (hit.size()) {
      c.exec("mark_cached",
             "UPDATE tasks SET state = 'cached', key = $3, output = $4 WHERE build_id = $1 AND id = $2",
             {bid, tid, key, hit.at(0, 0)});
      finished(c, build, id);
    } else {
      c.exec("mark_ready",
             "UPDATE tasks SET state = 'ready', key = $3, inputs = $4, input_names = $5 "
             "WHERE build_id = $1 AND id = $2",
             {bid, tid, key, join_lines(digests), join_lines(names)});
    }
  }

  void maybe_finish(Conn& c, int64_t build) {
    std::string bid = std::to_string(build);
    auto r = c.exec("progress",
                    "SELECT count(*) FILTER (WHERE state NOT IN ('done', 'cached')), "
                    "count(*) FILTER (WHERE state = 'failed') FROM tasks WHERE build_id = $1",
                    {bid});
    long left = std::stol(r.at(0, 0)), failed = std::stol(r.at(0, 1));
    if (failed > 0)
      c.exec("build_failed", "UPDATE builds SET state = 'failed', finished_at = now() WHERE id = $1 AND state = 'running'",
             {bid});
    else if (left == 0)
      c.exec("build_done", "UPDATE builds SET state = 'done', finished_at = now() WHERE id = $1 AND state = 'running'",
             {bid});
  }

  bool try_claim(const std::string& worker, const std::string& toolchain, int part, ClaimReply* rep) {
    Timer t("sched.claim");
    auto c = pool_.get();
    // The classic Postgres job queue: SKIP LOCKED lets many claimers run at
    // once without ever handing the same row to two of them.
    //
    // Two tries: best task in our own partition, then best task anywhere
    // (stealing). Each is an index scan on a partial index. The first version
    // did it in one query with ORDER BY (part = $3) DESC, which no index can
    // serve, so every claim sorted every ready task.
    const char* kClaim =
        "UPDATE tasks SET state = 'running', lease_owner = $1, attempts = attempts + 1, "
        "lease_until = now() + make_interval(secs => $4) "
        "WHERE uid = (SELECT uid FROM tasks WHERE state = 'ready' AND toolchain = $2 %s "
        "             ORDER BY priority DESC LIMIT 1 FOR UPDATE SKIP LOCKED) "
        "RETURNING uid, attempts, kind, name, coalesce(inputs, input), coalesce(input_names, 'in.i'), args";
    char own[1024], any[1024];
    std::snprintf(own, sizeof own, kClaim, "AND part = $3");
    std::snprintf(any, sizeof any, kClaim, "AND $3::int IS NOT NULL");
    std::vector<Param> p = {worker, toolchain, std::to_string(part), std::to_string(o_.lease_s)};
    auto r = c->exec("claim_own", own, p);
    if (r.size()) {
      stats().add("sched.claim_own_partition", 0);
    } else {
      r = c->exec("claim_steal", any, p);
      if (r.size() == 0) return false;
      stats().add("sched.claim_stolen", 0);
    }
    rep->set_found(true);
    Assignment* a = rep->mutable_task();
    a->set_task_uid(std::stoll(r.at(0, 0)));
    a->set_attempt(std::stoi(r.at(0, 1)));
    a->set_kind((TaskKind)std::stoi(r.at(0, 2)));
    a->set_name(r.at(0, 3));
    for (const auto& s : split_lines(r.at(0, 4))) a->add_inputs(s);
    for (const auto& s : split_lines(r.at(0, 5))) a->add_input_names(s);
    for (const auto& s : split_lines(r.at(0, 6))) a->add_args(s);
    stats().add(std::string("sched.claimed.") + kKindName[a->kind()], 0);
    return true;
  }

  // Leases that ran out mean the worker died or hung: requeue (or give up).
  void reap_loop() {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      try {
        auto c = pool_.get();
        auto r = c->exec("reap",
                         "UPDATE tasks SET state = CASE WHEN attempts >= $1 THEN 'failed' ELSE 'ready' END, "
                         "lease_owner = NULL, "
                         "log = CASE WHEN attempts >= $1 THEN 'gave up: lease expired ' || attempts || ' times' "
                         "ELSE log END "
                         "WHERE state = 'running' AND lease_until < now() RETURNING build_id, state",
                         {std::to_string(o_.max_attempts)});
        for (size_t i = 0; i < r.size(); i++) {
          stats().add("sched.lease_expired", 0);
          if (r.at(i, 1) == "failed") maybe_finish(*c, std::stoll(r.at(i, 0)));
        }
        if (r.size()) wake_workers();
      } catch (const std::exception& e) {
        std::fprintf(stderr, "reaper: %s\n", e.what());
      }
    }
  }

  int live_partitions() {
    auto c = pool_.get();
    auto r = c->exec("live_parts",
                     "SELECT coalesce(max(part) + 1, 1) FROM workers WHERE last_seen > now() - interval '30 seconds'");
    return std::max(1, std::stoi(r.at(0, 0)));
  }

  int partition_of(const std::string& worker) {
    {
      std::lock_guard<std::mutex> lk(parts_mu_);
      auto it = worker_part_.find(worker);
      if (it != worker_part_.end()) return it->second;
    }
    // coordinator restarted since the worker registered
    auto c = pool_.get();
    auto r = c->exec("worker_part", "SELECT part FROM workers WHERE id = $1", {worker});
    int p = r.size() ? std::stoi(r.at(0, 0)) : -1;
    std::lock_guard<std::mutex> lk(parts_mu_);
    worker_part_[worker] = p;
    return p;
  }

  void wake_workers() {
    {
      std::lock_guard<std::mutex> lk(ready_mu_);
      ready_gen_++;
    }
    ready_cv_.notify_all();
  }

  Options o_;
  Pool pool_;
  Cas cas_;
  std::thread reaper_;
  std::mutex ready_mu_;
  std::condition_variable ready_cv_;
  uint64_t ready_gen_ = 0;
  std::mutex parts_mu_;
  std::unordered_map<std::string, int> worker_part_;
};

}  // namespace dc

int main(int argc, char** argv) {
  dc::Options o;
  if (const char* e = std::getenv("DC_DB")) o.db = e;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "--listen") o.listen = next();
    else if (a == "--db") o.db = next();
    else if (a == "--cas") o.cas = next();
    else if (a == "--lease") o.lease_s = std::atoi(next().c_str());
    else if (a == "--max-attempts") o.max_attempts = std::atoi(next().c_str());
    else if (a == "--db-pool") o.pool = std::atoi(next().c_str());
    else if (a == "--durable") o.durable = true;
    else {
      std::fprintf(stderr,
                   "usage: dc-coordinator [--listen addr] [--db conninfo] [--cas dir] [--lease s] "
                   "[--max-attempts n] [--db-pool n] [--durable]\n");
      return a == "-h" || a == "--help" ? 0 : 1;
    }
  }

  // Commits normally wait for the WAL to reach disk (~1 ms each here). If
  // Postgres crashes, async commit can lose the last few hundred ms of
  // updates: those tasks go back to an earlier state, their leases expire and
  // they run again. Every output is content-addressed, so running twice gives
  // the same bytes: the cost of losing a commit is a recompile, never a wrong
  // result. --durable turns it back on.
  if (!o.durable) o.db += " options='-c synchronous_commit=off'";

  // Postgres may still be starting (docker compose): retry for a while
  std::unique_ptr<dc::CoordinatorService> svc;
  for (int i = 0;; i++) {
    try {
      svc = std::make_unique<dc::CoordinatorService>(o);
      break;
    } catch (const std::exception& e) {
      if (i >= 30) {
        std::fprintf(stderr, "dc-coordinator: %s\n", e.what());
        return 1;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  grpc::ServerBuilder b;
  b.AddListeningPort(o.listen, grpc::InsecureServerCredentials());
  b.SetMaxReceiveMessageSize(dc::kMaxMessage);
  b.SetMaxSendMessageSize(dc::kMaxMessage);
  b.RegisterService(svc.get());
  auto server = b.BuildAndStart();
  if (!server) {
    std::fprintf(stderr, "dc-coordinator: cannot listen on %s\n", o.listen.c_str());
    return 1;
  }
  std::printf("dc-coordinator listening on %s (cas %s, lease %ds)\n", o.listen.c_str(), o.cas.c_str(), o.lease_s);
  std::fflush(stdout);
  server->Wait();
}
