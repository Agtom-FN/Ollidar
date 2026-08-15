package com.lidarscan.core.plan

/**
 * B11 — a Kotlin mirror of `engine/include/scanengine/plan/plan_model.h`
 * (A12), the one structure the floor-plan viewer draws.
 *
 * A12's own header says this is "the ONE structure everything downstream
 * reads: the DXF writer, the PDF sheet writer, the Qt editor, **the Android
 * viewer**, and the tests", and that it is deliberately plain data so an app
 * can hold it without linking the implementation. That is exactly what happens
 * here: `plan_jni.cpp` runs `extract_floor_plan()` natively and marshals the
 * resulting `PlanModel` into these types; the DXF/PDF bytes are written by
 * A12's own writers, never re-derived in Kotlin.
 *
 * **Units and frame** are A12's, unchanged: metres, in the plan frame (the two
 * axes of the gravity-aligned cloud perpendicular to up). Scaling to paper
 * happens exactly once, inside A12's PDF writer — the Compose viewer scales to
 * *screen*, which is a different and non-authoritative transform.
 *
 * **Double, not float**, for the same reason A12 gives: a plan accumulates
 * least-squares fits and line intersections over tens of metres, and float32's
 * ~1 mm of slack at 20 m is the same order as the numbers being resolved.
 */

data class PlanVec2(val x: Double, val y: Double)

/** Mirror of `plan::WallEvidence`. Decides whether a room polygon is inset by half the thickness. */
enum class WallEvidence(val code: Int, val label: String) {
    /** One scanned face — the fitted line **is** the visible face, so thickness is an assumption. */
    SINGLE_FACE(0, "one face"),

    /** Both faces scanned — thickness is measured. */
    PAIRED_FACES(1, "measured"),
    ;

    companion object {
        fun fromCode(c: Int) = entries.firstOrNull { it.code == c } ?: SINGLE_FACE
    }
}

/** Mirror of `plan::OpeningKind`. Everything here is a **candidate** — A12's editor is where a human confirms. */
enum class OpeningKind(val code: Int, val label: String) {
    UNKNOWN(0, "Opening"),
    NARROW_GAP(1, "Narrow gap"),
    DOOR_CANDIDATE(2, "Door"),
    WINDOW_CANDIDATE(3, "Window"),
    WIDE_OPENING(4, "Wide opening"),
    ;

    companion object {
        fun fromCode(c: Int) = entries.firstOrNull { it.code == c } ?: UNKNOWN
    }
}

/** Mirror of `plan::SillCheck` — the window-sill re-slice verdict. */
enum class SillCheck(val code: Int, val label: String) {
    NOT_CHECKED(0, "sill not checked"),

    /** The lower band has no support anywhere on this wall — reported, never guessed. */
    NO_DATA(1, "no sill data"),
    OPEN_BELOW(2, "open below (door-like)"),
    SOLID_BELOW(3, "solid below (window-like)"),
    ;

    companion object {
        fun fromCode(c: Int) = entries.firstOrNull { it.code == c } ?: NOT_CHECKED
    }
}

data class WallSegment(
    val id: Int,
    val a: PlanVec2,
    val b: PlanVec2,
    val thicknessM: Double,
    val evidence: WallEvidence,
    val rmsResidualM: Double,
    val coverage: Double,
    val supportCells: Int,
    val confidence: Float,
    val snapped: Boolean,
) {
    val lengthM: Double get() = kotlin.math.hypot(b.x - a.x, b.y - a.y)
}

data class Opening(
    val id: Int,
    val wallId: Int,
    val a: PlanVec2,
    val b: PlanVec2,
    val widthM: Double,
    val kind: OpeningKind,
    val sill: SillCheck,
    val confidence: Float,
)

data class Room(
    val id: Int,
    val label: String,
    val polygon: List<PlanVec2>,
    val areaM2: Double,
    val perimeterM: Double,
    val centroid: PlanVec2,
    val confidence: Float,
    /**
     * False when the bounding cycle closed itself through a bridged opening or
     * an extended corner. The area is still reported — but it rests on an
     * inference rather than measured wall, and the viewer says so.
     */
    val fullyMeasured: Boolean,
)

data class PlanBounds(
    val minX: Double,
    val minY: Double,
    val maxX: Double,
    val maxY: Double,
    val valid: Boolean,
) {
    val width: Double get() = if (valid) maxX - minX else 0.0
    val height: Double get() = if (valid) maxY - minY else 0.0
}

data class PlanStats(
    val pointsConsidered: Long,
    val pointsInBand: Long,
    val gridW: Int,
    val gridH: Int,
    val occupiedCells: Int,
    val ransacLines: Int,
    val snappedWalls: Int,
    val pairedWalls: Int,
    val dominantAngleRad: Double,
    val totalWallLengthM: Double,
    val totalRoomAreaM2: Double,
)

data class PlanModel(
    val walls: List<WallSegment>,
    val openings: List<Opening>,
    val rooms: List<Room>,
    val bounds: PlanBounds,
    val stats: PlanStats,
    val sliceZMinM: Double,
    val sliceZMaxM: Double,
    val gridResM: Double,
) {
    val isEmpty: Boolean get() = walls.isEmpty() && rooms.isEmpty()

    /**
     * The one diagnostic worth putting on screen when a plan comes back empty
     * or wrong, because it is the knob A12 says has to track point density:
     * `SliceOptions::min_cell_points` defaults to 3, and
     * [PlanStats.occupiedCells] "is how you see that it is wrong".
     */
    fun emptyDiagnosis(): String = when {
        stats.pointsConsidered == 0L ->
            "The cloud is empty — post-process the capture before extracting a plan."
        stats.pointsInBand == 0L ->
            "No points fell in the ${"%.2f".format(sliceZMinM)}–${"%.2f".format(sliceZMaxM)} m slice band. If the " +
                "capture's floor is not at z = 0, move the band; the cloud is in the session's own local frame, not a levelled one."
        stats.occupiedCells < 50 ->
            "Only ${stats.occupiedCells} occupied cells in the slice — too sparse for RANSAC to find a wall. A heavily " +
                "decimated cloud needs a lower min-cell-points, or a coarser grid."
        else ->
            "Points were found but no wall survived the fit. Try a coarser grid, a taller band, or turn orthogonality " +
                "snapping off if the space genuinely is not rectilinear."
    }
}

/** Options handed to A12's `extract_floor_plan()`. Defaults match `SliceOptions`/`WallOptions` exactly. */
data class PlanOptions(
    val zMinM: Float = 1.0f,
    val zMaxM: Float = 1.5f,
    val gridResM: Float = 0.02f,
    val snapOrthogonal: Boolean = true,
    val snapToleranceDeg: Float = 7.0f,
    /**
     * Points per cell before a cell counts as occupied. **Three**, not one:
     * A12 measured that accepting the noise tails widens every face to ~5 cells,
     * which is wider than the 150 mm partition the pipeline is trying to resolve
     * into two faces, and RANSAC then fits a third line down the middle of the wall.
     */
    val minCellPoints: Int = 3,
    val windowSillCheck: Boolean = true,
    val detectRooms: Boolean = true,
    val detectOpenings: Boolean = true,
)
