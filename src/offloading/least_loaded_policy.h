#pragma once
#include <limits>
#include "offloading_policy.h"

namespace iov::offloading {

// Baseline: send the task to the server whose backlog would clear
// soonest (pending work / capacity), ignoring the vehicle's distance.
class LeastLoadedPolicy : public IOffloadingPolicy {
public:
    std::string name() const override { return "least_loaded"; }

    std::optional<uint64_t> choose(const iov::scheduling::Task& /*task*/,
                                   const std::vector<Candidate>& candidates) override {
        std::optional<uint64_t> best;
        double best_wait = std::numeric_limits<double>::infinity();
        for (const auto& c : candidates) {
            double wait_ms = c.server.pending_work_mcycles / c.server.cpu_capacity;
            if (wait_ms < best_wait) {
                best_wait = wait_ms;
                best = c.server.server_id;
            }
        }
        return best;
    }
};

}  // namespace iov::offloading
