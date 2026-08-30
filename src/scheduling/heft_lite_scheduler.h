#pragma once
#include <algorithm>
#include <functional>
#include <unordered_map>
#include "task_scheduler.h"

namespace iov::scheduling {

// Concrete, working policy: "upward rank" priority, a simplified HEFT
// rule. Rank(task) = task.estimated_workload + max(Rank(child)) over its
// dependents, computed once from the full DAG. Ready tasks are then
// dispatched highest-rank-first, so tasks on the longest remaining
// dependency chain (the critical path) get priority — this is what
// actually moves the needle on end-to-end completion time.
class HeftLiteScheduler : public ITaskScheduler {
public:
    std::vector<uint64_t> orderReadyTasks(
        const std::vector<Task>& all_tasks,
        const std::vector<uint64_t>& ready_task_ids) override {
        computeRanksIfNeeded(all_tasks);

        std::vector<uint64_t> ordered = ready_task_ids;
        std::sort(ordered.begin(), ordered.end(), [this](uint64_t a, uint64_t b) {
            return rank_.at(a) > rank_.at(b);
        });
        return ordered;
    }

private:
    void computeRanksIfNeeded(const std::vector<Task>& all_tasks) {
        if (!rank_.empty()) return;

        // Build children map (reverse of dependencies) once.
        std::unordered_map<uint64_t, std::vector<uint64_t>> children;
        std::unordered_map<uint64_t, const Task*> by_id;
        for (const auto& t : all_tasks) by_id[t.id] = &t;
        for (const auto& t : all_tasks)
            for (auto dep : t.dependencies) children[dep].push_back(t.id);

        // Memoized recursive upward-rank computation.
        std::function<double(uint64_t)> rankOf = [&](uint64_t id) -> double {
            auto it = rank_.find(id);
            if (it != rank_.end()) return it->second;
            double max_child_rank = 0.0;
            for (auto child_id : children[id])
                max_child_rank = std::max(max_child_rank, rankOf(child_id));
            double r = by_id.at(id)->estimated_workload + max_child_rank;
            rank_[id] = r;
            return r;
        };

        for (const auto& t : all_tasks) rankOf(t.id);
    }

    std::unordered_map<uint64_t, double> rank_;
};

}  // namespace iov::scheduling
