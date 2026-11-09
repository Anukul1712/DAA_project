// iov_sim: online, queue-aware DAG scheduling + task offloading + resource
// allocation for Internet-of-Vehicles edge computing, driven by SUMO.
//
// Every tick (default 0.1 s):
//   1. Advance mobility (real SUMO via TraCI, or the --mock feed).
//   2. Run the edge servers forward to "now"; collect finished tasks.
//      Children whose dependencies are now all done become "ready".
//   3. New DAGs arrive (Poisson process), each owned by a random vehicle.
//   4. Dispatch: the scheduler gives every ready task a priority and orders
//      them; each task, in that order, is offered to the servers with a free
//      admission slot, and the offloading policy picks one.
//   5. Log metrics.
//
// Three decisions, three modules:
//   scheduling        --scheduler     fifo | heft | srpt         (priority + dispatch order)
//   server selection  --offloading    eft | least_loaded | nearest | ml | random
//   resource alloc.   --server-queue  fifo | priority            (which queued task gets the CPU)
//
// A task's life: ready -> (wait for decision tick) -> dispatched -> upload over
// the vehicle's current link -> queue at the server -> execute -> done.
// A DAG's makespan = last task finish - DAG arrival.
//
// Outputs (prefix set by --out-prefix):
//   <prefix>_dags.csv      one row per DAG (makespan, slowdown, ...)
//   <prefix>_summary.csv   one-row summary of the run
//   <prefix>_tasks.csv     one row per finished task (not with --light)
//   <prefix>_ticks.csv     one row per tick (not with --light)
//   <prefix>_servers.csv   server layout (not with --light)
//   <prefix>_positions.csv vehicle positions every 0.5 s (--log-positions)
//   <prefix>_decisions.csv ML features + real task time per task (--log-decisions)
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
#include "src/scheduling/fifo_scheduler.h"
#include "src/scheduling/heft_lite_scheduler.h"
#include "src/scheduling/srpt_dag_scheduler.h"
#include "src/scheduling/task_scheduler.h"
#include "src/scheduling/workload.h"

using namespace iov;

namespace {

// ---------------------------------------------------------------- config

struct Config {
    std::string host = "127.0.0.1";
    int port = -1;
    bool use_mock = false;
    int mock_num_vehicles = 300;

    std::string servers_file = "sumo_scenario/city/servers.csv";
    std::string scheduler = "fifo";      // fifo | heft | srpt
    std::string offloading = "eft";      // eft | nearest | least_loaded | ml | random
    std::string server_queue = "fifo";   // fifo | priority
    std::string workload = "synthetic";  // synthetic | wfcommons
    std::string dag_file = "data/wfcommons_dags.csv";
    std::string ml_model = "ml/offload_mlp.txt";

    double step_length_s = 0.1;  // must match SUMO's --step-length
    double warmup_s = 100.0;     // no DAGs before this (lets traffic build up)
    double duration_s = 300.0;   // DAGs arrive during (warmup, warmup + duration]
    double drain_s = 120.0;      // then wait at most this long for DAGs to finish
    double load = 0.4;           // offered load = arrival rate x mean DAG work / total capacity
    double arrival_rate = -1.0;  // DAGs per second; if set, overrides --load
    // Backlog a task sees under priority queues. all: all pending work (conservative, default);
    // ahead: only work with higher priority (optimistic: ignores later arrivals that overtake).
    std::string backlog_view = "all";
    int server_slots = 0;        // max tasks per server (uploading+queued+running); 0 = unlimited
    unsigned seed = 1;

    std::string out_prefix = "results/run";
    std::string label;
    bool color_vehicles = false;
    bool log_positions = false;
    bool log_decisions = false;
    bool light = false;
};

const char* kUsage =
    "Usage: iov_sim (--mock | --port N) [options]\n"
    "  --port N             TraCI port of a running `sumo --remote-port N`\n"
    "  --host H             TraCI host (default 127.0.0.1)\n"
    "  --mock               synthetic mobility instead of SUMO\n"
    "  --num-vehicles N     vehicles for --mock (default 300)\n"
    "  --servers F          edge-server layout csv (default sumo_scenario/city/servers.csv)\n"
    "  --scheduler S        fifo | heft | srpt (default fifo)\n"
    "  --offloading P       eft | nearest | least_loaded | ml | random (default eft)\n"
    "  --server-queue Q     fifo | priority: how a server picks its next task (default fifo)\n"
    "  --workload W         synthetic | wfcommons (default synthetic)\n"
    "  --dag-file F         DAG csv for --workload wfcommons (default data/wfcommons_dags.csv)\n"
    "  --ml-model F         model file for --offloading ml (default ml/offload_mlp.txt)\n"
    "  --load U             offered load: work arriving / total server capacity (default 0.4)\n"
    "  --arrival-rate R     DAGs per second instead of --load\n"
    "  --backlog-view V     all | ahead: backlog a task sees under priority queues (default all)\n"
    "  --server-slots K     max tasks per server, 0 = unlimited (default 0)\n"
    "  --warmup S           seconds before the first DAG (default 100)\n"
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
        else if (a == "--servers") c.servers_file = next();
        else if (a == "--scheduler") c.scheduler = next();
        else if (a == "--offloading") c.offloading = next();
        else if (a == "--server-queue") c.server_queue = next();
        else if (a == "--workload") c.workload = next();
        else if (a == "--dag-file") c.dag_file = next();
        else if (a == "--ml-model") c.ml_model = next();
        else if (a == "--load") c.load = std::stod(next());
        else if (a == "--arrival-rate") c.arrival_rate = std::stod(next());
        else if (a == "--backlog-view") c.backlog_view = next();
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
    if (c.backlog_view != "all" && c.backlog_view != "ahead") throw std::runtime_error("--backlog-view must be all or ahead");
    if (c.label.empty()) c.label = c.scheduler + "_" + c.offloading + "_" + c.server_queue;
    return c;
}

// ---------------------------------------------------------------- scenario

struct EdgeServer {
    resource_mgmt::ServerSpec spec;
    std::string name;
    double x, y;
    simulation::Rgb color;
};

// Reads the server layout written by sumo_scenario/make_scenario.py.
// Server ids must be 0..N-1 in order.
std::vector<EdgeServer> loadServers(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open server file " + path);
    std::string line;
    std::getline(in, line);  // header
    std::vector<EdgeServer> out;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string f[8];
        for (auto& x : f) std::getline(ss, x, ',');
        EdgeServer s;
        s.spec.server_id = std::stoull(f[0]);
        if (s.spec.server_id != out.size()) throw std::runtime_error("server ids must be 0..N-1 in " + path);
        s.name = f[1];
        s.x = std::stod(f[2]);
        s.y = std::stod(f[3]);
        s.spec.cpu_capacity = std::stod(f[4]);
        s.color = {static_cast<uint8_t>(std::stoi(f[5])), static_cast<uint8_t>(std::stoi(f[6])),
                   static_cast<uint8_t>(std::stoi(f[7]))};
        out.push_back(s);
    }
    if (out.empty()) throw std::runtime_error(path + " has no servers");
    return out;
}

std::unique_ptr<scheduling::ITaskScheduler> makeScheduler(const std::string& name, double mean_cap,
                                                          double mean_rate) {
    if (name == "heft") return std::make_unique<scheduling::HeftLiteScheduler>(mean_cap, mean_rate);
    if (name == "srpt") return std::make_unique<scheduling::SrptDagScheduler>(mean_cap, mean_rate);
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
    double ideal_s = 0;
    double owner_x = 0, owner_y = 0;
    bool owner_left = false;
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
    std::vector<EdgeServer> servers;
    try {
        cfg = parseArgs(argc, argv);
        servers = loadServers(cfg.servers_file);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        return 1;
    }

    double area = 0.0;
    for (const auto& s : servers) area = std::max({area, s.x, s.y});
    std::unique_ptr<simulation::IMobilityProvider> mobility;
    try {
        if (cfg.use_mock) {
            mobility = std::make_unique<simulation::MockMobilityProvider>(cfg.mock_num_vehicles, cfg.step_length_s,
                                                                           area * 8.0 / 7.0);
        } else {
            mobility = std::make_unique<simulation::SumoBridge>(cfg.host, cfg.port);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to start mobility: " << e.what() << "\n";
        return 1;
    }

    std::vector<resource_mgmt::ServerSpec> specs;
    double total_cap = 0.0;
    for (const auto& s : servers) {
        specs.push_back(s.spec);
        total_cap += s.spec.cpu_capacity;
    }
    const double mean_cap = total_cap / static_cast<double>(servers.size());
    const simulation::ChannelModel channel;
    const double typical_rate = channel.link(0, 0, 300, 0).rate_kb_per_ms;

    std::unique_ptr<resource_mgmt::QueueResourceAllocator> alloc;
    std::unique_ptr<scheduling::ITaskScheduler> scheduler;
    std::unique_ptr<offloading::IOffloadingPolicy> policy;
    std::unique_ptr<scheduling::IDagSource> workload;
    try {
        alloc = std::make_unique<resource_mgmt::QueueResourceAllocator>(
            specs, resource_mgmt::parseDiscipline(cfg.server_queue));
        scheduler = makeScheduler(cfg.scheduler, mean_cap, typical_rate);
        policy = makePolicy(cfg);
        workload = makeWorkload(cfg);
    } catch (const std::exception& e) {
        std::cerr << "Setup error: " << e.what() << "\n";
        return 1;
    }

    // Offered load rho = lambda * E[DAG work] / total capacity  =>  lambda.
    const double total_capacity_per_s = total_cap * 1000.0;
    const double arrival_rate = cfg.arrival_rate >= 0.0
        ? cfg.arrival_rate
        : cfg.load * total_capacity_per_s / workload->meanDagWorkMcycles();
    const double offered_load = arrival_rate * workload->meanDagWorkMcycles() / total_capacity_per_s;

    std::mt19937 rng(cfg.seed);
    std::map<int, DagState> active;
    std::unordered_map<uint64_t, int> task_dag;
    std::vector<scheduling::ReadyTask> ready;
    std::unordered_map<std::string, int> owner_dag_count;
    std::unordered_map<std::string, int> owner_server;
    uint64_t next_task_id = 0;
    int next_dag_id = 0;

    std::vector<double> makespans, slowdowns, upload_ms, queue_ms, exec_ms, wait_ms;
    std::vector<int> tasks_per_server(servers.size(), 0);
    long tasks_dispatched = 0, ticks = 0;
    int dags_arrived = 0, dags_completed = 0, dags_owner_left = 0, arrivals_without_vehicle = 0;
    double vehicles_sum = 0.0;
    size_t vehicles_max = 0;

    auto openOut = [&](const std::string& suffix) {
        std::ofstream f(cfg.out_prefix + suffix);
        if (!f) throw std::runtime_error("cannot write " + cfg.out_prefix + suffix + " (does the folder exist?)");
        f << std::setprecision(6);
        return f;
    };
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
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    servers_csv << "server_id,name,x,y,cpu_capacity,r,g,b\n";
    for (const auto& s : servers)
        servers_csv << s.spec.server_id << "," << s.name << "," << s.x << "," << s.y << "," << s.spec.cpu_capacity
                    << "," << int(s.color.r) << "," << int(s.color.g) << "," << int(s.color.b) << "\n";
    tasks_csv << "task_id,dag_id,server_id,ready_s,dispatch_s,data_ready_s,start_s,finish_s,"
                 "wait_ms,upload_ms,queue_ms,exec_ms,workload_mcycles,data_kb\n";
    dags_csv << "dag_id,owner_vehicle,n_tasks,total_work_mcycles,arrival_s,finish_s,makespan_s,ideal_s,slowdown,"
                "owner_left\n";
    ticks_csv << "time_s,num_vehicles,mean_speed_mps,active_dags,ready_waiting,total_backlog_ms,max_backlog_ms,"
                 "dags_completed\n";
    if (cfg.log_positions) pos_csv << "time_s,vehicle,x,y,speed,server_id\n";
    decisions_csv << "distance_m,data_kb,workload_mcycles,cpu_capacity,pending_work_mcycles,"
                     "tasks_in_system,actual_ms,eft_estimate_ms\n";

    auto taskOf = [](const DagState& ds, uint64_t task_id) -> const scheduling::Task& {
        return ds.dag.tasks[static_cast<size_t>(task_id - ds.dag.tasks.front().id)];
    };

    const simulation::Rgb kDefaultColor{255, 255, 0};
    const double arrivals_end = cfg.warmup_s + cfg.duration_s;
    const long max_steps = std::lround((arrivals_end + cfg.drain_s) / cfg.step_length_s);
    std::poisson_distribution<int> arrivals_per_tick(arrival_rate * cfg.step_length_s);
    const int pos_every = std::max(1, static_cast<int>(std::lround(0.5 / cfg.step_length_s)));

    std::cout << "iov_sim: " << (cfg.use_mock ? "mock mobility" : "SUMO via TraCI") << ", " << servers.size()
              << " servers, workload=" << workload->name() << ", scheduler=" << scheduler->name()
              << ", offloading=" << policy->name() << ", server-queue=" << cfg.server_queue
              << ", load=" << offered_load << " (" << arrival_rate << " DAG/s), seed=" << cfg.seed << "\n";

    double now_s = 0.0;
    try {
        for (long step = 1; step <= max_steps; ++step) {
            mobility->step();
            now_s = static_cast<double>(step) * cfg.step_length_s;
            ++ticks;
            const auto& veh = mobility->vehicles();
            if (now_s > cfg.warmup_s && now_s <= arrivals_end) {
                vehicles_sum += static_cast<double>(veh.size());
                vehicles_max = std::max(vehicles_max, veh.size());
            }

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
            for (const auto& c : alloc->advanceTo(now_s * 1000.0)) {
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
                          << r.dispatch_s << "," << r.data_ready_s << "," << r.start_s << "," << r.finish_s << ","
                          << w << "," << u << "," << q << "," << e << "," << t.workload_mcycles << ","
                          << t.data_kb << "\n";
                if (cfg.log_decisions) {
                    for (double f : r.features) decisions_csv << f << ",";
                    decisions_csv << (r.finish_s - r.dispatch_s) * 1000.0 << "," << r.eft_estimate_ms << "\n";
                }
                scheduler->onTaskFinished(dag_id, t);

                for (auto child : ds.children[c.task_id])
                    if (--ds.deps_left[child] == 0) ready.push_back({child, dag_id, r.finish_s});

                if (++ds.done == ds.dag.tasks.size()) {
                    double fin = 0.0, work = 0.0;
                    for (const auto& [tid, tr] : ds.rec) fin = std::max(fin, tr.finish_s);
                    for (const auto& tk : ds.dag.tasks) work += tk.workload_mcycles;
                    double ms = fin - ds.dag.arrival_s;
                    makespans.push_back(ms);
                    slowdowns.push_back(ms / ds.ideal_s);
                    ++dags_completed;
                    if (ds.owner_left) ++dags_owner_left;
                    dags_csv << dag_id << "," << ds.dag.owner_vehicle << "," << ds.dag.tasks.size() << "," << work
                             << "," << ds.dag.arrival_s << "," << fin << "," << ms << "," << ds.ideal_s << ","
                             << ms / ds.ideal_s << "," << ds.owner_left << "\n";
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
                std::vector<std::string> ids;
                if (n_new > 0) {
                    ids.reserve(veh.size());
                    for (const auto& kv : veh) ids.push_back(kv.first);
                    std::sort(ids.begin(), ids.end());  // deterministic choice
                }
                for (int k = 0; k < n_new; ++k) {
                    ++dags_arrived;
                    if (ids.empty()) {
                        ++arrivals_without_vehicle;
                        continue;
                    }
                    std::uniform_int_distribution<size_t> pick(0, ids.size() - 1);
                    const std::string owner = ids[pick(rng)];

                    DagState ds;
                    ds.dag = workload->make(next_dag_id, now_s, owner, next_task_id, rng);
                    ds.owner_x = veh.at(owner).x;
                    ds.owner_y = veh.at(owner).y;
                    // Ideal (contention-free) makespan: longest path where every task
                    // gets its best server with no queueing.
                    std::unordered_map<uint64_t, double> earliest_finish;
                    for (const auto& t : ds.dag.tasks) {
                        double start = 0.0;
                        for (auto d : t.dependencies) start = std::max(start, earliest_finish[d]);
                        double best = 1e300;
                        for (const auto& s : servers) {
                            auto link = channel.link(ds.owner_x, ds.owner_y, s.x, s.y);
                            best = std::min(best, link.uploadMs(t.data_kb) + t.workload_mcycles / s.spec.cpu_capacity);
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

            // ---- 4. dispatch: priority order, then server choice per task
            if (!ready.empty()) {
                auto order = scheduler->order(std::move(ready));
                ready.clear();
                for (const auto& rt : order) {
                    DagState& ds = active.at(rt.dag_id);
                    const auto& task = taskOf(ds, rt.task_id);
                    const double prio = scheduler->priority(rt);
                    // Server states as seen by THIS task: with the priority discipline,
                    // only work that would be served before it counts as backlog.
                    auto state = cfg.backlog_view == "all" ? alloc->state() : alloc->stateFor(prio);
                    std::vector<offloading::Candidate> cands;
                    for (size_t i = 0; i < servers.size(); ++i) {
                        if (cfg.server_slots > 0 && state[i].tasks_in_system >= cfg.server_slots) continue;
                        cands.push_back({state[i], channel.link(ds.owner_x, ds.owner_y, servers[i].x, servers[i].y),
                                         std::hypot(ds.owner_x - servers[i].x, ds.owner_y - servers[i].y)});
                    }
                    std::optional<uint64_t> choice;
                    if (!cands.empty()) choice = policy->choose(task, cands);
                    if (!choice) {
                        ready.push_back(rt);  // try again next tick
                        continue;
                    }
                    size_t si = static_cast<size_t>(*choice);
                    const auto& chosen = *std::find_if(cands.begin(), cands.end(),
                                                       [&](const offloading::Candidate& c) { return c.server.server_id == *choice; });
                    double data_ready_ms = now_s * 1000.0 + chosen.link.uploadMs(task.data_kb);
                    alloc->admit(*choice, {task.id, task.workload_mcycles, data_ready_ms, prio});
                    ++tasks_per_server[si];
                    ++tasks_dispatched;

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
                double tot = 0.0, mx = 0.0;
                for (const auto& s : alloc->state()) {
                    double b = s.pending_work_mcycles / s.cpu_capacity;
                    tot += b;
                    mx = std::max(mx, b);
                }
                ticks_csv << now_s << "," << veh.size() << "," << speed << "," << active.size() << ","
                          << ready.size() << "," << tot << "," << mx << "," << dags_completed << "\n";
            }
            if (cfg.log_positions && step % pos_every == 0) {
                for (const auto& [id, v] : veh) {
                    auto os = owner_server.find(id);
                    pos_csv << now_s << "," << id << "," << v.x << "," << v.y << "," << v.speed << ","
                            << (os == owner_server.end() ? -1 : os->second) << "\n";
                }
            }
            if (now_s >= arrivals_end && active.empty()) break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Simulation stopped at t=" << now_s << " s: " << e.what() << "\n";
        return 1;
    }

    // ---- summary
    const int unfinished = static_cast<int>(active.size());
    const double measured_ms = std::max(0.0, now_s - cfg.warmup_s) * 1000.0;
    std::vector<double> utils;
    for (const auto& s : servers) utils.push_back(measured_ms > 0 ? alloc->busyMs(s.spec.server_id) / measured_ms : 0.0);
    double u_sum = std::accumulate(utils.begin(), utils.end(), 0.0), u_sq = 0.0;
    for (double u : utils) u_sq += u * u;
    const double jain = u_sq > 0 ? u_sum * u_sum / (static_cast<double>(utils.size()) * u_sq) : 0.0;

    std::ofstream summary(cfg.out_prefix + "_summary.csv");
    summary << std::setprecision(6);
    summary << "label,mobility,workload,scheduler,offloading,server_queue,backlog_view,seed,load,arrival_rate,server_slots,"
               "n_servers,duration_s,ticks,mean_vehicles,max_vehicles,dags_arrived,dags_completed,dags_unfinished,"
               "arrivals_without_vehicle,dags_owner_left,tasks_dispatched,tasks_completed,"
               "mean_makespan_s,median_makespan_s,p95_makespan_s,p99_makespan_s,mean_slowdown,p95_slowdown,"
               "mean_wait_ms,mean_upload_ms,mean_queue_ms,mean_exec_ms,util_mean,util_max,util_jain,sim_end_s";
    for (const auto& s : servers) summary << "," << s.name << "_util";
    summary << "\n";
    const double win_ticks = cfg.duration_s / cfg.step_length_s;
    summary << cfg.label << "," << (cfg.use_mock ? "mock" : "sumo") << "," << workload->name() << ","
            << scheduler->name() << "," << policy->name() << "," << cfg.server_queue << "," << cfg.backlog_view
            << "," << cfg.seed << ","
            << std::round(offered_load * 1000.0) / 1000.0 << "," << arrival_rate << "," << cfg.server_slots << ","
            << servers.size() << "," << cfg.duration_s << "," << ticks << "," << vehicles_sum / win_ticks << ","
            << vehicles_max << "," << dags_arrived << "," << dags_completed << "," << unfinished << ","
            << arrivals_without_vehicle << "," << dags_owner_left << "," << tasks_dispatched << ","
            << wait_ms.size() << "," << mean(makespans) << "," << percentile(makespans, 0.5) << ","
            << percentile(makespans, 0.95) << "," << percentile(makespans, 0.99) << "," << mean(slowdowns) << ","
            << percentile(slowdowns, 0.95) << "," << mean(wait_ms) << "," << mean(upload_ms) << ","
            << mean(queue_ms) << "," << mean(exec_ms) << "," << mean(utils) << ","
            << *std::max_element(utils.begin(), utils.end()) << "," << jain << "," << now_s;
    for (double u : utils) summary << "," << u;
    summary << "\n";

    std::cout << "done at t=" << now_s << " s: " << dags_completed << "/" << dags_arrived << " DAGs, "
              << wait_ms.size() << " tasks completed (" << unfinished << " DAGs unfinished), mean makespan "
              << mean(makespans) << " s, p95 " << percentile(makespans, 0.95) << " s\n";
    return 0;
}
