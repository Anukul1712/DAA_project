// iov_sim: experiment driver for DAG task scheduling + offloading in an
// Internet-of-Vehicles edge network.
//
// Every tick (default 0.1 s):
//   1. Advance mobility (real SUMO via TraCI, or the --mock feed).
//   2. Run the edge servers forward; collect tasks that finished.
//      Children whose dependencies are now all done become "ready".
//   3. New DAGs arrive (Poisson process), each owned by a random vehicle.
//   4. Dispatch: the scheduler orders all ready tasks; each one, in that
//      order, is offered to the servers that have a free admission slot,
//      and the offloading policy picks one. Tasks that find no free slot
//      wait for the next tick -- this is where scheduling order matters.
//   5. Log metrics.
//
// A task's life:  ready -> (wait for slot) -> dispatched -> upload over the
// vehicle's current link -> queue at the server -> execute -> done.
// A DAG's makespan is (last task finish - DAG arrival).
//
// Outputs (prefix set by --out-prefix):
//   <prefix>_tasks.csv     one row per finished task, with time breakdown
//   <prefix>_dags.csv      one row per DAG (makespan, slowdown, ...)
//   <prefix>_ticks.csv     one row per tick (vehicles, queues, ...)
//   <prefix>_servers.csv   server positions/capacities (for plots)
//   <prefix>_summary.csv   one-row summary of the run
//   <prefix>_positions.csv vehicle positions every 0.5 s (--log-positions)
//   <prefix>_decisions.csv one row per finished task with the ML features of
//                          its chosen server and the real time it took
//                          (--log-decisions; training data for ml/)
// With --light only the dags and summary files are written (for big sweeps).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "simulation/sumo_integration/channel_model.h"
#include "simulation/sumo_integration/mobility_mock.h"
#include "simulation/sumo_integration/mobility_provider.h"
#include "simulation/sumo_integration/sumo_bridge.h"
#include "src/offloading/eft_policy.h"
#include "src/offloading/least_loaded_policy.h"
#include "src/offloading/mlp_policy.h"
#include "src/offloading/nearest_server_policy.h"
#include "src/offloading/offloading_policy.h"
#include "src/resource_mgmt/fifo_resource_allocator.h"
#include "src/scheduling/dag_generator.h"
#include "src/scheduling/fifo_scheduler.h"
#include "src/scheduling/heft_lite_scheduler.h"
#include "src/scheduling/task_scheduler.h"
#include "src/scheduling/workload.h"

using namespace iov;

namespace {

// ---------------------------------------------------------------- config

struct Config {
    std::string host = "127.0.0.1";
    int port = -1;
    bool use_mock = false;
    int mock_num_vehicles = 40;

    std::string scheduler = "heft";  // heft | fifo
    std::string offloading = "eft";  // eft | nearest | least_loaded | ml | random
    std::string workload = "synthetic";  // synthetic | wfcommons
    std::string dag_file = "data/wfcommons_dags.csv";
    std::string ml_model = "ml/offload_mlp.txt";

    double step_length_s = 0.1;   // must match SUMO's --step-length
    double warmup_s = 60.0;       // no DAGs before this (lets traffic build up)
    double duration_s = 300.0;    // DAGs arrive during [warmup, warmup + duration)
    double drain_s = 120.0;       // after arrivals stop, wait at most this long for DAGs to finish
    double load = 0.4;            // offered load = arrival rate x mean DAG work / total server capacity
    double arrival_rate = -1.0;   // DAGs per second; if set, overrides --load
    int server_slots = 0;         // max tasks per server (uploading+queued+running); 0 = unlimited
    unsigned seed = 1;

    std::string out_prefix = "results/run";
    std::string label;
    bool color_vehicles = false;  // recolour vehicles in sumo-gui
    bool log_positions = false;
    bool log_decisions = false;
    bool light = false;           // only dags + summary output
};

const char* kUsage =
    "Usage: iov_sim (--mock | --port N) [options]\n"
    "  --port N             TraCI port of a running `sumo --remote-port N`\n"
    "  --host H             TraCI host (default 127.0.0.1)\n"
    "  --mock               synthetic mobility instead of SUMO\n"
    "  --num-vehicles N     vehicles for --mock (default 40)\n"
    "  --scheduler S        heft | fifo (default heft)\n"
    "  --offloading P       eft | nearest | least_loaded | ml | random (default eft)\n"
    "  --workload W         synthetic | wfcommons (default synthetic)\n"
    "  --dag-file F         DAG csv for --workload wfcommons (default data/wfcommons_dags.csv)\n"
    "  --ml-model F         model file for --offloading ml (default ml/offload_mlp.txt)\n"
    "  --load U             offered load: work arriving / server capacity (default 0.4)\n"
    "  --arrival-rate R     DAGs per second instead of --load\n"
    "  --server-slots K     max tasks per server, 0 = unlimited (default 0)\n"
    "  --warmup S           seconds before the first DAG (default 60)\n"
    "  --duration S         seconds during which DAGs arrive (default 300)\n"
    "  --drain S            max seconds to wait for DAGs to finish (default 120)\n"
    "  --step-length S      seconds per tick, must match SUMO (default 0.1)\n"
    "  --seed N             random seed (default 1)\n"
    "  --out-prefix P       output file prefix (default results/run)\n"
    "  --label L            name stored in the summary row\n"
    "  --color-vehicles     colour vehicles in sumo-gui by the server they use\n"
    "  --log-positions      also write vehicle positions (for animations)\n"
    "  --log-decisions      also write ML training data (features + real task time)\n"
    "  --light              only write the dags and summary files\n";

Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--host") c.host = next();
        else if (a == "--port") c.port = std::stoi(next());
        else if (a == "--mock") c.use_mock = true;
        else if (a == "--num-vehicles") c.mock_num_vehicles = std::stoi(next());
        else if (a == "--scheduler") c.scheduler = next();
        else if (a == "--offloading") c.offloading = next();
        else if (a == "--workload") c.workload = next();
        else if (a == "--dag-file") c.dag_file = next();
        else if (a == "--ml-model") c.ml_model = next();
        else if (a == "--load") c.load = std::stod(next());
        else if (a == "--arrival-rate") c.arrival_rate = std::stod(next());
        else if (a == "--server-slots") c.server_slots = std::stoi(next());
        else if (a == "--warmup") c.warmup_s = std::stod(next());
        else if (a == "--duration") c.duration_s = std::stod(next());
        else if (a == "--drain") c.drain_s = std::stod(next());
        else if (a == "--step-length") c.step_length_s = std::stod(next());
        else if (a == "--seed") c.seed = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--out-prefix") c.out_prefix = next();
        else if (a == "--label") c.label = next();
        else if (a == "--color-vehicles") c.color_vehicles = true;
        else if (a == "--log-positions") c.log_positions = true;
        else if (a == "--log-decisions") c.log_decisions = true;
        else if (a == "--light") c.light = true;
        else if (a == "--help" || a == "-h") {
            std::cout << kUsage;
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + a + " (try --help)");
        }
    }
    if (!c.use_mock && c.port < 0) throw std::runtime_error("pass either --mock or --port <traci-port>");
    if (c.step_length_s <= 0.0) throw std::runtime_error("--step-length must be > 0");
    if (c.load < 0.0) throw std::runtime_error("--load must be >= 0");
    if (c.label.empty()) c.label = c.scheduler + "_" + c.offloading;
    return c;
}

// ---------------------------------------------------------------- scenario

// Four heterogeneous edge servers (road-side units) near the corners of
// the 600 m x 600 m SUMO grid. Keep in sync with sumo_scenario/servers.add.xml.
struct EdgeServer {
    resource_mgmt::ServerSpec spec;
    std::string name;
    double x, y;
    simulation::Rgb color;  // used in sumo-gui and plots
};

std::vector<EdgeServer> makeServers() {
    return {
        {{0, 2.0}, "S0", 100.0, 100.0, {42, 120, 214}},   // slow
        {{1, 3.0}, "S1", 500.0, 100.0, {235, 104, 52}},  // medium
        {{2, 3.0}, "S2", 100.0, 500.0, {27, 175, 122}},   // medium
        {{3, 6.0}, "S3", 500.0, 500.0, {98, 80, 214}},  // fast
    };
}

std::unique_ptr<scheduling::ITaskScheduler> makeScheduler(const std::string& name,
                                                          double mean_cap, double mean_rate) {
    if (name == "heft") return std::make_unique<scheduling::HeftLiteScheduler>(mean_cap, mean_rate);
    if (name == "fifo") return std::make_unique<scheduling::FifoScheduler>();
    throw std::runtime_error("unknown --scheduler: " + name);
}

std::unique_ptr<offloading::IOffloadingPolicy> makePolicy(const Config& c) {
    if (c.offloading == "eft") return std::make_unique<offloading::EftPolicy>();
    if (c.offloading == "nearest") return std::make_unique<offloading::NearestServerPolicy>();
    if (c.offloading == "least_loaded") return std::make_unique<offloading::LeastLoadedPolicy>();
    if (c.offloading == "ml") return std::make_unique<offloading::MlpPolicy>(c.ml_model);
    if (c.offloading == "random") return std::make_unique<offloading::RandomPolicy>(c.seed * 7919u + 17u);
    throw std::runtime_error("unknown --offloading: " + c.offloading);
}

std::unique_ptr<scheduling::IDagSource> makeWorkload(const Config& c) {
    if (c.workload == "synthetic") return std::make_unique<scheduling::SyntheticDagSource>();
    if (c.workload == "wfcommons") return std::make_unique<scheduling::TraceDagSource>(c.dag_file);
    throw std::runtime_error("unknown --workload: " + c.workload);
}

// ---------------------------------------------------------------- state

struct TaskRecord {
    double ready_s = 0, dispatch_s = 0, data_ready_s = 0, start_s = 0, finish_s = 0;
    uint64_t server = 0;
    std::vector<double> features;  // ML features of the chosen server at dispatch
    double eft_estimate_ms = 0;    // what EFT's formula predicted for the chosen server
};

struct DagState {
    scheduling::Dag dag;
    std::unordered_map<uint64_t, int> deps_left;
    std::unordered_map<uint64_t, std::vector<uint64_t>> children;
    std::unordered_map<uint64_t, TaskRecord> rec;
    size_t done = 0;
    double ideal_s = 0;  // no-queueing lower-bound estimate, for slowdown
    double owner_x = 0, owner_y = 0;  // last known position of the owner vehicle
    bool owner_left = false;          // owner drove out of the network before finishing
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double idx = p * static_cast<double>(v.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx)), hi = static_cast<size_t>(std::ceil(idx));
    return v[lo] + (v[hi] - v[lo]) * (idx - static_cast<double>(lo));
}

double mean(const std::vector<double>& v) {
    return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    try {
        cfg = parseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        return 1;
    }

    std::unique_ptr<simulation::IMobilityProvider> mobility;
    try {
        if (cfg.use_mock) {
            mobility = std::make_unique<simulation::MockMobilityProvider>(cfg.mock_num_vehicles,
                                                                           cfg.step_length_s);
        } else {
            mobility = std::make_unique<simulation::SumoBridge>(cfg.host, cfg.port);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to start mobility: " << e.what() << "\n";
        return 1;
    }

    const auto servers = makeServers();
    std::vector<resource_mgmt::ServerSpec> specs;
    double mean_cap = 0.0;
    for (const auto& s : servers) {
        specs.push_back(s.spec);
        mean_cap += s.spec.cpu_capacity / static_cast<double>(servers.size());
    }
    const simulation::ChannelModel channel;
    const double typical_rate = channel.link(0, 0, 300, 0).rate_kb_per_ms;  // rate at 300 m

    resource_mgmt::FifoResourceAllocator alloc(specs);
    std::unique_ptr<scheduling::ITaskScheduler> scheduler;
    std::unique_ptr<offloading::IOffloadingPolicy> policy;
    std::unique_ptr<scheduling::IDagSource> workload;
    try {
        scheduler = makeScheduler(cfg.scheduler, mean_cap, typical_rate);
        policy = makePolicy(cfg);
        workload = makeWorkload(cfg);
    } catch (const std::exception& e) {
        std::cerr << "Setup error: " << e.what() << "\n";
        return 1;
    }

    // Arrival rate from offered load: load = rate * mean DAG work / total capacity.
    const double total_capacity_per_s = mean_cap * static_cast<double>(servers.size()) * 1000.0;
    const double arrival_rate = cfg.arrival_rate >= 0.0
        ? cfg.arrival_rate
        : cfg.load * total_capacity_per_s / workload->meanDagWorkMcycles();
    const double offered_load = arrival_rate * workload->meanDagWorkMcycles() / total_capacity_per_s;

    std::mt19937 rng(cfg.seed);

    std::map<int, DagState> active;                          // dag_id -> state
    std::unordered_map<uint64_t, int> task_dag;              // task id -> dag id
    std::vector<scheduling::ReadyTask> ready;                // waiting for a server slot
    std::unordered_map<std::string, int> owner_dag_count;    // for vehicle colouring
    uint64_t next_task_id = 0;
    int next_dag_id = 0;

    // Run-level results.
    std::vector<double> makespans, slowdowns, upload_ms, queue_ms, exec_ms, wait_ms;
    std::vector<int> tasks_per_server(servers.size(), 0);
    int dags_arrived = 0, dags_completed = 0, dags_owner_left = 0, arrivals_without_vehicle = 0;

    // ---- output files
    auto openOut = [&](const std::string& suffix) {
        std::ofstream f(cfg.out_prefix + suffix);
        if (!f) throw std::runtime_error("cannot write " + cfg.out_prefix + suffix +
                                         " (does the folder exist?)");
        f << std::setprecision(6);
        return f;
    };
    // Files that are not opened (e.g. tasks/ticks with --light) silently
    // ignore writes.
    std::ofstream tasks_csv, dags_csv, ticks_csv, pos_csv, decisions_csv, servers_csv;
    try {
        dags_csv = openOut("_dags.csv");
        if (!cfg.light) {
            tasks_csv = openOut("_tasks.csv");
            ticks_csv = openOut("_ticks.csv");
            servers_csv = openOut("_servers.csv");
        }
        if (cfg.log_positions) pos_csv = openOut("_positions.csv");
        if (cfg.log_decisions) decisions_csv = openOut("_decisions.csv");
        servers_csv << "server_id,name,x,y,cpu_capacity,r,g,b\n";
        for (const auto& s : servers)
            servers_csv << s.spec.server_id << "," << s.name << "," << s.x << "," << s.y << ","
                        << s.spec.cpu_capacity << "," << int(s.color.r) << "," << int(s.color.g)
                        << "," << int(s.color.b) << "\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    tasks_csv << "task_id,dag_id,server_id,ready_s,dispatch_s,data_ready_s,start_s,finish_s,"
                 "wait_ms,upload_ms,queue_ms,exec_ms,workload_mcycles,data_kb\n";
    dags_csv << "dag_id,owner_vehicle,n_tasks,arrival_s,finish_s,makespan_s,ideal_s,slowdown,"
                "owner_left\n";
    ticks_csv << "time_s,num_vehicles,mean_speed_mps,active_dags,ready_waiting";
    for (const auto& s : servers) ticks_csv << "," << s.name << "_tasks," << s.name << "_backlog_ms";
    ticks_csv << ",dags_completed\n";
    if (cfg.log_positions) pos_csv << "time_s,vehicle,x,y,speed,server_id\n";
    decisions_csv << "distance_m,data_kb,workload_mcycles,cpu_capacity,pending_work_mcycles,"
                     "tasks_in_system,actual_ms,eft_estimate_ms\n";

    // Task ids inside a DAG are consecutive, so a task is found by offset.
    auto taskOf = [](const DagState& ds, uint64_t task_id) -> const scheduling::Task& {
        return ds.dag.tasks[static_cast<size_t>(task_id - ds.dag.tasks.front().id)];
    };

    // Owner colour = server of its most recent dispatched task.
    std::unordered_map<std::string, int> owner_server;
    const simulation::Rgb kDefaultColor{255, 255, 0};  // SUMO's default vehicle colour

    const double arrivals_end = cfg.warmup_s + cfg.duration_s;
    const double hard_end = arrivals_end + cfg.drain_s;
    const long max_steps = std::lround(hard_end / cfg.step_length_s);
    std::poisson_distribution<int> arrivals_per_tick(arrival_rate * cfg.step_length_s);
    const int pos_every = std::max(1, static_cast<int>(std::lround(0.5 / cfg.step_length_s)));

    std::cout << "iov_sim: " << (cfg.use_mock ? "mock mobility" : "SUMO via TraCI")
              << ", workload=" << workload->name() << ", scheduler=" << scheduler->name()
              << ", offloading=" << policy->name() << ", load=" << offered_load << " ("
              << arrival_rate << " DAG/s), seed=" << cfg.seed << "\n";

    double now_s = 0.0;
    try {
        for (long step = 1; step <= max_steps; ++step) {
            mobility->step();
            now_s = static_cast<double>(step) * cfg.step_length_s;
            const auto& veh = mobility->vehicles();

            // Track owner positions (keep the last known one if the vehicle left).
            for (auto& [id, ds] : active) {
                auto it = veh.find(ds.dag.owner_vehicle);
                if (it != veh.end()) {
                    ds.owner_x = it->second.x;
                    ds.owner_y = it->second.y;
                } else {
                    ds.owner_left = true;
                }
            }

            // ---- 2. servers run until now; handle completions
            for (const auto& c : alloc.advanceTo(now_s * 1000.0)) {
                int dag_id = task_dag.at(c.task_id);
                DagState& ds = active.at(dag_id);
                TaskRecord& r = ds.rec[c.task_id];
                r.start_s = c.start_ms / 1000.0;
                r.finish_s = c.finish_ms / 1000.0;
                const auto& t = taskOf(ds, c.task_id);
                double w = (r.dispatch_s - r.ready_s) * 1000.0, u = (r.data_ready_s - r.dispatch_s) * 1000.0,
                       q = (r.start_s - r.data_ready_s) * 1000.0, e = (r.finish_s - r.start_s) * 1000.0;
                wait_ms.push_back(w);
                upload_ms.push_back(u);
                queue_ms.push_back(q);
                exec_ms.push_back(e);
                tasks_csv << c.task_id << "," << dag_id << "," << r.server << "," << r.ready_s << ","
                          << r.dispatch_s << "," << r.data_ready_s << "," << r.start_s << ","
                          << r.finish_s << "," << w << "," << u << "," << q << "," << e << ","
                          << t.workload_mcycles << "," << t.data_kb << "\n";
                if (cfg.log_decisions) {
                    for (double f : r.features) decisions_csv << f << ",";
                    decisions_csv << (r.finish_s - r.dispatch_s) * 1000.0 << "," << r.eft_estimate_ms << "\n";
                }

                for (auto child : ds.children[c.task_id])
                    if (--ds.deps_left[child] == 0) ready.push_back({child, dag_id, r.finish_s});

                if (++ds.done == ds.dag.tasks.size()) {
                    double fin = 0.0;
                    for (const auto& [tid, tr] : ds.rec) fin = std::max(fin, tr.finish_s);
                    double ms = fin - ds.dag.arrival_s;
                    makespans.push_back(ms);
                    slowdowns.push_back(ms / ds.ideal_s);
                    ++dags_completed;
                    if (ds.owner_left) ++dags_owner_left;
                    dags_csv << dag_id << "," << ds.dag.owner_vehicle << "," << ds.dag.tasks.size()
                             << "," << ds.dag.arrival_s << "," << fin << "," << ms << ","
                             << ds.ideal_s << "," << ms / ds.ideal_s << "," << ds.owner_left << "\n";
                    scheduler->onDagFinished(ds.dag);
                    const std::string owner = ds.dag.owner_vehicle;
                    for (const auto& tk : ds.dag.tasks) task_dag.erase(tk.id);
                    active.erase(dag_id);
                    if (--owner_dag_count[owner] == 0) {
                        owner_dag_count.erase(owner);
                        owner_server.erase(owner);
                        if (cfg.color_vehicles) mobility->setVehicleColor(owner, kDefaultColor);
                    }
                }
            }

            // ---- 3. new DAG arrivals
            if (now_s > cfg.warmup_s && now_s <= arrivals_end + 1e-9) {
                int n_new = arrivals_per_tick(rng);
                for (int k = 0; k < n_new; ++k) {
                    ++dags_arrived;
                    if (veh.empty()) {
                        ++arrivals_without_vehicle;
                        continue;
                    }
                    std::vector<std::string> ids;
                    ids.reserve(veh.size());
                    for (const auto& kv : veh) ids.push_back(kv.first);
                    std::sort(ids.begin(), ids.end());  // deterministic choice
                    std::uniform_int_distribution<size_t> pick(0, ids.size() - 1);
                    const std::string owner = ids[pick(rng)];

                    DagState ds;
                    ds.dag = workload->make(next_dag_id, now_s, owner, next_task_id, rng);
                    ds.owner_x = veh.at(owner).x;
                    ds.owner_y = veh.at(owner).y;

                    // ideal: longest path where each task gets its best server with no queueing
                    std::unordered_map<uint64_t, double> earliest_finish;
                    for (const auto& t : ds.dag.tasks) {
                        double start = 0.0;
                        for (auto d : t.dependencies) start = std::max(start, earliest_finish[d]);
                        double best = 1e300;
                        for (const auto& s : servers) {
                            auto link = channel.link(ds.owner_x, ds.owner_y, s.x, s.y);
                            best = std::min(best, link.uploadMs(t.data_kb) +
                                                      t.workload_mcycles / s.spec.cpu_capacity);
                        }
                        earliest_finish[t.id] = start + best / 1000.0;
                        ds.ideal_s = std::max(ds.ideal_s, earliest_finish[t.id]);
                    }
                    for (const auto& t : ds.dag.tasks) {
                        task_dag[t.id] = ds.dag.id;
                        ds.deps_left[t.id] = static_cast<int>(t.dependencies.size());
                        for (auto d : t.dependencies) ds.children[d].push_back(t.id);
                        if (t.dependencies.empty()) ready.push_back({t.id, ds.dag.id, now_s});
                    }
                    scheduler->onDagArrival(ds.dag);
                    ++owner_dag_count[owner];
                    active.emplace(ds.dag.id, std::move(ds));
                    ++next_dag_id;
                }
            }

            // ---- 4. dispatch
            if (!ready.empty()) {
                auto order = scheduler->order(std::move(ready));
                ready.clear();
                auto state = alloc.state();  // updated locally as we admit
                for (const auto& rt : order) {
                    DagState& ds = active.at(rt.dag_id);
                    std::vector<offloading::Candidate> cands;
                    for (size_t i = 0; i < servers.size(); ++i) {
                        if (cfg.server_slots > 0 && state[i].tasks_in_system >= cfg.server_slots) continue;
                        cands.push_back({state[i],
                                         channel.link(ds.owner_x, ds.owner_y, servers[i].x, servers[i].y),
                                         std::hypot(ds.owner_x - servers[i].x, ds.owner_y - servers[i].y)});
                    }
                    const auto& task = taskOf(ds, rt.task_id);
                    std::optional<uint64_t> choice;
                    if (!cands.empty()) choice = policy->choose(task, cands);
                    if (!choice) {
                        ready.push_back(rt);  // try again next tick
                        continue;
                    }
                    size_t si = static_cast<size_t>(*choice);  // server ids are 0..N-1
                    const auto& chosen = *std::find_if(cands.begin(), cands.end(), [&](const offloading::Candidate& c) {
                        return c.server.server_id == *choice;
                    });
                    double data_ready_ms = now_s * 1000.0 + chosen.link.uploadMs(task.data_kb);
                    alloc.admit(*choice, {task.id, task.workload_mcycles, data_ready_ms});
                    state[si].tasks_in_system += 1;
                    state[si].pending_work_mcycles += task.workload_mcycles;
                    ++tasks_per_server[si];

                    TaskRecord& r = ds.rec[task.id];
                    r.ready_s = rt.ready_time_s;
                    r.dispatch_s = now_s;
                    r.data_ready_s = data_ready_ms / 1000.0;
                    r.server = *choice;
                    if (cfg.log_decisions) {
                        r.features = offloading::mlFeatures(task, chosen);
                        r.eft_estimate_ms = offloading::estimatedFinishMs(task, chosen);
                    }

                    const std::string& owner = ds.dag.owner_vehicle;
                    auto prev = owner_server.find(owner);
                    if (prev == owner_server.end() || prev->second != static_cast<int>(si)) {
                        owner_server[owner] = static_cast<int>(si);
                        if (cfg.color_vehicles) mobility->setVehicleColor(owner, servers[si].color);
                    }
                }
            }

            // ---- 5. per-tick log
            if (!cfg.light) {
                double speed = 0.0;
                for (const auto& kv : veh) speed += kv.second.speed;
                if (!veh.empty()) speed /= static_cast<double>(veh.size());
                ticks_csv << now_s << "," << veh.size() << "," << speed << "," << active.size() << ","
                          << ready.size();
                for (const auto& s : alloc.state())
                    ticks_csv << "," << s.tasks_in_system << "," << s.pending_work_mcycles / s.cpu_capacity;
                ticks_csv << "," << dags_completed << "\n";
            }

            if (cfg.log_positions && step % pos_every == 0) {
                for (const auto& [id, v] : veh) {
                    auto os = owner_server.find(id);
                    pos_csv << now_s << "," << id << "," << v.x << "," << v.y << "," << v.speed << ","
                            << (os == owner_server.end() ? -1 : os->second) << "\n";
                }
            }

            if (now_s >= arrivals_end && active.empty()) break;  // everything finished
        }
    } catch (const std::exception& e) {
        std::cerr << "Simulation stopped at t=" << now_s << " s: " << e.what() << "\n";
        return 1;
    }

    // ---- summary
    const int unfinished = static_cast<int>(active.size());
    // Utilisation is measured from the first possible arrival (end of warm-up) to the end.
    const double measured_ms = std::max(0.0, now_s - cfg.warmup_s) * 1000.0;
    std::ofstream summary(cfg.out_prefix + "_summary.csv");
    summary << std::setprecision(6);
    summary << "label,mobility,workload,scheduler,offloading,seed,load,arrival_rate,server_slots,duration_s,"
               "dags_arrived,dags_completed,dags_unfinished,arrivals_without_vehicle,dags_owner_left,"
               "mean_makespan_s,median_makespan_s,p95_makespan_s,mean_slowdown,p95_slowdown,"
               "mean_wait_ms,mean_upload_ms,mean_queue_ms,mean_exec_ms,sim_end_s";
    for (const auto& s : servers) summary << "," << s.name << "_util," << s.name << "_task_share";
    summary << "\n";
    int total_tasks = std::accumulate(tasks_per_server.begin(), tasks_per_server.end(), 0);
    summary << cfg.label << "," << (cfg.use_mock ? "mock" : "sumo") << "," << workload->name() << ","
            << scheduler->name() << "," << policy->name() << "," << cfg.seed << ","
            << std::round(offered_load * 1000.0) / 1000.0 << "," << arrival_rate << "," << cfg.server_slots
            << "," << cfg.duration_s << "," << dags_arrived << "," << dags_completed << ","
            << unfinished << "," << arrivals_without_vehicle << "," << dags_owner_left << ","
            << mean(makespans) << "," << percentile(makespans, 0.5) << ","
            << percentile(makespans, 0.95) << "," << mean(slowdowns) << ","
            << percentile(slowdowns, 0.95) << "," << mean(wait_ms) << "," << mean(upload_ms) << ","
            << mean(queue_ms) << "," << mean(exec_ms) << "," << now_s;
    for (size_t i = 0; i < servers.size(); ++i) {
        double util = measured_ms > 0 ? alloc.busyMs(servers[i].spec.server_id) / measured_ms : 0.0;
        double share = total_tasks > 0 ? static_cast<double>(tasks_per_server[i]) / total_tasks : 0.0;
        summary << "," << util << "," << share;
    }
    summary << "\n";

    std::cout << "done at t=" << now_s << " s: " << dags_completed << "/" << dags_arrived
              << " DAGs completed (" << unfinished << " unfinished), mean makespan "
              << mean(makespans) << " s, p95 " << percentile(makespans, 0.95) << " s\n";
    return 0;
}
