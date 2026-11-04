#include "sumo_bridge.h"

namespace iov::simulation {

SumoBridge::SumoBridge(const std::string& host, int port) { client_.connect(host, port); }

SumoBridge::~SumoBridge() { client_.close(); }

void SumoBridge::step() {
    client_.simulationStep();
    auto samples = client_.getAllVehicleData();
    cache_.clear();
    cache_.reserve(samples.size());
    for (const auto& s : samples) cache_[s.id] = VehiclePose{s.id, s.x, s.y, s.speed};
}

const std::unordered_map<std::string, VehiclePose>& SumoBridge::vehicles() const { return cache_; }

void SumoBridge::setVehicleColor(const std::string& vehicle_id, Rgb color) {
    if (!cache_.count(vehicle_id)) return;  // vehicle already left SUMO
    client_.setVehicleColor(vehicle_id, color.r, color.g, color.b, 255);
}

}  // namespace iov::simulation
