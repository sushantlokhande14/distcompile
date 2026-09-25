// dc-worker: pulls tasks from the coordinator, runs the compiler, uploads results.

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <random>
#include <set>
#include <thread>

#include "cas.h"
#include "preprocess.h"
#include "rpc.h"

namespace fs = std::filesystem;

namespace dc {

namespace {

struct Options {
  std::string addr = "localhost:7070";
  std::string id;
  std::string cache = "/tmp/dc-worker-cache";
  std::string driver = "gcc";
  int slots = 2;
  double fail_rate = 0;  // testing: report this fraction of tasks as infrastructure failures
  int crash_after = 0;   // testing: die (without reporting) when claiming task number N
};

// The coordinator tells us what to run, so only run things we expect.
const std::set<std::string> kAllowedDrivers = {"gcc", "g++", "cc", "c++", "clang", "clang++"};

class Worker {
 public:
  explicit Worker(Options o) : o_(std::move(o)), stub_(connect(o_.addr)), cache_(o_.cache) {
    toolchain_ = toolchain_id(o_.driver);
  }

  int run() {
    if (toolchain_.empty()) {
      std::fprintf(stderr, "dc-worker: can't run %s --version\n", o_.driver.c_str());
      return 1;
    }
    WorkerInfo me;
    me.set_id(o_.id);
    me.set_toolchain(toolchain_);
    me.set_slots(o_.slots);
    Empty none;
    auto st = with_retry("RegisterWorker", [&](grpc::ClientContext& c) { return stub_->RegisterWorker(&c, me, &none); },
                         60);
    if (!st.ok()) {
      std::fprintf(stderr, "dc-worker: register: %s\n", st.error_message().c_str());
      return 1;
    }
    std::printf("dc-worker %s: %d slots, toolchain '%s'\n", o_.id.c_str(), o_.slots, toolchain_.c_str());
    std::fflush(stdout);

    std::thread hb([&] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        grpc::ClientContext c;
        c.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        Empty e;
        stub_->Heartbeat(&c, me, &e);
      }
    });
    std::vector<std::thread> slots;
    for (int i = 0; i < o_.slots; i++) slots.emplace_back([this] { loop(); });
    for (auto& t : slots) t.join();
    hb.join();
    return 0;
  }

 private:
  void loop() {
    std::mt19937_64 rng(std::hash<std::thread::id>()(std::this_thread::get_id()));
    for (;;) {
      ClaimRequest req;
      req.set_worker(o_.id);
      req.set_toolchain(toolchain_);
      req.set_wait_ms(2000);
      ClaimReply rep;
      auto st = with_retry("ClaimTask", [&](grpc::ClientContext& c) { return stub_->ClaimTask(&c, req, &rep); });
      if (!st.ok() || !rep.found()) continue;
      const Assignment& a = rep.task();

      if (o_.crash_after && ++claimed_ >= o_.crash_after) {
        std::fprintf(stderr, "dc-worker %s: simulated crash while holding %s\n", o_.id.c_str(), a.name().c_str());
        std::fflush(stderr);
        _exit(3);  // no completion: the lease will expire and someone else gets it
      }

      TaskResult res;
      if (o_.fail_rate > 0 && std::uniform_real_distribution<>(0, 1)(rng) < o_.fail_rate) {
        res.set_ok(false);
        res.set_retryable(true);
        res.set_log("injected failure");
      } else {
        res = execute(a);
      }
      res.set_task_uid(a.task_uid());
      res.set_attempt(a.attempt());
      res.set_worker(o_.id);
      CompleteReply done;
      with_retry("CompleteTask", [&](grpc::ClientContext& c) { return stub_->CompleteTask(&c, res, &done); });
    }
  }

  bool fetch(const std::string& digest, std::string* data) {
    if (cache_.get(digest, data)) {
      stats().add("worker.local_hit", 0, (int64_t)data->size());
      return true;
    }
    Digest d;
    d.set_hash(digest);
    Blob b;
    auto st = with_retry("GetBlob", [&](grpc::ClientContext& c) { return stub_->GetBlob(&c, d, &b); });
    if (!st.ok() || cache_.put(b.data(), digest).empty()) return false;
    *data = std::move(*b.mutable_data());
    return true;
  }

  TaskResult execute(const Assignment& a) {
    TaskResult res;
    auto fail = [&](const std::string& why, bool retry) {
      res.set_ok(false);
      res.set_retryable(retry);
      res.set_log(why);
      return res;
    };
    if (a.inputs_size() != a.input_names_size()) return fail("inputs and names don't line up", false);

    char tmpl[] = "/tmp/dcw-XXXXXX";
    if (!mkdtemp(tmpl)) return fail("mkdtemp failed", true);
    std::string dir = tmpl;
    struct Cleanup {
      std::string d;
      ~Cleanup() {
        std::error_code ec;
        fs::remove_all(d, ec);
      }
    } cleanup{dir};

    double t0 = now_ms();
    for (int i = 0; i < a.inputs_size(); i++) {
      std::string data;
      if (!fetch(a.inputs(i), &data)) return fail("could not fetch input " + a.inputs(i), true);
      if (a.input_names(i).find('/') != std::string::npos) return fail("bad input name", false);
      if (!write_file_atomic(dir + "/" + a.input_names(i), data)) return fail("disk write failed", true);
    }
    res.set_fetch_ms(now_ms() - t0);

    std::vector<std::string> argv;
    if (a.kind() == COMPILE) {
      if (a.args_size() == 0 || !kAllowedDrivers.count(a.args(0))) return fail("compiler not allowed", false);
      argv.assign(a.args().begin(), a.args().end());
      argv.insert(argv.end(), {"-c", "in.i", "-o", "out"});
    } else if (a.kind() == ARCHIVE) {
      argv = {"ar", "rcsD", "out"};  // D: deterministic (no timestamps/uids), so equal inputs give equal bytes
      argv.insert(argv.end(), a.input_names().begin(), a.input_names().end());
    } else {
      if (a.args_size() == 0 || !kAllowedDrivers.count(a.args(0))) return fail("linker driver not allowed", false);
      argv = {a.args(0), "-o", "out"};
      argv.insert(argv.end(), a.input_names().begin(), a.input_names().end());
      argv.insert(argv.end(), a.args().begin() + 1, a.args().end());
    }
    ProcResult pr = run_process(argv, dir, 600);
    res.set_exec_ms(pr.ms);
    stats().add("worker.exec", pr.ms);
    if (pr.exit_code != 0) return fail(pr.output.empty() ? "exit " + std::to_string(pr.exit_code) : pr.output, false);

    std::string out;
    if (!read_file(dir + "/out", &out)) return fail("no output file", false);
    double t1 = now_ms();
    std::string h = cache_.put(out);
    BlobBatch batch;
    Blob* b = batch.add_blobs();
    b->set_hash(h);
    b->set_data(std::move(out));
    PutReply pr2;
    auto st = with_retry("PutBlobs", [&](grpc::ClientContext& c) { return stub_->PutBlobs(&c, batch, &pr2); });
    if (!st.ok()) return fail("upload failed: " + st.error_message(), true);
    res.set_upload_ms(now_ms() - t1);
    res.set_ok(true);
    res.set_output(h);
    if (!pr.output.empty()) res.set_log(pr.output);  // warnings
    return res;
  }

  Options o_;
  std::unique_ptr<Coordinator::Stub> stub_;
  Cas cache_;
  std::string toolchain_;
  std::atomic<int> claimed_{0};
};

}  // namespace

}  // namespace dc

int main(int argc, char** argv) {
  dc::Options o;
  char host[256] = "worker";
  gethostname(host, sizeof host);
  o.id = std::string(host) + "-" + std::to_string(getpid());
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "--coordinator") o.addr = next();
    else if (a == "--id") o.id = next();
    else if (a == "--slots") o.slots = std::max(1, std::atoi(next().c_str()));
    else if (a == "--cache") o.cache = next();
    else if (a == "--driver") o.driver = next();
    else if (a == "--fail-rate") o.fail_rate = std::atof(next().c_str());
    else if (a == "--crash-after") o.crash_after = std::atoi(next().c_str());
    else {
      std::fprintf(stderr,
                   "usage: dc-worker [--coordinator addr] [--id name] [--slots n] [--cache dir] [--driver gcc]\n"
                   "                 [--fail-rate p] [--crash-after n]   (the last two are for testing)\n");
      return a == "-h" || a == "--help" ? 0 : 1;
    }
  }
  return dc::Worker(o).run();
}
