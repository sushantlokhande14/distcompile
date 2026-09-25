#include "graph.h"

#include <algorithm>
#include <numeric>

namespace dc {

std::vector<int> topo_order(const std::vector<GraphTask>& t, std::string* err) {
  int n = (int)t.size();
  std::vector<int> indeg(n, 0), order;
  std::vector<std::vector<int>> succ(n);
  for (int i = 0; i < n; i++) {
    if (t[i].id != i) {
      if (err) *err = "task ids must be 0..n-1";
      return {};
    }
    for (int d : t[i].deps) {
      if (d < 0 || d >= n || d == i) {
        if (err) *err = "task " + std::to_string(i) + " has a bad dependency " + std::to_string(d);
        return {};
      }
      succ[d].push_back(i);
      indeg[i]++;
    }
  }
  for (int i = 0; i < n; i++)
    if (indeg[i] == 0) order.push_back(i);
  for (size_t k = 0; k < order.size(); k++)
    for (int s : succ[order[k]])
      if (--indeg[s] == 0) order.push_back(s);
  if ((int)order.size() != n) {
    if (err) *err = "dependency cycle";
    return {};
  }
  return order;
}

std::vector<int64_t> upward_rank(const std::vector<GraphTask>& t) {
  std::vector<int> order = topo_order(t, nullptr);
  std::vector<std::vector<int>> succ(t.size());
  for (const auto& x : t)
    for (int d : x.deps) succ[d].push_back(x.id);
  std::vector<int64_t> rank(t.size(), 0);
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    int i = *it;
    int64_t best = 0;
    for (int s : succ[i]) best = std::max(best, rank[s]);
    rank[i] = t[i].cost + best;
  }
  return rank;
}

std::vector<int> partition(const std::vector<GraphTask>& t, int parts) {
  int n = (int)t.size();
  std::vector<int> part(n, 0);
  if (parts <= 1 || n == 0) return part;

  // Units that should stay together: each archive with its objects, each link
  // with any objects nobody else claimed, and leftover tasks on their own.
  std::vector<int> unit(n, -1);
  int units = 0;
  auto claim = [&](int kind) {
    for (int i = 0; i < n; i++) {
      if (t[i].kind != kind || unit[i] >= 0) continue;
      unit[i] = units;
      for (int d : t[i].deps)
        if (t[d].kind == kCompile && unit[d] < 0) unit[d] = units;
      units++;
    }
  };
  claim(kArchive);
  claim(kLink);
  for (int i = 0; i < n; i++)
    if (unit[i] < 0) unit[i] = units++;

  std::vector<int64_t> cost(units, 0);
  for (int i = 0; i < n; i++) cost[unit[i]] += std::max<int64_t>(1, t[i].cost);
  std::vector<int> by_cost(units);
  std::iota(by_cost.begin(), by_cost.end(), 0);
  std::stable_sort(by_cost.begin(), by_cost.end(), [&](int a, int b) { return cost[a] > cost[b]; });

  // LPT: biggest unit to the currently lightest partition
  std::vector<int64_t> load(parts, 0);
  std::vector<int> unit_part(units, 0);
  for (int u : by_cost) {
    int p = int(std::min_element(load.begin(), load.end()) - load.begin());
    unit_part[u] = p;
    load[p] += cost[u];
  }
  for (int i = 0; i < n; i++) part[i] = unit_part[unit[i]];
  return part;
}

}  // namespace dc
