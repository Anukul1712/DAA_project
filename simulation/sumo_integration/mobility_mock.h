#pragma once
#include <cmath>
#include <string>
#include "mobility_provider.h"

namespace iov::simulation {

// Synthetic mobility for quick runs without SUMO (--mock). Vehicles drive
// around a circle centred on the 600 m x 600 m grid at a constant
// speed. Positions advance by speed * step_length each step(), so the
// reported speed matches the actual motion.
class MockMobilityProvider : public IMobilityProvider {
public:
    MockMobilityProvider(int num_vehicles, double step_length_s, double area_size_m = 600.0,
                         double speed_mps = 12.0)
        : dt_(step_length_s), center_(area_size_m / 2.0), radius_(area_size_m * 0.35),
          speed_(speed_mps) {
        for (int i = 0; i < num_vehicles; ++i) {
            std::string id = "mock_" + std::to_string(i);
            phase_[id] = 2.0 * kPi * i / num_vehicles;
            vehicles_[id] = {id, 0.0, 0.0, speed_};
        }
        place();
    }

    void step() override {
        t_ += dt_;
        place();
    }

    const std::unordered_map<std::string, VehiclePose>& vehicles() const override {
        return vehicles_;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    void place() {
        double angle_now = speed_ * t_ / radius_;  // arc length / radius
        for (auto& [id, v] : vehicles_) {
            double a = angle_now + phase_[id];
            v.x = center_ + radius_ * std::cos(a);
            v.y = center_ + radius_ * std::sin(a);
        }
    }

    double dt_, center_, radius_, speed_;
    double t_ = 0.0;
    std::unordered_map<std::string, double> phase_;
    std::unordered_map<std::string, VehiclePose> vehicles_;
};

}  // namespace iov::simulation
