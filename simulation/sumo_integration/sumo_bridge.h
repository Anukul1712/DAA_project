#pragma once
#include <string>
#include "mobility_provider.h"
#include "traci_client.h"

namespace iov::simulation {

// Real SUMO backend. Connects over TraCI (see traci_client.h) to a `sumo`
// or `sumo-gui` process that is already running with --remote-port.
// Starting that process is done by run_experiment.py.
//
// step() advances SUMO by one step and fetches every vehicle's position
// and speed in one batched round trip; vehicles() returns that cache.
class SumoBridge : public IMobilityProvider {
public:
    // Throws TraciClientError if it cannot connect after retrying.
    SumoBridge(const std::string& host, int port);
    ~SumoBridge() override;

    void step() override;
    const std::unordered_map<std::string, VehiclePose>& vehicles() const override;
    void setVehicleColor(const std::string& vehicle_id, Rgb color) override;

private:
    TraciClient client_;
    std::unordered_map<std::string, VehiclePose> cache_;
};

}  // namespace iov::simulation
