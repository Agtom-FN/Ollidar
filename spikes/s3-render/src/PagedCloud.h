#pragma once

#include "PointTypes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <utils/Entity.h>

namespace filament {
class Engine;
class IndexBuffer;
class MaterialInstance;
class Scene;
class VertexBuffer;
} // namespace filament

// Paged streaming point buffers, ~1M points per page (the layout §3.12 calls
// for). Each page is one VertexBuffer + one identity IndexBuffer + one
// renderable entity drawn as PrimitiveType::POINTS. Appending only touches the
// tail page: a partial VertexBuffer::setBufferAt() upload plus a
// setGeometryAt() count bump. Nothing is reallocated while streaming.
class PagedCloud {
public:
    static constexpr uint32_t kPageCapacity = 1u << 20; // 1,048,576 points

    void init(filament::Engine* engine, filament::Scene* scene,
              filament::MaterialInstance* mi, uint32_t maxPages);
    void shutdown();

    // Append n points to the tail; allocates pages on demand. Returns appended.
    size_t append(const PointVertex* src, size_t n);

    // Drop all points (pages and GPU buffers are retained for reuse).
    void clear();

    size_t total() const { return mTotal; }
    size_t pageCount() const { return mPages.size(); }
    size_t gpuBytes() const;

private:
    struct Page {
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
        utils::Entity entity;
        uint32_t live = 0;
        bool inScene = false;
    };

    Page& ensureTailPage();
    void uploadRange(Page& p, uint32_t firstPoint, const PointVertex* src, uint32_t n);
    void syncGeometry(Page& p);

    filament::Engine* mEngine = nullptr;
    filament::Scene* mScene = nullptr;
    filament::MaterialInstance* mMaterial = nullptr;
    uint32_t mMaxPages = 0;
    std::vector<Page> mPages;
    size_t mTail = 0; // index of the page currently being filled
    size_t mTotal = 0;
};
