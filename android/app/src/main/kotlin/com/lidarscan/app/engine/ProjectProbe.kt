package com.lidarscan.app.engine

/**
 * ROUND 8 (owner item 27c) — **what a saved `.lscan` actually contains**, read
 * off the container's own bytes.
 *
 * ### Why this is read from the container and not from the app's manifest
 *
 * The app keeps its own sidecar (`lidarscan.json`, see
 * `core/store/FileProjectStore`) and it is the right place for names, display
 * params and the mount trim. It is the wrong place for *"is there a trajectory
 * in here"*, for three reasons that all showed up in the field:
 *
 *  * a project can arrive by import, transfer bundle, or ROUND 6's manifest
 *    recovery, and then the sidecar is a reconstruction rather than a record;
 *  * the sidecar is written by a version of this app, and the question being
 *    asked is precisely *"was this recorded by a version that stored poses"*;
 *  * the streams are the only thing that is always true about a container.
 *
 * ### What each flag decides
 *
 * [hasRecordedMap] picks the fast path: a 0.5.0+ capture cached its resolved
 * cloud into `streams/map.bin`, so Review can draw the 3D room immediately
 * instead of paying for a re-resolve first.
 *
 * [hasPoses] is the honest-explanation flag. A COIN-D6 is a 2D lidar; the third
 * dimension of a D6 scan is entirely the phone's trajectory. A capture without
 * `kPoseAr` chunks — which is **every capture this app made before 0.5.0**,
 * including the owner's own scan-015 — cannot be rebuilt into a 3D cloud by
 * anything, ever. Review says so in those words rather than showing an empty
 * viewer or, worse, the raw 2D fan.
 */
data class ProjectProbe(
    /** The directory opened as a `.lscan` at all. False for a missing or unreadable project. */
    val opened: Boolean,
    /** COIN-D6 chunks and no Mid-360 ones — the sensor, according to the bytes. */
    val isD6: Boolean,
    /** `ChunkType::kPoseAr` present. False for every pre-0.5.0 capture. */
    val hasPoses: Boolean,
    /** `ChunkType::kPointsXyzRgba` present — the resolved cloud the capture cached. */
    val hasRecordedMap: Boolean,
    /** `"mountCalibration"` present in the container's manifest.json. */
    val hasMount: Boolean,
) {
    /**
     * True when this is a D6 project that can be turned into a 3D cloud —
     * either because the resolved cloud is cached, or because the trajectory
     * is there to re-resolve it from.
     */
    val canShow3d: Boolean get() = opened && (hasRecordedMap || (isD6 && hasPoses))

    /**
     * True for the one case that has to be explained rather than fixed: a D6
     * capture with returns but no trajectory.
     */
    val predatesTrajectoryStorage: Boolean
        get() = opened && isD6 && !hasPoses && !hasRecordedMap

    companion object {
        /** The unreadable/absent answer, so callers never carry a nullable probe around. */
        val NONE = ProjectProbe(
            opened = false,
            isD6 = false,
            hasPoses = false,
            hasRecordedMap = false,
            hasMount = false,
        )

        /**
         * Decodes `nativeProcProbeProject`'s bitfield. The bit numbering is
         * fixed in `app/src/main/cpp/processing_jni.cpp` and mirrored here;
         * both sides carry the same comment, because a bitfield split across a
         * JNI boundary is exactly the kind of thing that drifts silently.
         *
         *   bit 0  opened            bit 3  has recorded map
         *   bit 1  is a D6 project   bit 4  manifest carries the mount extrinsic
         *   bit 2  has kPoseAr poses
         */
        fun of(flags: Long): ProjectProbe = ProjectProbe(
            opened = flags and 1L != 0L,
            isD6 = flags and 2L != 0L,
            hasPoses = flags and 4L != 0L,
            hasRecordedMap = flags and 8L != 0L,
            hasMount = flags and 16L != 0L,
        )
    }
}
