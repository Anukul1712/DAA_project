#pragma once
#include <limits>
#include "offloading_policy.h"

namespace iov::offloading {

// Earliest Finish Time: send the task to the server where it is
// estimated to finish first, taking into account all three costs in the
// same unit (milliseconds):
//   - upload time   (link latency + data / uplink rate; depends on distance)
//   - queueing time (server backlog / capacity)
//   - execution time (workload / capacity; depends on server speed)
// See estimatedFinishMs() in offloading_policy.h.
class EftPolicy : public IOffloadingPolicy {
public:
    std::string name() const override { return "eft"; }

    std::optional<uint64_t> choose(const iov::scheduling::Task& task,
                                   const std::vector<Candidate>& candidates) override {
        std::optional<uint64_t> best;
        double best_finish = std::numeric_limits<double>::infinity();
        for (const auto& c : candidates) {
            double f = estimatedFinishMs(task, c);
            if (f < best_finish) {
                best_finish = f;
                best = c.server.server_id;
            }
        }
        return best;
    }
};

}  // namespace iov::offloading
