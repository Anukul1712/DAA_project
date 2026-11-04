#pragma once
#include <algorithm>
#include <unordered_map>
#include "task_scheduler.h"

namespace iov::scheduling {

// Upward-rank priority -- the task-prioritisation phase of HEFT
// (Topcuoglu et al., 2002). For every task:
//
//   rank(t) = mean_exec_ms(t) + mean_upload_ms(t) + max over children c of rank(c)
//
// where mean_exec_ms uses the average server capacity and mean_upload_ms
// uses an average uplink rate. rank(t) is an estimate of the time from
// starting t to finishing its whole DAG, so the waiting task with the
// highest rank is the one on the longest remaining critical path, and it
// is dispatched first.
//
// This is "HEFT-lite": it only implements HEFT's ordering phase. Server
// selection is done separately by the offloading policy (the EFT policy
// is the closest match to HEFT's processor-selection phase).
class HeftLiteScheduler : public ITaskScheduler {
public:
    HeftLiteScheduler(double mean_capacity_mcycles_per_ms, double mean_rate_kb_per_ms)
        : mean_capacity_(mean_capacity_mcycles_per_ms), mean_rate_(mean_rate_kb_per_ms) {}

    std::string name() const override { return "heft"; }

    void onDagArrival(const Dag& dag) override {
        // Tasks are topologically ordered, so walk backwards: every child
        // is ranked before its parents.
        std::unordered_map<uint64_t, double> max_child_rank;
        for (auto it = dag.tasks.rbegin(); it != dag.tasks.rend(); ++it) {
            const Task& t = *it;
            double own = t.workload_mcycles / mean_capacity_ + t.data_kb / mean_rate_;
            double r = own + max_child_rank[t.id];  // 0 if no children
            rank_[t.id] = r;
            for (auto dep : t.dependencies)
                max_child_rank[dep] = std::max(max_child_rank[dep], r);
        }
    }

    void onDagFinished(const Dag& dag) override {
        for (const auto& t : dag.tasks) rank_.erase(t.id);
    }

    std::vector<ReadyTask> order(std::vector<ReadyTask> ready) override {
        std::sort(ready.begin(), ready.end(), [this](const ReadyTask& a, const ReadyTask& b) {
            double ra = rank_.at(a.task_id), rb = rank_.at(b.task_id);
            if (ra != rb) return ra > rb;
            if (a.ready_time_s != b.ready_time_s) return a.ready_time_s < b.ready_time_s;
            return a.task_id < b.task_id;
        });
        return ready;
    }

    double rankOf(uint64_t task_id) const { return rank_.at(task_id); }

private:
    double mean_capacity_;
    double mean_rate_;
    std::unordered_map<uint64_t, double> rank_;
};

}  // namespace iov::scheduling
