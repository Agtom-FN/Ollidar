package com.lidarscan.app.processing

import com.lidarscan.core.capture.TrajectoryRibbon
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * ROUND 16 item 59 — reads `processed/trajectory.bin`, the corrected walk the
 * engine writes beside the corrected cloud.
 *
 * ## Why this is a file read and not a JNI call
 *
 * The trajectory exists inside `reprocess_d6_container` already — corrected by
 * the section stitch and, when the closer fires, by the ROUND 16 loop-end
 * correction — and until this round it was discarded at the end of that
 * function. Two ways to get at it were available:
 *
 *  * a new engine entry point that re-derives it, which would mean a second
 *    full resolve (seconds of work, on a phone) and, worse, a SECOND ANSWER:
 *    nothing would guarantee the path it returned belonged to the same pass
 *    that produced `map_stitched.bin`, and a path drawn from an uncorrected
 *    trajectory over a corrected cloud is a lie shaped exactly like a
 *    diagnosis;
 *  * the pass that already has it writes it down.
 *
 * The second is smaller and cannot disagree with itself, so that is what
 * happens. `processed/` is the directory literally named for derived output,
 * deleting this file returns the container to what the phone sealed, and the
 * raw streams are untouched — the same contract `map_stitched.bin` and
 * `stitch.json` live by (see `engine/include/scanengine/slam/post/reprocess.h`).
 *
 * ## The format, and this is an INDEPENDENT decoder of it
 *
 * Written from the format's description rather than from a shared struct, on
 * purpose — a reader generated from the writer tests neither. Little-endian
 * throughout, which is every Android device this ships to and is asserted
 * rather than assumed by [ByteOrder.LITTLE_ENDIAN] below.
 *
 * ```
 * offset  size  meaning
 *      0     8  magic "LSTRAJ01"
 *      8     4  u32 pose count
 *     12     4  reserved, zero
 *     16  12*n  n records of three float32 metres (x, y, z)
 * ```
 *
 * Every failure returns [TrajectoryRibbon.EMPTY] rather than throwing: a
 * missing or truncated derived file means "no path to draw", which is a state
 * Review already has to render (a container that has never been processed), and
 * it must never be a reason the viewer fails to open a scan.
 */
object TrajectoryFile {

    private const val HEADER_BYTES = 16
    private const val RECORD_BYTES = 12
    private val MAGIC = byteArrayOf(
        'L'.code.toByte(), 'S'.code.toByte(), 'T'.code.toByte(), 'R'.code.toByte(),
        'A'.code.toByte(), 'J'.code.toByte(), '0'.code.toByte(), '1'.code.toByte(),
    )

    /** The largest file this will read: 4 M poses is 37 hours at 30 Hz. */
    private const val MAX_POSES = 4_000_000

    fun read(file: File): TrajectoryRibbon.Ribbon {
        if (!file.isFile) return TrajectoryRibbon.EMPTY
        val length = file.length()
        if (length < HEADER_BYTES || length > Int.MAX_VALUE.toLong()) return TrajectoryRibbon.EMPTY
        val bytes = runCatching { file.readBytes() }.getOrNull() ?: return TrajectoryRibbon.EMPTY
        return decode(bytes)
    }

    /** Exposed so a JVM test can feed it bytes without touching a filesystem. */
    fun decode(bytes: ByteArray): TrajectoryRibbon.Ribbon {
        if (bytes.size < HEADER_BYTES) return TrajectoryRibbon.EMPTY
        for (i in MAGIC.indices) {
            if (bytes[i] != MAGIC[i]) return TrajectoryRibbon.EMPTY
        }
        val buf = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        val count = buf.getInt(8)
        if (count <= 1 || count > MAX_POSES) return TrajectoryRibbon.EMPTY
        // The length check is what makes a truncated file "no path" instead of
        // a path that runs into whatever was after it on disk.
        val expected = HEADER_BYTES.toLong() + count.toLong() * RECORD_BYTES
        if (bytes.size.toLong() != expected) return TrajectoryRibbon.EMPTY

        val xyz = FloatArray(count * 3)
        var o = HEADER_BYTES
        for (i in 0 until count) {
            xyz[i * 3] = buf.getFloat(o)
            xyz[i * 3 + 1] = buf.getFloat(o + 4)
            xyz[i * 3 + 2] = buf.getFloat(o + 8)
            o += RECORD_BYTES
        }
        // Thinning and colouring live in `:core` with the live trail's, so a
        // sealed walk and the one the operator watched are drawn by one rule.
        return TrajectoryRibbon.fromPoses(xyz, count)
    }
}
