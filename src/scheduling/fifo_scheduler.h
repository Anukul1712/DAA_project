#pragma once
#include "task_scheduler.h"

namespace iov::scheduling {

// Baseline: first-ready, first-served. Priority = the time the task became
// ready (ties broken by task id, i.e. creation order).
class FifoScheduler : public ITaskScheduler {
public:
    std::string name() const override { return "fifo"; }
    double priority(const ReadyTask& t) const override { return t.ready_time_s; }
};

}  // namespace iov::scheduling
