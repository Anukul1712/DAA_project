#pragma once
#include <algorithm>
#include "task_scheduler.h"

namespace iov::scheduling {

// Baseline: first-ready, first-dispatched. Ties (tasks that became ready
// at the same moment) are broken by task id, i.e. creation order.
class FifoScheduler : public ITaskScheduler {
public:
    std::string name() const override { return "fifo"; }

    std::vector<ReadyTask> order(std::vector<ReadyTask> ready) override {
        std::sort(ready.begin(), ready.end(), [](const ReadyTask& a, const ReadyTask& b) {
            if (a.ready_time_s != b.ready_time_s) return a.ready_time_s < b.ready_time_s;
            return a.task_id < b.task_id;
        });
        return ready;
    }
};

}  // namespace iov::scheduling
