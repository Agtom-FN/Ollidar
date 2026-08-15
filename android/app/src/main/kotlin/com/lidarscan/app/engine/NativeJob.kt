package com.lidarscan.app.engine

/**
 * B6: one row of A15's job queue, marshalled from `scanengine::jobs::Job` by
 * `processing_jni.cpp`.
 *
 * **The constructor signature is load-bearing** — `JNI_OnLoad` looks it up as
 * the literal string `(JIIFILjava/lang/String;Ljava/lang/String;)V`. Adding,
 * removing or reordering a parameter here without changing that string compiles
 * on both sides and fails at library-load time; `javap -s` on the compiled
 * class is what checks it (see android/NOTES.md's verification section).
 */
class NativeJob(
    @JvmField val id: Long,
    /** `jobs::JobKind` — see [com.lidarscan.core.jobs.JobKind]. */
    @JvmField val kind: Int,
    /** `jobs::JobState` — see [com.lidarscan.core.jobs.JobState]. */
    @JvmField val state: Int,
    @JvmField val progress: Float,
    /** `scan_error_t`; 0 = OK. 9 (`kCancelled`) is how a cancellation reads. */
    @JvmField val error: Int,
    @JvmField val stage: String,
    @JvmField val message: String,
)

/**
 * B12: the verdict of a georeferenced auto-merge (A13).
 *
 * Same descriptor rule as [NativeJob]:
 * `(ZIIIIIFFJJZLjava/lang/String;Ljava/lang/String;)V`.
 */
class NativeMergeSummary(
    @JvmField val ok: Boolean,
    @JvmField val sessionsAligned: Int,
    @JvmField val sessionsSkipped: Int,
    @JvmField val pairsRefined: Int,
    @JvmField val pairsConverged: Int,
    /** Pairs the overlap gate refused. A13 **reports** these rather than merging them. */
    @JvmField val pairsLowOverlap: Int,
    @JvmField val worstRmsM: Float,
    @JvmField val worstOverlap: Float,
    @JvmField val inputPoints: Long,
    @JvmField val mergedPoints: Long,
    /**
     * Two sessions carrying different non-zero EPSG codes. Still composable —
     * the ENU→ECEF→ENU path does not use the code — but it usually means the
     * operator picked two different project CRSs, so it is surfaced.
     */
    @JvmField val epsgMismatch: Boolean,
    @JvmField val blocker: String,
    @JvmField val message: String,
)
