package com.lidarscan.app.merge

import com.lidarscan.app.engine.NativeMergeSummary
import com.lidarscan.app.engine.ScanEngineNative
import com.lidarscan.app.processing.ProcessingRepository
import com.lidarscan.core.gnss.GeorefRecord
import com.lidarscan.core.store.Project
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * B12 — Tech Spec §3.10's "Android offers georeferenced auto-merge only".
 *
 * The deliberate shape of this flow, and what it refuses:
 *
 * * **Georeferenced only.** A13's `align_georeferenced()` places sessions by
 *   composing through their shared CRS. Without that, the alternative is
 *   manual 3-point or drag alignment, which needs a two-cloud picking
 *   workspace — that is the desktop merge workbench (C6), and §3.10 says so.
 *   The app refuses politely and names the reason rather than merging at the
 *   identity, which A13 itself calls "the worst possible failure mode — it
 *   looks like data".
 * * **Each session is post-processed first, and that is the expensive part.**
 *   A13 cannot read a cloud out of a `.lscan`: nothing writes a processed cloud
 *   into one (`merge/merge.h`'s own note, and A7 §8 item 2), so
 *   `SessionMerger::add_session(lscan_dir)` is unimplemented in the engine. The
 *   only way to have two clouds in memory is to run the pipeline for each,
 *   which the UI states up front instead of surprising the user with it.
 *   A session already post-processed in this app session is reused via its job
 *   id, which is why running Post-process on each project first is worth doing.
 */
class MergeRepository(private val processing: ProcessingRepository) {

    data class Candidate(
        val project: Project,
        val georef: GeorefRecord?,
        /** A finished post-process job for this project in this app session, or 0. */
        val chainFromJob: Long,
    ) {
        val isGeoreferenced: Boolean get() = georef?.converged == true
        val reason: String? get() = when {
            georef == null -> "No georeference recorded — this capture had no RTK rover attached."
            !georef.converged -> "Georeference did not converge: ${georef.blocker.ifBlank { "not enough usable fixes" }}."
            else -> null
        }
    }

    data class Progress(val fraction: Float, val label: String)

    suspend fun merge(
        candidates: List<Candidate>,
        outputPly: File?,
        onProgress: (Progress) -> Unit,
    ): Result<NativeMergeSummary> = withContext(Dispatchers.Default) {
        val h = processing.handleOrZero()
        if (h == 0L) return@withContext Result.failure(IllegalStateException(processing.lastError()))
        if (candidates.size < 2) {
            return@withContext Result.failure(IllegalArgumentException("A merge needs at least two sessions."))
        }
        val summary = ScanEngineNative.nativeProcRunMerge(
            h,
            candidates.map { it.project.directory.absolutePath }.toTypedArray(),
            candidates.map { it.project.manifest.name }.toTypedArray(),
            candidates.map { it.chainFromJob }.toLongArray(),
            encodeGeoref(candidates.map { it.georef }),
            outputPly?.absolutePath ?: "",
            { fraction, label -> onProgress(Progress(fraction, label)) },
        ) ?: return@withContext Result.failure(IllegalStateException(ScanEngineNative.nativeProcLastError(h)))
        Result.success(summary)
    }

    fun cancel() {
        val h = processing.handleOrZero()
        if (h != 0L) ScanEngineNative.nativeProcCancelMerge(h)
    }

    companion object {
        /** Doubles per session in the flat array `nativeProcRunMerge` takes. */
        const val GEOREF_STRIDE = 23

        const val IDX_VALID = 0
        const val IDX_CONVERGED = 1
        const val IDX_EPSG = 2

        /** 16 doubles, ROW-MAJOR — `global_from_local`. */
        const val IDX_MATRIX_0 = 3
        const val IDX_ENU_LAT = 19
        const val IDX_ENU_LON = 20
        const val IDX_ENU_HEIGHT = 21
        const val IDX_SIGMA_H = 22

        /**
         * Packs each session's [GeorefRecord] into the layout
         * `processing_jni.cpp` documents. Kept next to the constants above and
         * asserted by `MergeGeorefEncodingTest` — a shifted layout here would
         * put a session at the wrong place on the globe, silently.
         */
        fun encodeGeoref(records: List<GeorefRecord?>): DoubleArray {
            val out = DoubleArray(records.size * GEOREF_STRIDE)
            records.forEachIndexed { i, r ->
                val base = i * GEOREF_STRIDE
                if (r == null) return@forEachIndexed
                out[base + IDX_VALID] = 1.0
                out[base + IDX_CONVERGED] = if (r.converged) 1.0 else 0.0
                out[base + IDX_EPSG] = r.epsg.toDouble()
                // All 16 or none. A per-element fallback would happily copy the
                // first few values of a wrong-length array and pad the rest,
                // producing a singular matrix that collapses the session to a
                // point — and A13 places an unaligned session at the identity,
                // where it "looks like data".
                val m = r.globalFromLocal
                for (k in 0 until 16) {
                    out[base + IDX_MATRIX_0 + k] = if (m.size == 16) m[k] else if (k % 5 == 0) 1.0 else 0.0
                }
                out[base + IDX_ENU_LAT] = r.enuOriginLatDeg
                out[base + IDX_ENU_LON] = r.enuOriginLonDeg
                out[base + IDX_ENU_HEIGHT] = r.enuOriginHeightM
                out[base + IDX_SIGMA_H] = r.horizontalSigmaM
            }
            return out
        }
    }
}
