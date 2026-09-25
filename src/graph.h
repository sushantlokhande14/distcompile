#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dc {

enum Kind { kCompile = 0, kArchive = 1, kLink = 2 };

struct GraphTask {
  int id;
  int kind;
  int64_t cost;           // rough work estimate
  std::vector<int> deps;  // must finish before this task
};

// Ids must be 0..n-1, deps in range, no cycles. Returns a topological order
// (Kahn's algorithm), or an empty vector and a message in *err.
std::vector<int> topo_order(const std::vector<GraphTask>& t, std::string* err);

// "Upward rank": a task's own cost plus the most expensive chain of tasks that
// waits on it. Running the highest rank first keeps the critical path moving,
// so the build doesn't end with every worker idle but one.
std::vector<int64_t> upward_rank(const std::vector<GraphTask>& t);

// Splits the graph into `parts` groups for data locality: an archive and the
// objects that go into it land in the same group (so the worker that compiled
// them usually archives them too, without downloading anything). Groups are
// balanced by cost with longest-processing-time-first. Workers prefer their
// own group and take from the others when it runs dry.
std::vector<int> partition(const std::vector<GraphTask>& t, int parts);

}  // namespace dc
