#pragma once
#include <algorithm>
#include <deque>
#include <map>
#include <stdexcept>
#include <vector>
#include "resource_allocator.h"

namespace iov::resource_mgmt {

// One FIFO queue per server, simulated exactly (event by event) rather
// than in whole ticks:
//   - A job is "uploading" until its data_ready_ms, then joins the queue.
//   - Queued jobs run one at a time in the order their data arrived.
//   - Work is done at cpu_capacity mega-cycles per ms. When a job
//     finishes mid-tick, the rest of the tick goes to the next job.
//   - Finish times are exact, so results are not rounded to tick length.
class FifoResourceAllocator : public IResourceAllocator {
public:
    explicit FifoResourceAllocator(const std::vector<ServerSpec>& specs) {
        for (const auto& s : specs) {
            if (s.cpu_capacity <= 0.0) throw std::invalid_argument("cpu_capacity must be > 0");
            servers_[s.server_id].spec = s;
        }
    }

    void admit(uint64_t server_id, const Job& job) override {
        auto& srv = servers_.at(server_id);
        // Keep uploads sorted by arrival time so they enter the queue in order.
        auto pos = std::upper_bound(srv.uploading.begin(), srv.uploading.end(), job,
                                    [](const Job& a, const Job& b) {
                                        return a.data_ready_ms < b.data_ready_ms;
                                    });
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
        for (const auto& [id, srv] : servers_) {
            double pending = 0.0;
            for (const auto& j : srv.uploading) pending += j.workload_mcycles;
            for (const auto& q : srv.queue) pending += q.remaining;
            out.push_back({id, srv.spec.cpu_capacity,
                           static_cast<int>(srv.uploading.size() + srv.queue.size()), pending});
        }
        return out;
    }

    // Total time each server spent executing (for utilisation).
    double busyMs(uint64_t server_id) const { return servers_.at(server_id).busy_ms; }

private:
    struct Running {
        Job job;
        double remaining;
        double start_ms = -1.0;  // -1 = not started yet
    };
    struct Server {
        ServerSpec spec;
        std::vector<Job> uploading;  // sorted by data_ready_ms
        std::deque<Running> queue;   // data has arrived; front is running
        double clock_ms = 0.0;       // how far this server has been simulated
        double busy_ms = 0.0;
    };

    static void moveArrived(Server& srv, double t) {
        size_t n = 0;
        while (n < srv.uploading.size() && srv.uploading[n].data_ready_ms <= t) {
            const Job& j = srv.uploading[n];
            srv.queue.push_back({j, j.workload_mcycles});
            ++n;
        }
        srv.uploading.erase(srv.uploading.begin(), srv.uploading.begin() + static_cast<long>(n));
    }

    static void runServer(uint64_t id, Server& srv, double end_ms, std::vector<Completion>& done) {
        double t = srv.clock_ms;
        while (t < end_ms) {
            moveArrived(srv, t);
            if (srv.queue.empty()) {
                // Idle: jump to the next upload arrival, or to the end.
                if (srv.uploading.empty() || srv.uploading.front().data_ready_ms >= end_ms) break;
                t = srv.uploading.front().data_ready_ms;
                continue;
            }
            Running& head = srv.queue.front();
            if (head.start_ms < 0.0) head.start_ms = t;
            double finish = t + head.remaining / srv.spec.cpu_capacity;
            if (finish <= end_ms) {
                srv.busy_ms += finish - t;
                done.push_back({head.job.task_id, id, head.start_ms, finish});
                srv.queue.pop_front();
                t = finish;
            } else {
                srv.busy_ms += end_ms - t;
                head.remaining -= (end_ms - t) * srv.spec.cpu_capacity;
                t = end_ms;
            }
        }
        srv.clock_ms = std::max(srv.clock_ms, end_ms);
        moveArrived(srv, srv.clock_ms);
    }

    std::map<uint64_t, Server> servers_;  // ordered -> deterministic iteration
};

}  // namespace iov::resource_mgmt
