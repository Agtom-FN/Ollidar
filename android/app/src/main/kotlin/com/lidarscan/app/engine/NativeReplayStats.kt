package com.lidarscan.app.engine

/**
 * Mirrors `lidarscan_jni::ReplayStatsSnapshot` (`replay_engine.h`), itself a
 * snapshot of `scanengine::lscan::ReplayStats` plus run state. Constructed
 * from JNI via a cached constructor (`(JJIIZZI)V` —
 * `replay_jni.cpp`'s `nativeReplayStats`).
 */
data class NativeReplayStats(
    val chunksReplayed: Long,
    val bytesReplayed: Long,
    val truncatedTailChunks: Int,
    val crcMismatchChunks: Int,
    val running: Boolean,
    val done: Boolean,
    /** `scanengine::ScanError` the replay thread ended with; 0 == kOk. */
    val resultError: Int,
)
