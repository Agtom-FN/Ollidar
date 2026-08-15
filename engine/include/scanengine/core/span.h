// span.h — zero-copy views over byte buffers.
//
// Migrated from the S7 spike (spikes/s7-windows-toolchain/engine-stub/
// include/scanengine/frame.hpp), which proved on all five CI targets that the
// C++20 <span> path and the C++17 fallback both compile. Every transport/ and
// drivers/ entry point takes a Span<const uint8_t> rather than a pointer+size
// pair, so a buffer's extent can never be transposed at a call site.
//
// Owner: A1. Stable — do not change the fallback semantics without checking
// the Android NDK leg of engine-ci.
#ifndef SCANENGINE_CORE_SPAN_H
#define SCANENGINE_CORE_SPAN_H

#include <cstddef>
#include <cstdint>

// <version> defines the feature-test macros. Without it, __cpp_lib_span is
// only visible in TUs that happened to include <span> transitively first —
// which split this header between std::span and the fallback across TUs on
// GCC and broke the link with undefined std::span symbols (engine-ci #2).
#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#define SCANENGINE_HAS_STD_SPAN 1
#include <span>
#endif

namespace scanengine {

#if defined(SCANENGINE_HAS_STD_SPAN)
template <typename T>
using Span = std::span<T>;
#else
// Minimal C++17 stand-in for std::span<T>: enough for read-only byte views
// over a driver ring buffer. Not a general replacement.
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

using ByteSpan = Span<const std::uint8_t>;

inline ByteSpan bytes(const std::uint8_t* p, std::size_t n) { return ByteSpan(p, n); }

}  // namespace scanengine

#endif  // SCANENGINE_CORE_SPAN_H
