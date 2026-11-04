#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace iov::simulation {

struct VehiclePose {
    std::string vehicle_id;
    double x = 0.0, y = 0.0;  // metres, network coordinates
    double speed = 0.0;       // m/s
};

struct Rgb {
    uint8_t r, g, b;
};

// Source of vehicle positions. Two implementations:
//   - SumoBridge          (sumo_bridge.h): real SUMO over TraCI
//   - MockMobilityProvider (mobility_mock.h): synthetic circles, no SUMO needed
class IMobilityProvider {
public:
    virtual ~IMobilityProvider() = default;

    // Advance by one simulation step (step length is fixed by the caller).
    virtual void step() = 0;

    // Vehicles present after the last step().
    virtual const std::unordered_map<std::string, VehiclePose>& vehicles() const = 0;

    // Optional, for visualisation in sumo-gui. Default: do nothing.
    virtual void setVehicleColor(const std::string& /*vehicle_id*/, Rgb /*color*/) {}
};

}  // namespace iov::simulation
