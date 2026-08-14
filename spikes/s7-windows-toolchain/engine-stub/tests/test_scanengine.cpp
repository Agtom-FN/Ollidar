#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "scanengine/clock.hpp"
#include "scanengine/eigen_demo.hpp"
#include "scanengine/frame.hpp"
#include "scanengine/worker_pool.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>

TEST_CASE("SteadyClock is monotonic and names its backend") {
    const auto t0 = scanengine::SteadyClock::now();
    const auto t1 = scanengine::SteadyClock::now();
    CHECK(t1.nanos >= t0.nanos);
    CHECK(scanengine::SteadyClock::backend_name() != nullptr);
    std::printf("[scanengine] clock backend: %s\n", scanengine::SteadyClock::backend_name());
}

TEST_CASE("RawFrame built via designated initializers round-trips payload") {
    const std::uint8_t bytes[4] = {0xAA, 0x55, 0xF0, 0x0F};  // D6 start-frame per spec 2.1
    const auto frame = scanengine::make_d6_frame(/*t_mono_ns=*/12345, /*sequence=*/7, bytes, 4);

    CHECK(frame.source == scanengine::SourceKind::kD6Uart);
    CHECK(frame.t_mono_ns == 12345);
    CHECK(frame.sequence == 7);
    REQUIRE(frame.payload.size() == 4);
    CHECK(frame.payload[0] == 0xAA);
    CHECK(frame.payload[3] == 0x0F);
}

TEST_CASE("WorkerPool runs submitted tasks and drains") {
    scanengine::WorkerPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i) {
        pool.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.drain();
    CHECK(counter.load() == 100);
}

TEST_CASE("Eigen dependency resolves and solves") {
    const double residual = scanengine::eigen_smoke_test();
    CHECK(residual < 1e-9);
}
