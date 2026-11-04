#pragma once
#include <cmath>
#include "../../src/offloading/offloading_policy.h"

namespace iov::simulation {

// Simple analytic vehicle-to-edge-server link model, applied to real
// vehicle positions. It is NOT a radio/network simulator; it only
// captures "farther away = slower and more delayed link":
//
//   latency_ms     = base_latency_ms + per_meter_ms * d
//   rate_kb_per_ms = peak_rate / (1 + d / half_rate_distance_m)
//
// Defaults: 2 ms + 0.05 ms/m, 5 KB/ms (= 40 Mbit/s) peak, rate halves at
// 200 m. At the far side of the 600 m grid (~570 m) the rate is ~1.3 KB/ms.
struct ChannelModel {
    double base_latency_ms = 2.0;
    double per_meter_ms = 0.05;
    double peak_rate_kb_per_ms = 5.0;
    double half_rate_distance_m = 200.0;

    iov::offloading::LinkEstimate link(double vx, double vy, double sx, double sy) const {
        double d = std::hypot(vx - sx, vy - sy);
        return {base_latency_ms + per_meter_ms * d,
                peak_rate_kb_per_ms / (1.0 + d / half_rate_distance_m)};
    }
};

}  // namespace iov::simulation
