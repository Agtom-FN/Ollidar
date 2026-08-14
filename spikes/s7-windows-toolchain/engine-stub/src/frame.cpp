#include "scanengine/frame.hpp"

namespace scanengine {

RawFrame make_d6_frame(std::int64_t t_mono_ns,
                        std::uint32_t sequence,
                        const std::uint8_t* data,
                        std::size_t len) {
    // Designated initializers (C++20 core language feature). Order here
    // matches declaration order in RawFrame, as required by the standard.
    return RawFrame{
        .source = SourceKind::kD6Uart,
        .t_mono_ns = t_mono_ns,
        .sequence = sequence,
        .payload = Span<const std::uint8_t>(data, len),
    };
}

}  // namespace scanengine
