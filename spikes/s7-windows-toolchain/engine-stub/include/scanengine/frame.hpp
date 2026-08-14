// frame.hpp — exercises the C++ language features the real engine depends
// on for the transport/ and drivers/ layers (see Tech Spec §3, workstream A):
//   - designated initializers (C++20 core language; MSVC/clang/gcc all
//     support them under /std:c++20 or -std=c++20)
//   - std::span (C++20 <span>) for zero-copy views over serial/UDP byte
//     buffers, with a C++17 fallback (a tiny hand-rolled span-alike) so this
//     header still compiles if a target ever has to pin C++17 (e.g. an old
//     NDK).
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#define SCANENGINE_HAS_STD_SPAN 1
#include <span>
#endif

namespace scanengine {

#if defined(SCANENGINE_HAS_STD_SPAN)
template <typename T>
using Span = std::span<T>;
#else
// Minimal C++17 stand-in for std::span<T>: just enough for read-only byte
// views over a driver's ring buffer. Not a general replacement.
template <typename T>
class Span {
public:
    Span() = default;
    Span(T* data, std::size_t size) : data_(data), size_(size) {}

    T* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    T& operator[](std::size_t i) const { return data_[i]; }
    T* begin() const noexcept { return data_; }
    T* end() const noexcept { return data_ + size_; }

private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
};
#endif

// Which sensor/transport a raw frame came from — mirrors drivers/d6 and
// drivers/mid360 from the spec's module layout.
enum class SourceKind : std::uint8_t {
    kUnknown = 0,
    kD6Uart = 1,
    kMid360Udp = 2,
    kRtkNmea = 3,
};

// A timestamped, opaque byte frame as it crosses the transport -> driver
// boundary. Built with designated initializers at every call site in this
// stub and (per A1/A2/A3) in the real drivers, so field order changes can't
// silently transpose values — that bit us conceptually in the D6 spec's
// packet layout (§2.1), which is exactly the kind of bug designated
// initializers catch at compile time when a field is renamed.
struct RawFrame {
    SourceKind source = SourceKind::kUnknown;
    std::int64_t t_mono_ns = 0;
    std::uint32_t sequence = 0;
    Span<const std::uint8_t> payload{};
};

// Constructs a frame the way real driver code will: via designated
// initializers. Kept as a real (non-inline-in-header-only) function so both
// the C++20 std::span path and the C++17 fallback path get compiled and
// exercised by the unit tests on every target.
RawFrame make_d6_frame(std::int64_t t_mono_ns,
                        std::uint32_t sequence,
                        const std::uint8_t* data,
                        std::size_t len);

}  // namespace scanengine
