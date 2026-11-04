// Minimal unit tests, no framework needed. Build target: iov_tests.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "simulation/sumo_integration/channel_model.h"
#include "simulation/sumo_integration/traci_client.h"
#include "src/offloading/eft_policy.h"
#include "src/offloading/least_loaded_policy.h"
#include "src/offloading/mlp_policy.h"
#include "src/offloading/nearest_server_policy.h"
#include "src/resource_mgmt/fifo_resource_allocator.h"
#include "src/scheduling/dag_generator.h"
#include "src/scheduling/fifo_scheduler.h"
#include "src/scheduling/heft_lite_scheduler.h"

using namespace iov;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)
#define CHECK_NEAR(a, b) CHECK(std::fabs((a) - (b)) < 1e-6)

// ---------------------------------------------------------------- TraCI framing

static void testTraciShortCommand() {
    simulation::TraciBuffer b;
    b.putCommand(0x02, std::string(8, '\0'));
    CHECK(b.data().size() == 10);
    CHECK(static_cast<uint8_t>(b.data()[0]) == 10);  // length includes itself
    CHECK(static_cast<uint8_t>(b.data()[1]) == 0x02);
}

static void testTraciExtendedCommand() {
    simulation::TraciBuffer b;
    std::string payload(300, 'x');
    b.putCommand(0xa4, payload);
    // [0x00][4-byte length][cmd][payload], length counts everything
    CHECK(b.data().size() == 306);
    simulation::TraciReader r(b.data());
    CHECK(r.readLength() == 306);
    CHECK(r.readU8() == 0xa4);
    CHECK(r.remaining() == 300);
}

static void testTraciRoundTrip() {
    simulation::TraciBuffer b;
    b.putU32(123456);
    b.putF64(-3.25);
    b.putStr("veh42");
    simulation::TraciReader r(b.data());
    CHECK(r.readU32() == 123456);
    CHECK_NEAR(r.readF64(), -3.25);
    CHECK(r.readStr() == "veh42");
    CHECK(r.atEnd());
}

// ---------------------------------------------------------------- allocator

static void testAllocatorExactFinishTimes() {
    resource_mgmt::FifoResourceAllocator a({{0, 2.0}});  // 2 Mcycles/ms
    a.admit(0, {1, 100.0, 0.0});   // 50 ms
    a.admit(0, {2, 100.0, 0.0});   // another 50 ms, should start at 50 ms
    auto done = a.advanceTo(100.0);
    CHECK(done.size() == 2);
    CHECK_NEAR(done[0].finish_ms, 50.0);
    CHECK_NEAR(done[1].start_ms, 50.0);
    CHECK_NEAR(done[1].finish_ms, 100.0);
    CHECK_NEAR(a.busyMs(0), 100.0);
}

static void testAllocatorWorkSpansTicks() {
    resource_mgmt::FifoResourceAllocator a({{0, 1.0}});
    a.admit(0, {7, 250.0, 0.0});  // 250 ms of work
    CHECK(a.advanceTo(100.0).empty());
    CHECK(a.advanceTo(200.0).empty());
    CHECK_NEAR(a.state()[0].pending_work_mcycles, 50.0);
    auto done = a.advanceTo(300.0);
    CHECK(done.size() == 1);
    CHECK_NEAR(done[0].finish_ms, 250.0);
}

static void testAllocatorWaitsForUpload() {
    resource_mgmt::FifoResourceAllocator a({{0, 1.0}});
    a.admit(0, {1, 10.0, 40.0});  // data arrives at 40 ms
    CHECK(a.state()[0].tasks_in_system == 1);
    auto done = a.advanceTo(100.0);
    CHECK(done.size() == 1);
    CHECK_NEAR(done[0].start_ms, 40.0);
    CHECK_NEAR(done[0].finish_ms, 50.0);
    CHECK(a.state()[0].tasks_in_system == 0);
}

static void testAllocatorQueueOrderIsUploadOrder() {
    resource_mgmt::FifoResourceAllocator a({{0, 1.0}});
    a.admit(0, {1, 10.0, 30.0});
    a.admit(0, {2, 10.0, 5.0});  // admitted later but its data arrives first
    auto done = a.advanceTo(100.0);
    CHECK(done.size() == 2);
    CHECK(done[0].task_id == 2);
    CHECK_NEAR(done[0].finish_ms, 15.0);
    CHECK_NEAR(done[1].finish_ms, 40.0);
}

// ---------------------------------------------------------------- schedulers

static scheduling::Dag chainPlusLeaf() {
    // 0 -> 1 -> 2 (long chain), and 3 (independent, small)
    scheduling::Dag d;
    d.id = 0;
    d.tasks = {{0, {}, 100, 0}, {1, {0}, 100, 0}, {2, {1}, 100, 0}, {3, {}, 50, 0}};
    return d;
}

static void testHeftRanks() {
    scheduling::HeftLiteScheduler h(1.0, 1.0);
    auto d = chainPlusLeaf();
    h.onDagArrival(d);
    CHECK_NEAR(h.rankOf(2), 100.0);
    CHECK_NEAR(h.rankOf(1), 200.0);
    CHECK_NEAR(h.rankOf(0), 300.0);
    CHECK_NEAR(h.rankOf(3), 50.0);
    // Task 3 became ready first, but task 0 heads the critical path.
    auto o = h.order({{3, 0, 0.0}, {0, 0, 1.0}});
    CHECK(o[0].task_id == 0);
}

static void testFifoOrder() {
    scheduling::FifoScheduler f;
    auto o = f.order({{5, 0, 2.0}, {9, 1, 1.0}, {4, 0, 2.0}});
    CHECK(o[0].task_id == 9);
    CHECK(o[1].task_id == 4);
    CHECK(o[2].task_id == 5);
}

// ---------------------------------------------------------------- policies

static void testPolicies() {
    scheduling::Task t{0, {}, 100.0, 100.0};
    // A: close but busy and slow.  B: far but idle and fast.
    offloading::Candidate a{{0, 1.0, 2, 300.0}, {5.0, 5.0}};
    offloading::Candidate b{{1, 4.0, 0, 0.0}, {30.0, 1.0}};
    // finish(A) = max(5 + 20, 300) + 100 = 400 ; finish(B) = max(30 + 100, 0) + 25 = 155
    CHECK_NEAR(offloading::estimatedFinishMs(t, a), 400.0);
    CHECK_NEAR(offloading::estimatedFinishMs(t, b), 155.0);

    offloading::NearestServerPolicy nearest;
    offloading::LeastLoadedPolicy least;
    offloading::EftPolicy eft;
    CHECK(*nearest.choose(t, {a, b}) == 0);
    CHECK(*least.choose(t, {a, b}) == 1);
    CHECK(*eft.choose(t, {a, b}) == 1);
    CHECK(!eft.choose(t, {}).has_value());
}

static void testChannelModel() {
    simulation::ChannelModel ch;
    auto at0 = ch.link(0, 0, 0, 0);
    auto at200 = ch.link(0, 0, 200, 0);
    CHECK_NEAR(at0.latency_ms, 2.0);
    CHECK_NEAR(at0.rate_kb_per_ms, 5.0);
    CHECK_NEAR(at200.latency_ms, 12.0);
    CHECK_NEAR(at200.rate_kb_per_ms, 2.5);
}

// ---------------------------------------------------------------- ML model file

static void testMlpModel() {
    // 6 inputs -> 2 ReLU units -> 1 output, small enough to check by hand.
    const char* path = "iov_test_model.txt";
    {
        std::ofstream f(path);
        f << "iov-mlp-v1 inputs 6\n"
             "mean 0 0 0 0 0 0\n"
             "scale 1 1 1 1 1 2\n"
             "target log1p_ms layers 2\n"
             "layer 6 2 relu\n"
             "1 0 0 0 0 0\n"
             "0 -1 0 0 0 1\n"
             "0 0\n"
             "layer 2 1 identity\n"
             "0.5 2\n"
             "1\n";
    }
    iov::offloading::MlpModel m(path);
    // x = (2, 3, ., ., ., 4): h1 = relu(2) = 2, h2 = relu(-3 + 4/2) = 0, out = 0.5*2 + 0 + 1 = 2
    CHECK_NEAR(m.predictMs({2, 3, 0, 0, 0, 4}), std::expm1(2.0));
    // h2 active: x6 = 10 -> h2 = relu(-3 + 5) = 2, out = 1 + 4 + 1 = 6
    CHECK_NEAR(m.predictMs({2, 3, 0, 0, 0, 10}), std::expm1(6.0));
    std::remove(path);
}

// ---------------------------------------------------------------- DAG generator

static void testDagGeneratorIsValid() {
    std::mt19937 rng(123);
    uint64_t next = 0;
    for (int k = 0; k < 500; ++k) {
        auto d = scheduling::makeRandomDag(k, 0.0, "v", next, rng);
        CHECK(d.tasks.size() >= 3 && d.tasks.size() <= 6);
        std::set<uint64_t> seen;
        for (size_t i = 0; i < d.tasks.size(); ++i) {
            const auto& t = d.tasks[i];
            if (i == 0) CHECK(t.dependencies.empty());
            else CHECK(!t.dependencies.empty() && t.dependencies.size() <= 2);
            for (auto dep : t.dependencies) CHECK(seen.count(dep) == 1);  // earlier task => acyclic
            CHECK(t.workload_mcycles >= 100.0 && t.workload_mcycles <= 500.0);
            seen.insert(t.id);
        }
    }
    CHECK(next > 0);
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"traci short command", testTraciShortCommand},
        {"traci extended command", testTraciExtendedCommand},
        {"traci round trip", testTraciRoundTrip},
        {"allocator exact finish times", testAllocatorExactFinishTimes},
        {"allocator work spans ticks", testAllocatorWorkSpansTicks},
        {"allocator waits for upload", testAllocatorWaitsForUpload},
        {"allocator queue order", testAllocatorQueueOrderIsUploadOrder},
        {"heft ranks", testHeftRanks},
        {"fifo order", testFifoOrder},
        {"offloading policies", testPolicies},
        {"channel model", testChannelModel},
        {"ml model file", testMlpModel},
        {"dag generator", testDagGeneratorIsValid},
    };
    for (const auto& [name, fn] : tests) {
        int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "[ ok ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
