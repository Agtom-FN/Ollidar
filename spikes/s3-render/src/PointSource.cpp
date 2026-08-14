#include "PointSource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr float kRoomX = 12.0f;
constexpr float kRoomY = 8.0f;
constexpr float kRoomZ = 3.0f;

struct Column {
    float cx, cy, r;
};
constexpr Column kColumns[4] = {
        {3.0f, 2.0f, 0.25f},
        {3.0f, 6.0f, 0.25f},
        {9.0f, 2.0f, 0.25f},
        {9.0f, 6.0f, 0.25f},
};

inline float rayBoxT(float px, float py, float pz, float dx, float dy, float dz) {
    // Distance from an interior point to the room shell along (dx,dy,dz).
    float t = 1e30f;
    auto slab = [&](float p, float d, float lo, float hi) {
        if (std::fabs(d) < 1e-6f) return;
        float t0 = (lo - p) / d;
        float t1 = (hi - p) / d;
        float tc = (t0 > 0.0f) ? t0 : 1e30f;
        if (t1 > 0.0f && t1 < tc) tc = t1;
        if (tc < t) t = tc;
    };
    slab(px, dx, 0.0f, kRoomX);
    slab(py, dy, 0.0f, kRoomY);
    slab(pz, dz, 0.0f, kRoomZ);
    return t;
}

inline float rayColumnT(float px, float py, float dx, float dy, const Column& c) {
    // 2D ray/circle in XY; columns run floor to ceiling.
    const float ox = px - c.cx;
    const float oy = py - c.cy;
    const float a = dx * dx + dy * dy;
    if (a < 1e-9f) return 1e30f;
    const float b = 2.0f * (ox * dx + oy * dy);
    const float cc = ox * ox + oy * oy - c.r * c.r;
    const float disc = b * b - 4.0f * a * cc;
    if (disc < 0.0f) return 1e30f;
    const float sq = std::sqrt(disc);
    const float t0 = (-b - sq) / (2.0f * a);
    if (t0 > 0.05f) return t0;
    const float t1 = (-b + sq) / (2.0f * a);
    return (t1 > 0.05f) ? t1 : 1e30f;
}

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

} // namespace

void PointSource::generateBulk(PointVertex* dst, size_t n, uint64_t startIndex) {
    for (size_t k = 0; k < n; ++k) {
        const uint64_t i = startIndex + k;
        const double di = double(i);

        // Scanner pose: slow Lissajous walk through the room at ~1.5 m height.
        const float px = 6.0f + 4.2f * float(std::sin(di * 3.1e-6));
        const float py = 4.0f + 2.6f * float(std::sin(di * 4.7e-6 + 1.1));
        const float pz = 1.5f + 0.35f * float(std::sin(di * 9.3e-6));

        // Sweep: fast azimuth, slow elevation nod (pushbroom-like).
        const float az = float(di * 0.0121);
        const float el = 1.05f * float(std::sin(di * 3.7e-4));
        const float ce = std::cos(el);
        const float dx = ce * std::cos(az);
        const float dy = ce * std::sin(az);
        const float dz = std::sin(el);

        float t = rayBoxT(px, py, pz, dx, dy, dz);
        bool hitColumn = false;
        for (const Column& c : kColumns) {
            const float tc = rayColumnT(px, py, dx, dy, c);
            if (tc < t) {
                t = tc;
                hitColumn = true;
            }
        }

        const uint32_t h = hash32(uint32_t(i) * 2654435761u);
        const float jitter = (float(h & 0xffff) / 65535.0f - 0.5f) * 0.012f;
        t += jitter;

        const float X = px + dx * t;
        const float Y = py + dy * t;
        const float Z = pz + dz * t;

        // Height ramp (deep blue -> cyan -> yellow), columns tinted warm.
        const float hn = std::clamp(Z / kRoomZ, 0.0f, 1.0f);
        float rr = std::clamp(1.6f * hn - 0.35f, 0.0f, 1.0f);
        float gg = std::clamp(1.45f * hn + 0.12f, 0.0f, 1.0f);
        float bb = std::clamp(1.15f - 1.25f * hn, 0.05f, 1.0f);
        if (hitColumn) {
            rr = std::min(1.0f, rr * 0.55f + 0.62f);
            gg = gg * 0.55f + 0.24f;
            bb = bb * 0.35f + 0.10f;
        }
        // Intensity jitter so the cloud does not look like flat vertex colour.
        const float inten = 0.82f + 0.18f * (float((h >> 16) & 0xff) / 255.0f);

        PointVertex& v = dst[k];
        v.x = X;
        v.y = Y;
        v.z = Z;
        v.r = uint8_t(std::clamp(rr * inten, 0.0f, 1.0f) * 255.0f);
        v.g = uint8_t(std::clamp(gg * inten, 0.0f, 1.0f) * 255.0f);
        v.b = uint8_t(std::clamp(bb * inten, 0.0f, 1.0f) * 255.0f);
        v.a = 255;
    }
}

void PointSource::generateBulkParallel(PointVertex* dst, size_t n, uint64_t startIndex) {
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const unsigned nthreads = std::min<unsigned>(hw, 10);
    if (n < 100000 || nthreads <= 1) {
        generateBulk(dst, n, startIndex);
        return;
    }
    std::vector<std::thread> ts;
    ts.reserve(nthreads);
    const size_t chunk = (n + nthreads - 1) / nthreads;
    for (unsigned t = 0; t < nthreads; ++t) {
        const size_t off = t * chunk;
        if (off >= n) break;
        const size_t cnt = std::min(chunk, n - off);
        ts.emplace_back([=] { generateBulk(dst + off, cnt, startIndex + off); });
    }
    for (auto& th : ts) th.join();
}

PointSource::PointSource() : mRing(kRingCapacity) {}

PointSource::~PointSource() {
    stop();
}

void PointSource::resetCounters() {
    mProduced.store(0, std::memory_order_relaxed);
    mDropped.store(0, std::memory_order_relaxed);
    mHead.store(0, std::memory_order_relaxed);
    mTail.store(0, std::memory_order_relaxed);
}

void PointSource::start(double pointsPerSecond) {
    stop();
    mHead.store(0, std::memory_order_relaxed);
    mTail.store(0, std::memory_order_relaxed);
    mRunning.store(true, std::memory_order_release);
    mThread = std::thread([this, pointsPerSecond] { producerLoop(pointsPerSecond); });
}

void PointSource::stop() {
    if (mRunning.exchange(false, std::memory_order_acq_rel)) {
        if (mThread.joinable()) mThread.join();
    } else if (mThread.joinable()) {
        mThread.join();
    }
}

void PointSource::producerLoop(double pointsPerSecond) {
    using clock = std::chrono::steady_clock;
    // Emit in small bursts (like a lidar packet) so the pacing is realistic.
    const size_t burst = 2000;
    const auto burstPeriod = std::chrono::duration<double>(double(burst) / pointsPerSecond);
    const auto t0 = clock::now();
    uint64_t bursts = 0;
    std::vector<PointVertex> scratch(burst);

    while (mRunning.load(std::memory_order_acquire)) {
        PointSource::generateBulk(scratch.data(), burst, mGenIndex);
        mGenIndex += burst;

        const uint64_t head = mHead.load(std::memory_order_relaxed);
        const uint64_t tail = mTail.load(std::memory_order_acquire);
        const size_t free = kRingCapacity - size_t(head - tail);
        const size_t toWrite = std::min(free, burst);
        if (toWrite < burst) {
            mDropped.fetch_add(burst - toWrite, std::memory_order_relaxed);
        }
        for (size_t k = 0; k < toWrite; ++k) {
            mRing[(head + k) & (kRingCapacity - 1)] = scratch[k];
        }
        mHead.store(head + toWrite, std::memory_order_release);
        mProduced.fetch_add(toWrite, std::memory_order_relaxed);

        ++bursts;
        const auto target = t0 + std::chrono::duration_cast<clock::duration>(burstPeriod * double(bursts));
        const auto now = clock::now();
        if (target > now) {
            std::this_thread::sleep_for(target - now);
        }
    }
}

size_t PointSource::drain(PointVertex* dst, size_t maxPoints) {
    const uint64_t head = mHead.load(std::memory_order_acquire);
    const uint64_t tail = mTail.load(std::memory_order_relaxed);
    size_t avail = size_t(head - tail);
    if (avail == 0) return 0;
    const size_t n = std::min(avail, maxPoints);
    const size_t start = size_t(tail & (kRingCapacity - 1));
    const size_t first = std::min(n, kRingCapacity - start);
    std::memcpy(dst, mRing.data() + start, first * sizeof(PointVertex));
    if (n > first) {
        std::memcpy(dst + first, mRing.data(), (n - first) * sizeof(PointVertex));
    }
    mTail.store(tail + n, std::memory_order_release);
    return n;
}
