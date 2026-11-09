#pragma once
#include <algorithm>
#include <unordered_map>
#include "task_scheduler.h"

namespace iov::scheduling {

// Upward-rank priority -- the task-prioritisation phase of HEFT
// (Topcuoglu, Hariri and Wu, IEEE TPDS 2002). For every task t:
//
//   rank(t) = w_t / c_mean + d_t / r_mean + max over children c of rank(c)
//
// with c_mean the average server capacity and r_mean a typical uplink rate.
// rank(t) estimates the time from starting t to finishing its whole DAG;
// the highest rank (longest remaining critical path) goes first, so
// priority = -rank.
class HeftLiteScheduler : public ITaskScheduler {
public:
    HeftLiteScheduler(double mean_capacity_mcycles_per_ms, double mean_rate_kb_per_ms)
        : mean_capacity_(mean_capacity_mcycles_per_ms), mean_rate_(mean_rate_kb_per_ms) {}

    std::string name() const override { return "heft"; }

    void onDagArrival(const Dag& dag) override {
        // Tasks are topologically ordered: walk backwards so children come first.
        std::unordered_map<uint64_t, double> max_child_rank;
        for (auto it = dag.tasks.rbegin(); it != dag.tasks.rend(); ++it) {
            const Task& t = *it;
            double r = t.workload_mcycles / mean_capacity_ + t.data_kb / mean_rate_ + max_child_rank[t.id];
            rank_[t.id] = r;
            for (auto dep : t.dependencies) max_child_rank[dep] = std::max(max_child_rank[dep], r);
        }
    }

    void onDagFinished(const Dag& dag) override {
        for (const auto& t : dag.tasks) rank_.erase(t.id);
    }

    double priority(const ReadyTask& t) const override { return -rank_.at(t.task_id); }
    double rankOf(uint64_t task_id) const { return rank_.at(task_id); }

private:
    double mean_capacity_;
    double mean_rate_;
    std::unordered_map<uint64_t, double> rank_;
};

}  // namespace iov::scheduling
