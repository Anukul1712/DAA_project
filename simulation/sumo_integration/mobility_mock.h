#pragma once
#include <cmath>
#include "mobility_provider.h"

namespace iov::simulation {

// PLACEHOLDER for the real SumoBridge. Generates a handful of vehicles
// moving in simple circular paths so the rest of the pipeline (which
// only depends on IMobilityProvider) can be built, run, and demoed
// before the real libsumo integration lands.
//
// Swap-out plan: SumoBridge (sumo_bridge.h) implements the same
// interface using libsumo::vehicle::getPosition() etc. Once that's
// wired up, main.cpp changes one line to construct SumoBridge instead
// of MockMobilityProvider — nothing else in the pipeline changes.
class MockMobilityProvider : public IMobilityProvider {
public:
    explicit MockMobilityProvider(int num_vehicles, double area_size = 1000.0)
        : area_size_(area_size) {
        for (int i = 0; i < num_vehicles; ++i) {
            vehicles_.push_back({"veh_" + std::to_string(i), 0.0, 0.0, 12.0});
            phase_.push_back(i * 0.9);
        }
    }

    void step() override {
        t_ += 1.0;
        for (size_t i = 0; i < vehicles_.size(); ++i) {
            double radius = area_size_ * 0.35;
            vehicles_[i].x = area_size_ / 2 + radius * std::cos(0.05 * t_ + phase_[i]);
            vehicles_[i].y = area_size_ / 2 + radius * std::sin(0.05 * t_ + phase_[i]);
        }
    }

    std::unordered_map<std::string, VehiclePose> vehiclePositions() const override {
        std::unordered_map<std::string, VehiclePose> out;
        for (const auto& v : vehicles_) out[v.vehicle_id] = v;
        return out;
    }

    double estimateLatencyMs(const std::string& vehicle_id,
                              double server_x, double server_y) const override {
        for (const auto& v : vehicles_) {
            if (v.vehicle_id == vehicle_id) {
                double dx = v.x - server_x, dy = v.y - server_y;
                double dist = std::sqrt(dx * dx + dy * dy);
                // placeholder propagation model: base + distance-scaled term
                return 2.0 + dist * 0.05;
            }
        }
        return 1e9;  // vehicle not found -> effectively unreachable
    }

private:
    std::vector<VehiclePose> vehicles_;
    std::vector<double> phase_;
    double area_size_;
    double t_ = 0.0;
};

}  // namespace iov::simulation
