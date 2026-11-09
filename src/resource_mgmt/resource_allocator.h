#pragma once
#include <cstdint>
#include <vector>
#include "../offloading/offloading_policy.h"

namespace iov::resource_mgmt {

struct ServerSpec {
    uint64_t server_id = 0;
    double cpu_capacity = 1.0;  // mega-cycles per ms
};

// A task handed to a server.
struct Job {
    uint64_t task_id = 0;
    double workload_mcycles = 0.0;
    double data_ready_ms = 0.0;  // absolute time the upload finishes; job can't start before this
    double priority = 0.0;       // lower = served first (used by the priority discipline)
};

// Reported when a job finishes.
struct Completion {
    uint64_t task_id = 0;
    uint64_t server_id = 0;
    double start_ms = 0.0;   // when the server started executing it
    double finish_ms = 0.0;  // exact finish time (not rounded to a tick)
};

// Strategy interface for per-server queueing/execution (resource allocation).
class IResourceAllocator {
public:
    virtual ~IResourceAllocator() = default;

    // Hand a job to a server (its upload is assumed to start now).
    virtual void admit(uint64_t server_id, const Job& job) = 0;

    // Run every server forward to absolute time now_ms and return the
    // jobs that finished, with exact finish times.
    virtual std::vector<Completion> advanceTo(double now_ms) = 0;

    // Snapshot of all servers (pending work = everything not yet done).
    virtual std::vector<iov::offloading::ServerState> state() const = 0;

    // Snapshot as seen by a NEW job with the given priority: pending work
    // counts only the work that would be served before it.
    virtual std::vector<iov::offloading::ServerState> stateFor(double priority) const = 0;
};

}  // namespace iov::resource_mgmt
