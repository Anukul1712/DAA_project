#pragma once
#include <algorithm>
#include <numeric>
#include <random>
#include <string>
#include "task_scheduler.h"

namespace iov::scheduling {

// Random DAG workload:
//   - 3..6 tasks
//   - every task except the first depends on 1..2 randomly chosen earlier
//     tasks, so the graph is acyclic and topologically ordered by construction
//   - workload ~ U(100, 500) mega-cycles, input data ~ U(50, 400) KB
struct DagGeneratorParams {
    int min_tasks = 3, max_tasks = 6;
    int min_deps = 1, max_deps = 2;
    double min_workload = 100.0, max_workload = 500.0;  // mega-cycles
    double min_data_kb = 50.0, max_data_kb = 400.0;
};

inline Dag makeRandomDag(int dag_id, double arrival_s, const std::string& owner,
                         uint64_t& next_task_id, std::mt19937& rng,
                         const DagGeneratorParams& p = {}) {
    std::uniform_int_distribution<int> n_tasks(p.min_tasks, p.max_tasks);
    std::uniform_int_distribution<int> n_deps(p.min_deps, p.max_deps);
    std::uniform_real_distribution<double> work(p.min_workload, p.max_workload);
    std::uniform_real_distribution<double> data(p.min_data_kb, p.max_data_kb);

    Dag dag;
    dag.id = dag_id;
    dag.arrival_s = arrival_s;
    dag.owner_vehicle = owner;

    int n = n_tasks(rng);
    uint64_t base = next_task_id;
    next_task_id += static_cast<uint64_t>(n);
    for (int i = 0; i < n; ++i) {
        Task t;
        t.id = base + static_cast<uint64_t>(i);
        t.workload_mcycles = work(rng);
        t.data_kb = data(rng);
        if (i > 0) {
            int k = std::min(n_deps(rng), i);
            std::vector<int> pool(i);
            std::iota(pool.begin(), pool.end(), 0);
            std::shuffle(pool.begin(), pool.end(), rng);
            std::sort(pool.begin(), pool.begin() + k);
            for (int j = 0; j < k; ++j) t.dependencies.push_back(base + static_cast<uint64_t>(pool[j]));
        }
        dag.tasks.push_back(t);
    }
    return dag;
}

}  // namespace iov::scheduling
