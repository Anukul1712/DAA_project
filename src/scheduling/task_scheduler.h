#pragma once
#include <vector>
#include <cstdint>

namespace iov::scheduling {

struct Task {
    uint64_t id;
    std::vector<uint64_t> dependencies;   // predecessor task ids
    double estimated_workload;            // e.g. CPU cycles / instructions
    double data_size;                     // bytes to transfer if offloaded
};

// Strategy interface: swap concrete policies without touching callers.
// Concrete policies live in this same folder, e.g. heft_scheduler.h,
// edf_scheduler.h.
class ITaskScheduler {
public:
    virtual ~ITaskScheduler() = default;

    // Given the full DAG and the set of currently-ready task ids
    // (all dependencies satisfied), return them in the order this
    // policy wants them dispatched.
    virtual std::vector<uint64_t> orderReadyTasks(
        const std::vector<Task>& all_tasks,
        const std::vector<uint64_t>& ready_task_ids) = 0;
};

}  // namespace iov::scheduling
