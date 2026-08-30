#pragma once
#include <cstdint>
#include <vector>
#include "../offloading/offloading_policy.h"

namespace iov::resource_mgmt {

struct QueueEntry {
    uint64_t task_id;
    double remaining_workload;
};

// Strategy interface: swap concrete allocation/queueing policies without
// touching callers. Concrete policies live in this same folder, e.g.
// fifo_allocator.h, priority_allocator.h.
class IResourceAllocator {
public:
    virtual ~IResourceAllocator() = default;

    // Admit a newly-assigned task onto the given server's queue.
    virtual void admit(uint64_t server_id, const QueueEntry& entry) = 0;

    // Advance simulation state by dt (ms): execute what capacity allows,
    // return ids of tasks that completed this tick.
    virtual std::vector<uint64_t> tick(double dt_ms) = 0;

    // Current state snapshot, consumed by offloading/ for decisions.
    virtual std::vector<iov::offloading::ServerState> currentState() const = 0;
};

}  // namespace iov::resource_mgmt
