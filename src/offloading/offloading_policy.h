#pragma once
#include <cstdint>
#include <optional>
#include "../scheduling/task_scheduler.h"

namespace iov::offloading {

struct ServerState {
    uint64_t server_id;
    double queue_length;      // pending workload, from resource_mgmt/
    double cpu_capacity;      // heterogeneous resource capacity
    double link_latency_ms;   // from simulation/ (SUMO), vehicle-to-edge
};

struct Assignment {
    uint64_t task_id;
    std::optional<uint64_t> server_id;  // nullopt = execute locally
};

// Strategy interface: swap concrete offloading policies without touching
// callers. Concrete policies live in this same folder, e.g.
// nearest_server_policy.h, least_loaded_policy.h.
class IOffloadingPolicy {
public:
    virtual ~IOffloadingPolicy() = default;

    virtual Assignment decide(
        const iov::scheduling::Task& task,
        const std::vector<ServerState>& available_servers) = 0;
};

}  // namespace iov::offloading
