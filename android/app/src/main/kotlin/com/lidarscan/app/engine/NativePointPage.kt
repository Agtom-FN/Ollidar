package com.lidarscan.app.engine

import java.nio.ByteBuffer

/**
 * One page of points, handed back from either the live capture engine
 * (`ScanEngineNative.nativeGetPointPage`, over `scan_engine_get_point_page`)
 * or B4's replay engine (`nativeReplayGetPointPage`, over
 * `scanengine::PageStore::page_view` directly) — see
 * `com.lidarscan.app.render.PointCloudSource` for the interface that makes
 * the renderer agnostic to which one it is reading.
 *
 * [buffer] is a DIRECT `ByteBuffer` aliasing the engine's own page memory —
 * zero-copy, per `scan_point_page`'s "stable for the page's lifetime"
 * contract (`engine/capi/scanengine_c.h`). It holds `count` back-to-back
 * 16-byte `scan_point_vertex` records: `float x, y, z; uint8 r, g, b, a;`
 * (little-endian, matches Filament's expected FLOAT3 + UBYTE4-normalized
 * vertex attribute layout with zero conversion — see
 * `PointCloudRenderer.uploadPage`). Never write through this buffer.
 *
 * Constructed from JNI via a cached constructor
 * (`(IIIIJJFFFFFFLjava/nio/ByteBuffer;)V` — scanengine_jni.cpp's/
 * replay_jni.cpp's shared `g_point_page_ctor`); keep the two in lock-step if
 * this field order changes.
 */
data class NativePointPage(
    val id: Int,
    val stream: Int,
    val count: Int,
    val capacity: Int,
    val tFirstNs: Long,
    val tLastNs: Long,
    val boundsMinX: Float,
    val boundsMinY: Float,
    val boundsMinZ: Float,
    val boundsMaxX: Float,
    val boundsMaxY: Float,
    val boundsMaxZ: Float,
    val buffer: ByteBuffer,
)
