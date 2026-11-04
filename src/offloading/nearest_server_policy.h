#pragma once
#include <limits>
#include "offloading_policy.h"

namespace iov::offloading {

// Baseline: send the task to the server with the lowest link latency
// (i.e. the closest one), ignoring how busy it is.
class NearestServerPolicy : public IOffloadingPolicy {
public:
    std::string name() const override { return "nearest"; }

    std::optional<uint64_t> choose(const iov::scheduling::Task& /*task*/,
                                   const std::vector<Candidate>& candidates) override {
        std::optional<uint64_t> best;
        double best_latency = std::numeric_limits<double>::infinity();
        for (const auto& c : candidates) {
            if (c.link.latency_ms < best_latency) {
                best_latency = c.link.latency_ms;
                best = c.server.server_id;
            }
        }
        return best;
    }
};

}  // namespace iov::offloading
