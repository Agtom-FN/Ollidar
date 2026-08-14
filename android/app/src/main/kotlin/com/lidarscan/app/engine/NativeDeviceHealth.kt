package com.lidarscan.app.engine

/**
 * Field-for-field mirror of `scan_device_health` (`engine/capi/scanengine_c.h`),
 * constructed from C++ via JNI (`scanengine_jni.cpp`'s `nativeDeviceHealth`,
 * cached constructor `(IIIIJJJJJDDDJ)V` — keep the two in lock-step if either
 * side's field order changes). Converted to the Android-agnostic
 * `com.lidarscan.core.engine.DeviceHealth` before it crosses into `:core`
 * (see [RealEngineBridge]).
 */
data class NativeDeviceHealth(
    val id: Int,
    val kind: Int,
    val state: Int,
    val lastError: Int,
    val bytesIn: Long,
    val packetsOk: Long,
    val packetsBad: Long,
    val pointsOut: Long,
    val drops: Long,
    val pointsPerSec: Double,
    val rotationHz: Double,
    val checksumPassRate: Double,
    val tLastDataNs: Long,
)
