#pragma once
#include <unordered_map>
#include "heft_lite_scheduler.h"
#include "task_scheduler.h"

namespace iov::scheduling {

// DAG-SRPT: Shortest Remaining DAG first (our scheduler).
//
// The priority of a ready task is the total remaining work of its DAG,
//
//   W_g(t) = sum of w_j over the tasks j of DAG g that have not finished yet,
//
// so the application closest to completion is served first. This is the
// multi-application version of the SRPT rule, which minimises the mean
// response time of a single server (Schrage, Operations Research 1968).
// Inside one DAG, ties are broken by HEFT upward rank (critical path first):
//
//   priority(t) = W_g(t) - epsilon * rank(t),   epsilon small enough that
//                                                rank only breaks ties.
class SrptDagScheduler : public ITaskScheduler {
public:
    SrptDagScheduler(double mean_capacity_mcycles_per_ms, double mean_rate_kb_per_ms)
        : ranks_(mean_capacity_mcycles_per_ms, mean_rate_kb_per_ms) {}

    std::string name() const override { return "srpt"; }

    void onDagArrival(const Dag& dag) override {
        ranks_.onDagArrival(dag);
        double w = 0.0;
        for (const auto& t : dag.tasks) w += t.workload_mcycles;
        remaining_[dag.id] = w;
    }

    void onTaskFinished(int dag_id, const Task& task) override {
        remaining_[dag_id] -= task.workload_mcycles;
    }

    void onDagFinished(const Dag& dag) override {
        ranks_.onDagFinished(dag);
        remaining_.erase(dag.id);
    }

    double priority(const ReadyTask& t) const override {
        return remaining_.at(t.dag_id) - kTieEpsilon * ranks_.rankOf(t.task_id);
    }

    double remainingWork(int dag_id) const { return remaining_.at(dag_id); }

private:
    static constexpr double kTieEpsilon = 1e-6;  // Mcycles per ms of rank
    HeftLiteScheduler ranks_;
    std::unordered_map<int, double> remaining_;
};

}  // namespace iov::scheduling
