#include "PagedCloud.h"

#include <filament/Box.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/VertexBuffer.h>

#include <utils/EntityManager.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace filament;

void PagedCloud::init(Engine* engine, Scene* scene, MaterialInstance* mi, uint32_t maxPages) {
    mEngine = engine;
    mScene = scene;
    mMaterial = mi;
    mMaxPages = maxPages;
    mPages.reserve(maxPages);
}

void PagedCloud::shutdown() {
    if (!mEngine) return;
    auto& em = utils::EntityManager::get();
    for (Page& p : mPages) {
        if (p.inScene) mScene->remove(p.entity);
        if (!p.entity.isNull()) {
            mEngine->destroy(p.entity);
            em.destroy(p.entity);
        }
        if (p.vb) mEngine->destroy(p.vb);
        if (p.ib) mEngine->destroy(p.ib);
    }
    mPages.clear();
    mTail = 0;
    mTotal = 0;
}

void PagedCloud::clear() {
    for (Page& p : mPages) {
        p.live = 0;
        if (p.inScene) {
            mScene->remove(p.entity);
            p.inScene = false;
        }
    }
    mTail = 0;
    mTotal = 0;
}

size_t PagedCloud::gpuBytes() const {
    return mPages.size() * size_t(kPageCapacity) * (sizeof(PointVertex) + sizeof(uint32_t));
}

PagedCloud::Page& PagedCloud::ensureTailPage() {
    // Advance past any full pages (pages survive clear() and get refilled).
    while (mTail < mPages.size() && mPages[mTail].live >= kPageCapacity) {
        ++mTail;
    }
    if (mTail < mPages.size()) {
        return mPages[mTail];
    }
    Page p;

    p.vb = VertexBuffer::Builder()
                   .vertexCount(kPageCapacity)
                   .bufferCount(1)
                   .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                              0, sizeof(PointVertex))
                   .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::UBYTE4,
                              offsetof(PointVertex, r), sizeof(PointVertex))
                   .normalized(VertexAttribute::COLOR)
                   .build(*mEngine);

    // Identity index buffer, uploaded once. POINTS still needs an IndexBuffer
    // in Filament's API; 4 MB per 1M-point page.
    {
        auto* idx = static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * kPageCapacity));
        for (uint32_t i = 0; i < kPageCapacity; ++i) idx[i] = i;
        p.ib = IndexBuffer::Builder()
                       .indexCount(kPageCapacity)
                       .bufferType(IndexBuffer::IndexType::UINT)
                       .build(*mEngine);
        p.ib->setBuffer(*mEngine,
                        IndexBuffer::BufferDescriptor(idx, sizeof(uint32_t) * kPageCapacity,
                                                      [](void* b, size_t, void*) { std::free(b); }));
    }

    p.entity = utils::EntityManager::get().create();
    // Room is 12 x 8 x 3 m; give every page the full room box. Frustum culling
    // stays on but never removes work here -- worst case for the benchmark.
    const Box box{{6.0f, 4.0f, 1.5f}, {6.5f, 4.5f, 2.0f}};
    RenderableManager::Builder(1)
            .boundingBox(box)
            .material(0, mMaterial)
            .geometry(0, RenderableManager::PrimitiveType::POINTS, p.vb, p.ib, 0, 1)
            .culling(true)
            .castShadows(false)
            .receiveShadows(false)
            .build(*mEngine, p.entity);

    mPages.push_back(p);
    mTail = mPages.size() - 1;
    return mPages[mTail];
}

void PagedCloud::uploadRange(Page& p, uint32_t firstPoint, const PointVertex* src, uint32_t n) {
    const size_t bytes = size_t(n) * sizeof(PointVertex);
    void* mem = std::malloc(bytes);
    std::memcpy(mem, src, bytes);
    p.vb->setBufferAt(*mEngine, 0,
                      VertexBuffer::BufferDescriptor(mem, bytes,
                                                     [](void* b, size_t, void*) { std::free(b); }),
                      uint32_t(size_t(firstPoint) * sizeof(PointVertex)));
}

void PagedCloud::syncGeometry(Page& p) {
    auto& rm = mEngine->getRenderableManager();
    auto inst = rm.getInstance(p.entity);
    rm.setGeometryAt(inst, 0, RenderableManager::PrimitiveType::POINTS, p.vb, p.ib, 0,
                     std::max<uint32_t>(p.live, 1));
    if (!p.inScene && p.live > 0) {
        mScene->addEntity(p.entity);
        p.inScene = true;
    }
}

size_t PagedCloud::append(const PointVertex* src, size_t n) {
    size_t done = 0;
    while (done < n) {
        const bool allFull = mPages.empty() ||
                             (mTail >= mPages.size() - 1 && mPages.back().live >= kPageCapacity);
        if (mPages.size() >= mMaxPages && allFull) {
            break; // capacity reached
        }
        Page& p = ensureTailPage();
        const uint32_t room = kPageCapacity - p.live;
        const uint32_t take = uint32_t(std::min<size_t>(room, n - done));
        uploadRange(p, p.live, src + done, take);
        p.live += take;
        syncGeometry(p);
        done += take;
        mTotal += take;
    }
    return done;
}
