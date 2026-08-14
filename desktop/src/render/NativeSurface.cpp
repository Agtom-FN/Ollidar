#include "render/NativeSurface.h"

namespace lidarscan {

const char* to_string(RenderBackend b) {
  switch (b) {
    case RenderBackend::kMetal: return "metal";
    case RenderBackend::kVulkan: return "vulkan";
  }
  return "unknown";
}

}  // namespace lidarscan
