#include "scanengine/worker_pool.hpp"

namespace scanengine {

WorkerPool::WorkerPool(std::size_t thread_count) {
    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void WorkerPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
        ++in_flight_;
    }
    cv_.notify_one();
}

void WorkerPool::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    drain_cv_.wait(lock, [this] { return in_flight_ == 0; });
}

void WorkerPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --in_flight_;
            if (in_flight_ == 0) drain_cv_.notify_all();
        }
    }
}

}  // namespace scanengine
