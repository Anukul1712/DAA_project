#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "../scheduling/task_scheduler.h"

namespace iov::offloading {

// Snapshot of one edge server, produced by resource_mgmt/.
struct ServerState {
    uint64_t server_id = 0;
    double cpu_capacity = 0.0;          // mega-cycles per ms
    int tasks_in_system = 0;            // uploading + queued + running
    double pending_work_mcycles = 0.0;  // work not yet done (uploading + queued + running remainder)
};

// Vehicle-to-server link, produced by the channel model from real
// vehicle positions.
struct LinkEstimate {
    double latency_ms = 0.0;      // fixed + distance-dependent delay
    double rate_kb_per_ms = 1.0;  // uplink throughput
    double uploadMs(double data_kb) const { return latency_ms + data_kb / rate_kb_per_ms; }
};

// One server the task may be sent to (it has a free admission slot).
struct Candidate {
    ServerState server;
    LinkEstimate link;
    double distance_m = 0.0;  // vehicle-to-server distance (raw input for the ML policy)
};

// Strategy interface: pick a server for one task. Returns nullopt to
// leave the task waiting (it is retried next tick).
class IOffloadingPolicy {
public:
    virtual ~IOffloadingPolicy() = default;
    virtual std::string name() const = 0;
    virtual std::optional<uint64_t> choose(const iov::scheduling::Task& task,
                                           const std::vector<Candidate>& candidates) = 0;
};

// Shared helper: estimated finish time (ms from now) of `task` on `c`.
// Upload and the server's existing backlog overlap, so the task can start
// when both are done; it then runs for workload / capacity.
inline double estimatedFinishMs(const iov::scheduling::Task& task, const Candidate& c) {
    double upload = c.link.uploadMs(task.data_kb);
    double backlog = c.server.pending_work_mcycles / c.server.cpu_capacity;
    double start = upload > backlog ? upload : backlog;
    return start + task.workload_mcycles / c.server.cpu_capacity;
}

}  // namespace iov::offloading
