#pragma once
#include <deque>
#include <unordered_map>
#include "resource_allocator.h"

namespace iov::resource_mgmt {

struct ServerSpec {
    uint64_t server_id;
    double cpu_capacity;  // work units processed per ms
};

// Concrete, working allocator: one FIFO queue per server. Each tick(),
// the head-of-line task on every server gets cpu_capacity * dt of work
// done; when its remaining_workload hits zero it's popped and reported
// as completed. This directly models "dynamic queue congestion" and
// "heterogeneous resources" from the objective — congestion because
// queue_length is just deque size, heterogeneity because each server
// has its own cpu_capacity.
class FifoResourceAllocator : public IResourceAllocator {
public:
    explicit FifoResourceAllocator(std::vector<ServerSpec> servers) {
        for (const auto& s : servers) {
            specs_[s.server_id] = s;
            queues_[s.server_id] = {};
            link_latency_ms_[s.server_id] = 0.0;
        }
    }

    void admit(uint64_t server_id, const QueueEntry& entry) override {
        queues_[server_id].push_back(entry);
    }

    // Optional: let the mobility layer push in latency estimates so
    // currentState() reflects live conditions.
    void setLinkLatency(uint64_t server_id, double latency_ms) {
        link_latency_ms_[server_id] = latency_ms;
    }

    std::vector<uint64_t> tick(double dt_ms) override {
        std::vector<uint64_t> completed;
        for (auto& [server_id, q] : queues_) {
            if (q.empty()) continue;
            double capacity = specs_[server_id].cpu_capacity;
            q.front().remaining_workload -= capacity * dt_ms;
            if (q.front().remaining_workload <= 0.0) {
                completed.push_back(q.front().task_id);
                q.pop_front();
            }
        }
        return completed;
    }

    std::vector<iov::offloading::ServerState> currentState() const override {
        std::vector<iov::offloading::ServerState> out;
        for (const auto& [server_id, spec] : specs_) {
            out.push_back({server_id,
                            static_cast<double>(queues_.at(server_id).size()),
                            spec.cpu_capacity,
                            link_latency_ms_.at(server_id)});
        }
        return out;
    }

private:
    std::unordered_map<uint64_t, ServerSpec> specs_;
    std::unordered_map<uint64_t, std::deque<QueueEntry>> queues_;
    std::unordered_map<uint64_t, double> link_latency_ms_;
};

}  // namespace iov::resource_mgmt
