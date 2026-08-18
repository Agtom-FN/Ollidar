package com.lidarscan.core.capture

/**
 * ROUND 15 item 56 — what "Floor plan" produced, in numbers and then in words.
 *
 * Lives in `:core` and takes plain numbers so every sentence the operator
 * reads is unit-testable on a bare JVM: the flat `DoubleArray(24)` the JNI
 * returns is decoded in exactly one place ([fromNative]) and nothing else
 * knows the slot order.
 *
 * ## Why there are two modes, and why the mode is on the page
 *
 * A12's wall extractor wants two scanned FACES before it calls something a
 * wall, and it wants them inside a 50 cm horizontal band at about chest
 * height, because that is where an architect cuts a plan. A COIN-D6 is a 10 Hz
 * single-line scanner whose fan is vertical: one revolution crosses that band
 * in two places, so the band is thin evidence even on a good walk. On the
 * owner's best capture — scan-033, the 26.6 m loop — the band holds 21,143 of
 * 220,438 points.
 *
 * So the result is honest about which of two things it is:
 *
 *  * [Mode.WALLS] — lines were fitted. They may have come from the plan slice
 *    or, when that was too sparse, from the floor map ([wallsFromFloorMap]),
 *    and that distinction changes what a thickness means.
 *  * [Mode.DENSITY] — nothing could be fitted, and the drawing is the returns
 *    themselves at a stated metric scale. That is still a measurement; an
 *    empty sheet labelled "floor plan" would not be.
 *
 * And [noRoomClosed] is reported separately from `walls == 0`, because "no
 * wall was found" and "the outline never closed" are different things to be
 * told, and only the second one has an action attached to it.
 */
data class FloorPlanResult(
    val ran: Boolean,
    val mode: Mode,
    val wallsFromFloorMap: Boolean,
    val noRoomClosed: Boolean,
    val cloudPoints: Long,
    val bandPoints: Long,
    val mapPoints: Long,
    val occupiedCells: Int,
    val mapCells: Int,
    val walls: Int,
    val wallsPaired: Int,
    val openings: Int,
    val doors: Int,
    val windows: Int,
    val rooms: Int,
    val wallLengthMeters: Double,
    val roomAreaM2: Double,
    val largestRoomAreaM2: Double,
    val extentXMeters: Double,
    val extentYMeters: Double,
    val pixelsPerMeter: Double,
    val scaleBarMeters: Double,
    val pngWidth: Int,
    val pngHeight: Int,
    val pngPath: String = "",
    val pdfPath: String = "",
    val dxfPath: String = "",
    val cloudSource: String = "",
) {
    enum class Mode { WALLS, DENSITY }

    val hasImage: Boolean get() = ran && pngPath.isNotEmpty()

    /** True when there is a document to share as well as a picture. */
    val hasDrawings: Boolean get() = pdfPath.isNotEmpty() || dxfPath.isNotEmpty()

    /** The one line under the preview. */
    val headline: String
        get() = when {
            !ran -> "No floor plan could be made from this scan."
            mode == Mode.DENSITY ->
                "No wall could be fitted — this is a map of where the scanner's " +
                    "returns actually landed, drawn to scale."
            rooms > 0 ->
                "$walls walls, $rooms " + (if (rooms == 1) "room" else "rooms") +
                    ", %.1f m² of floor.".format(roomAreaM2)
            else -> "$walls walls, %.1f m of wall — no outline closed into a room.".format(
                wallLengthMeters,
            )
        }

    /**
     * The supporting sentence. Says where the geometry came from and what the
     * numbers rest on, because a plan is the artifact most likely to leave
     * this app and be believed by someone who was not there.
     */
    val detail: String?
        get() {
            if (!ran) return null
            val where = when {
                mode == Mode.DENSITY ->
                    "Nothing was fitted, so nothing here is a surveyed line."
                wallsFromFloorMap ->
                    "The chest-height slice was too thin to fit walls, so they were traced " +
                        "from the whole scan projected downwards. Wall thicknesses in this " +
                        "mode are assumed, not measured."
                wallsPaired > 0 ->
                    "$wallsPaired of $walls walls were scanned on both sides, so their " +
                        "thickness is measured rather than assumed."
                else ->
                    "Every wall here was scanned on one side only, so its thickness is " +
                        "assumed rather than measured."
            }
            val size = " Covers %.1f × %.1f m.".format(extentXMeters, extentYMeters)
            val gap = if (noRoomClosed) {
                " Walking the gaps again — slowly, with the phone facing the wall — is what " +
                    "closes an outline."
            } else {
                ""
            }
            return where + size + gap
        }

    companion object {
        /** Slot layout is pinned by `processing_jni.cpp`'s documented table. */
        fun fromNative(
            v: DoubleArray?,
            paths: Array<String>? = null,
        ): FloorPlanResult? {
            if (v == null || v.size < 24) return null
            return FloorPlanResult(
                ran = v[0] != 0.0,
                mode = if (v[1] == 0.0) Mode.WALLS else Mode.DENSITY,
                wallsFromFloorMap = v[2] != 0.0,
                noRoomClosed = v[3] != 0.0,
                cloudPoints = v[4].toLong(),
                bandPoints = v[5].toLong(),
                mapPoints = v[6].toLong(),
                occupiedCells = v[7].toInt(),
                walls = v[8].toInt(),
                wallsPaired = v[9].toInt(),
                openings = v[10].toInt(),
                doors = v[11].toInt(),
                windows = v[12].toInt(),
                rooms = v[13].toInt(),
                wallLengthMeters = v[14],
                roomAreaM2 = v[15],
                largestRoomAreaM2 = v[16],
                extentXMeters = v[17],
                extentYMeters = v[18],
                pixelsPerMeter = v[19],
                scaleBarMeters = v[20],
                pngWidth = v[21].toInt(),
                pngHeight = v[22].toInt(),
                mapCells = v[23].toInt(),
                pngPath = paths?.getOrNull(0).orEmpty(),
                pdfPath = paths?.getOrNull(1).orEmpty(),
                dxfPath = paths?.getOrNull(2).orEmpty(),
                cloudSource = paths?.getOrNull(3).orEmpty(),
            )
        }
    }
}
