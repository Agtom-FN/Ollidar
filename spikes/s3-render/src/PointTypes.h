#pragma once
#include <cstdint>

// Interleaved point vertex: 16 bytes (position float3 + RGBA8).
// This is the layout the engine's paged streaming buffers would use.
struct PointVertex {
    float x, y, z;
    uint8_t r, g, b, a;
};
static_assert(sizeof(PointVertex) == 16, "PointVertex must be 16 bytes");
