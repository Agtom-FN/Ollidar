#pragma once

#include "PointTypes.h"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

// Synthetic lidar-like point generator.
//
// Shape: a rotating scanner walking a Lissajous path inside a 12 x 8 x 3 m room;
// each sample is a ray cast from the scanner to the room shell (walls / floor /
// ceiling) plus four interior columns. Colour ramps with height, with per-point
// "intensity" jitter -- visually close to a real indoor scan, and cheap enough
// that generation is never the bottleneck.
//
// Two modes:
//  * generateBulk()  - deterministic, thread-parallel, used to preload N points
//                      for the static benchmarks.
//  * start(rate)     - a producer thread emitting `rate` points/second into an
//                      SPSC ring buffer, drained by the render thread. This is
//                      the live-ingest path (S2 delivers ~200k pts/s).
class PointSource {
public:
    PointSource();
    ~PointSource();

    // Deterministic bulk generation of points [startIndex, startIndex + n).
    static void generateBulk(PointVertex* dst, size_t n, uint64_t startIndex);
    // Multi-threaded wrapper around generateBulk.
    static void generateBulkParallel(PointVertex* dst, size_t n, uint64_t startIndex);

    void start(double pointsPerSecond);
    void stop();
    bool running() const { return mRunning.load(std::memory_order_relaxed); }

    // Consumer side (render thread). Returns number of points copied.
    size_t drain(PointVertex* dst, size_t maxPoints);

    uint64_t produced() const { return mProduced.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return mDropped.load(std::memory_order_relaxed); }
    void resetCounters();

private:
    void producerLoop(double pointsPerSecond);

    static constexpr size_t kRingCapacity = 1u << 21; // 2M points = 32 MB
    std::vector<PointVertex> mRing;
    std::atomic<uint64_t> mHead{0}; // producer writes
    std::atomic<uint64_t> mTail{0}; // consumer reads
    std::atomic<bool> mRunning{false};
    std::atomic<uint64_t> mProduced{0};
    std::atomic<uint64_t> mDropped{0};
    std::thread mThread;
    uint64_t mGenIndex = 0;
};
