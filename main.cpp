// End-to-end demo: builds a sample task DAG, runs it through
// scheduling -> offloading -> resource_mgmt each tick, using a mock
// mobility feed standing in for SUMO (see
// simulation/sumo_integration/mobility_mock.h for the swap-out plan).
//
// This is the thing to run live at the mid-eval: it proves the four
// modules' interfaces actually fit together, not just that each
// compiles in isolation.
#include <iostream>
#include <fstream>
#include <set>
#include <unordered_set>

#include "src/scheduling/heft_lite_scheduler.h"
#include "src/offloading/greedy_latency_load_policy.h"
#include "src/resource_mgmt/fifo_resource_allocator.h"
#include "simulation/sumo_integration/mobility_mock.h"

using namespace iov;

int main() {
    // --- Sample DAG: a small fork-join application ---
    // 0 -> 1 -> 3
    //   -> 2 -> 3 -> 4
    std::vector<scheduling::Task> tasks = {
        {0, {}, 40.0, 100},
        {1, {0}, 60.0, 200},
        {2, {0}, 50.0, 150},
        {3, {1, 2}, 30.0, 100},
        {4, {3}, 20.0, 80},
    };

    // --- Server pool (heterogeneous capacity) ---
    std::vector<resource_mgmt::ServerSpec> server_specs = {
        {100, 1.0},  // slower, closer
        {101, 2.5},  // faster
    };
    std::unordered_map<uint64_t, std::pair<double, double>> server_pos = {
        {100, {200.0, 200.0}},
        {101, {800.0, 800.0}},
    };

    scheduling::HeftLiteScheduler scheduler;
    offloading::GreedyLatencyLoadPolicy offload_policy(/*latency_weight=*/0.3);
    resource_mgmt::FifoResourceAllocator allocator(server_specs);
    simulation::MockMobilityProvider mobility(/*num_vehicles=*/2);

    std::unordered_set<uint64_t> completed_tasks;
    std::unordered_set<uint64_t> dispatched_tasks;
    std::unordered_map<uint64_t, double> completion_time;

    const double dt_ms = 1.0;
    double t = 0.0;
    const double max_t = 500.0;

    while (completed_tasks.size() < tasks.size() && t < max_t) {
        mobility.step();

        // Update link latency per server using vehicle 0 as the
        // requesting client (placeholder association logic).
        auto vpos = mobility.vehiclePositions();
        for (const auto& s : server_specs) {
            double lat = mobility.estimateLatencyMs(
                "veh_0", server_pos[s.server_id].first, server_pos[s.server_id].second);
            allocator.setLinkLatency(s.server_id, lat);
        }

        // Find ready tasks not yet dispatched.
        std::vector<uint64_t> ready;
        for (const auto& task : tasks) {
            if (dispatched_tasks.count(task.id)) continue;
            bool deps_done = true;
            for (auto dep : task.dependencies)
                if (!completed_tasks.count(dep)) { deps_done = false; break; }
            if (deps_done) ready.push_back(task.id);
        }

        auto ordered = scheduler.orderReadyTasks(tasks, ready);
        auto server_state = allocator.currentState();

        for (auto task_id : ordered) {
            const auto& task = tasks[task_id];
            auto assignment = offload_policy.decide(task, server_state);
            if (assignment.server_id.has_value()) {
                allocator.admit(*assignment.server_id, {task.id, task.estimated_workload});
                dispatched_tasks.insert(task.id);
            }
        }

        for (auto done_id : allocator.tick(dt_ms)) {
            completed_tasks.insert(done_id);
            completion_time[done_id] = t;
        }

        t += dt_ms;
    }

    std::cout << "=== IoV Edge Scheduling Demo Run ===\n";
    for (const auto& task : tasks) {
        std::cout << "Task " << task.id << " completed at t="
                   << completion_time[task.id] << " ms\n";
    }
    double makespan = 0.0;
    for (const auto& [id, ct] : completion_time) makespan = std::max(makespan, ct);
    std::cout << "End-to-end completion time (makespan): " << makespan << " ms\n";

    std::ofstream out("results/demo_run.csv");
    out << "task_id,completion_time_ms\n";
    for (const auto& task : tasks) out << task.id << "," << completion_time[task.id] << "\n";
    out.close();
    std::cout << "Wrote results/demo_run.csv\n";

    return 0;
}
