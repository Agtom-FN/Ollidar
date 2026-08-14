// worker_pool.hpp — exercises <thread>/<mutex>/<condition_variable>, which
// every SLAM/processing module (slam/, plan/, merge/, jobs/) needs and which
// has historically been a portability sore spot (pthreads vs Win32 threads,
// MinGW's incomplete <thread> before recent GCC, etc.). Using the standard
// library exclusively — no platform thread APIs — is the point of this
// file: if it compiles and passes tests on all 5 CI targets, the engine's
// concurrency primitives are validated everywhere without per-OS code.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace scanengine {

// A tiny fixed-size thread pool. Not meant to be the real engine's job
// scheduler (jobs/ will be more capable) — just enough surface to prove
// std::thread + condvar + mutex build and run correctly cross-platform,
// including under Windows' /MD (multithreaded DLL) CRT (see
// TOOLCHAIN_NOTES.md).
class WorkerPool {
public:
    explicit WorkerPool(std::size_t thread_count);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void submit(std::function<void()> task);

    // Blocks until every submitted task has run. Used by tests; a real
    // scheduler would additionally expose futures/results.
    void drain();

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable drain_cv_;
    std::size_t in_flight_ = 0;
    bool stop_ = false;
};

}  // namespace scanengine
