#pragma once
#include <string>
#include "mobility_provider.h"

namespace iov::simulation {

// Real SUMO backend via libsumo. NOT YET IMPLEMENTED (needs SUMO_HOME +
// libsumo linked, see CMakeLists.txt). Until then, use
// MockMobilityProvider (mobility_mock.h) which implements the same
// IMobilityProvider interface, so the rest of the pipeline doesn't
// change when this gets filled in.
class SumoBridge : public IMobilityProvider {
public:
    explicit SumoBridge(const std::string& sumo_cfg_path);
    ~SumoBridge() override;

    void step() override;
    std::unordered_map<std::string, VehiclePose> vehiclePositions() const override;
    double estimateLatencyMs(const std::string& vehicle_id,
                              double server_x, double server_y) const override;

private:
    std::string sumo_cfg_path_;
    // TODO: libsumo state once linked (see simulation/sumo_integration/CMakeLists.txt)
};

}  // namespace iov::simulation
