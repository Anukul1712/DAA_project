#pragma once
#include <string>
#include <unordered_map>

namespace iov::simulation {

struct VehiclePose {
    std::string vehicle_id;
    double x, y;
    double speed;
};

// Both the mock feed (mobility_mock.h, usable today) and the real
// SumoBridge (sumo_bridge.h, requires libsumo) implement this interface.
// offloading/ and main.cpp only ever depend on IMobilityProvider, so
// swapping the mock for the real SUMO bridge later is a one-line change.
class IMobilityProvider {
public:
    virtual ~IMobilityProvider() = default;

    // Advance mobility state by one tick.
    virtual void step() = 0;

    virtual std::unordered_map<std::string, VehiclePose> vehiclePositions() const = 0;

    // Estimated link latency (ms) from a vehicle to a fixed edge-server
    // location.
    virtual double estimateLatencyMs(const std::string& vehicle_id,
                                      double server_x, double server_y) const = 0;
};

}  // namespace iov::simulation
