#pragma once
#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "resource_allocator.h"

namespace iov::resource_mgmt {

// How a server picks the next task among those whose data has arrived.
enum class Discipline {
    Fifo,      // in order of data arrival
    Priority,  // smallest Job::priority first (ties: data arrival order); non-preemptive
};

inline Discipline parseDiscipline(const std::string& s) {
    if (s == "fifo") return Discipline::Fifo;
    if (s == "priority") return Discipline::Priority;
    throw std::invalid_argument("unknown server queue discipline: " + s + " (fifo | priority)");
}

// One queue per server, simulated exactly (event by event) rather than in
// whole ticks:
//   - A job is "uploading" until its data_ready_ms, then joins the queue.
//   - The server runs one job at a time, non-preemptively. When it becomes
//     free it picks the next job by the discipline (FIFO or priority).
//   - Work is done at cpu_capacity mega-cycles per ms. When a job finishes
//     mid-tick, the rest of the tick goes to the next job.
//   - Finish times are exact, so results are not rounded to tick length.
class QueueResourceAllocator : public IResourceAllocator {
public:
    explicit QueueResourceAllocator(const std::vector<ServerSpec>& specs,
                                    Discipline d = Discipline::Fifo)
        : discipline_(d) {
        for (const auto& s : specs) {
            if (s.cpu_capacity <= 0.0) throw std::invalid_argument("cpu_capacity must be > 0");
            servers_[s.server_id].spec = s;
        }
    }

    void admit(uint64_t server_id, const Job& job) override {
        auto& srv = servers_.at(server_id);
        auto pos = std::upper_bound(srv.uploading.begin(), srv.uploading.end(), job,
                                    [](const Job& a, const Job& b) { return a.data_ready_ms < b.data_ready_ms; });
        srv.uploading.insert(pos, job);
    }

    std::vector<Completion> advanceTo(double now_ms) override {
        std::vector<Completion> done;
        for (auto& [id, srv] : servers_) runServer(id, srv, now_ms, done);
        std::sort(done.begin(), done.end(),
                  [](const Completion& a, const Completion& b) { return a.finish_ms < b.finish_ms; });
        return done;
    }

    std::vector<iov::offloading::ServerState> state() const override {
        std::vector<iov::offloading::ServerState> out;
        for (const auto& [id, srv] : servers_) out.push_back(snapshot(id, srv, nullptr));
        return out;
    }

    std::vector<iov::offloading::ServerState> stateFor(double priority) const override {
        std::vector<iov::offloading::ServerState> out;
        for (const auto& [id, srv] : servers_) out.push_back(snapshot(id, srv, &priority));
        return out;
    }

    double busyMs(uint64_t server_id) const { return servers_.at(server_id).busy_ms; }
    Discipline discipline() const { return discipline_; }

private:
    struct Waiting {
        Job job;
        uint64_t seq;  // arrival order, for FIFO and tie-breaking
    };
    struct Running {
        Job job;
        double remaining;
        double start_ms;
    };
    struct Server {
        ServerSpec spec;
        std::vector<Job> uploading;      // sorted by data_ready_ms
        std::vector<Waiting> queue;      // data has arrived, waiting for the CPU
        std::optional<Running> running;  // job on the CPU
        double clock_ms = 0.0;
        double busy_ms = 0.0;
        uint64_t next_seq = 0;
    };

    // Pending work of one server; if priority is given, only work that a new
    // job with that priority would have to wait for.
    iov::offloading::ServerState snapshot(uint64_t id, const Server& srv, const double* priority) const {
        auto ahead = [&](const Job& j) {
            return priority == nullptr || discipline_ == Discipline::Fifo || j.priority <= *priority;
        };
        double pending = srv.running ? srv.running->remaining : 0.0;
        for (const auto& w : srv.queue)
            if (ahead(w.job)) pending += w.job.workload_mcycles;
        for (const auto& j : srv.uploading)
            if (ahead(j)) pending += j.workload_mcycles;
        int n = static_cast<int>(srv.uploading.size() + srv.queue.size()) + (srv.running ? 1 : 0);
        return {id, srv.spec.cpu_capacity, n, pending};
    }

    static void moveArrived(Server& srv, double t) {
        size_t n = 0;
        while (n < srv.uploading.size() && srv.uploading[n].data_ready_ms <= t) {
            srv.queue.push_back({srv.uploading[n], srv.next_seq++});
            ++n;
        }
        srv.uploading.erase(srv.uploading.begin(), srv.uploading.begin() + static_cast<long>(n));
    }

    size_t pickNext(const Server& srv) const {
        size_t best = 0;
        for (size_t i = 1; i < srv.queue.size(); ++i) {
            const auto& a = srv.queue[i];
            const auto& b = srv.queue[best];
            bool better = discipline_ == Discipline::Fifo
                              ? a.seq < b.seq
                              : (a.job.priority < b.job.priority ||
                                 (a.job.priority == b.job.priority && a.seq < b.seq));
            if (better) best = i;
        }
        return best;
    }

    void runServer(uint64_t id, Server& srv, double end_ms, std::vector<Completion>& done) const {
        double t = srv.clock_ms;
        while (t < end_ms) {
            moveArrived(srv, t);
            if (!srv.running) {
                if (srv.queue.empty()) {
                    if (srv.uploading.empty() || srv.uploading.front().data_ready_ms >= end_ms) break;
                    t = srv.uploading.front().data_ready_ms;  // idle until the next upload lands
                    continue;
                }
                size_t k = pickNext(srv);
                srv.running = Running{srv.queue[k].job, srv.queue[k].job.workload_mcycles, t};
                srv.queue.erase(srv.queue.begin() + static_cast<long>(k));
            }
            Running& r = *srv.running;
            double finish = t + r.remaining / srv.spec.cpu_capacity;
            if (finish <= end_ms) {
                srv.busy_ms += finish - t;
                done.push_back({r.job.task_id, id, r.start_ms, finish});
                srv.running.reset();
                t = finish;
            } else {
                srv.busy_ms += end_ms - t;
                r.remaining -= (end_ms - t) * srv.spec.cpu_capacity;
                t = end_ms;
            }
        }
        srv.clock_ms = std::max(srv.clock_ms, end_ms);
        moveArrived(srv, srv.clock_ms);
    }

    Discipline discipline_;
    std::map<uint64_t, Server> servers_;  // ordered -> deterministic iteration
};

// Backwards-compatible name: FIFO discipline.
class FifoResourceAllocator : public QueueResourceAllocator {
public:
    explicit FifoResourceAllocator(const std::vector<ServerSpec>& specs)
        : QueueResourceAllocator(specs, Discipline::Fifo) {}
};

}  // namespace iov::resource_mgmt
