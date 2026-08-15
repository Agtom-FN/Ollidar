package com.lidarscan.app.engine

import com.lidarscan.core.plan.Opening
import com.lidarscan.core.plan.OpeningKind
import com.lidarscan.core.plan.PlanBounds
import com.lidarscan.core.plan.PlanModel
import com.lidarscan.core.plan.PlanStats
import com.lidarscan.core.plan.PlanVec2
import com.lidarscan.core.plan.Room
import com.lidarscan.core.plan.SillCheck
import com.lidarscan.core.plan.WallEvidence
import com.lidarscan.core.plan.WallSegment

/**
 * B11: decodes `processing_jni.cpp`'s flat plan arrays into `:core`'s
 * [PlanModel].
 *
 * The strides are the contract between the two files and are asserted by
 * `PlanDecodeTest` on synthetic arrays — a transposed or shifted layout is the
 * failure mode a flat-array marshalling has instead of a wrong JNI descriptor,
 * and it is a much cheaper one to catch.
 */
object NativePlanArrays {

    const val WALL_D_STRIDE = 8
    const val WALL_I_STRIDE = 4
    const val OPENING_D_STRIDE = 6
    const val OPENING_I_STRIDE = 4
    const val ROOM_D_STRIDE = 5
    const val ROOM_I_STRIDE = 3

    const val SUMMARY_LEN = 19
    const val SUMMARY_MIN_X = 0
    const val SUMMARY_MIN_Y = 1
    const val SUMMARY_MAX_X = 2
    const val SUMMARY_MAX_Y = 3
    const val SUMMARY_BOUNDS_VALID = 4
    const val SUMMARY_SLICE_Z_MIN = 5
    const val SUMMARY_SLICE_Z_MAX = 6
    const val SUMMARY_GRID_RES = 7
    const val SUMMARY_POINTS_CONSIDERED = 8
    const val SUMMARY_POINTS_IN_BAND = 9
    const val SUMMARY_GRID_W = 10
    const val SUMMARY_GRID_H = 11
    const val SUMMARY_OCCUPIED_CELLS = 12
    const val SUMMARY_RANSAC_LINES = 13
    const val SUMMARY_SNAPPED_WALLS = 14
    const val SUMMARY_PAIRED_WALLS = 15
    const val SUMMARY_DOMINANT_ANGLE_RAD = 16
    const val SUMMARY_TOTAL_WALL_LENGTH = 17
    const val SUMMARY_TOTAL_ROOM_AREA = 18

    fun decode(
        wallsD: DoubleArray,
        wallsI: IntArray,
        openingsD: DoubleArray,
        openingsI: IntArray,
        roomsD: DoubleArray,
        roomsI: IntArray,
        roomPolygons: DoubleArray,
        roomLabels: Array<String>,
        summary: DoubleArray,
    ): PlanModel {
        val wallCount = minOf(wallsD.size / WALL_D_STRIDE, wallsI.size / WALL_I_STRIDE)
        val walls = (0 until wallCount).map { i ->
            val d = i * WALL_D_STRIDE
            val n = i * WALL_I_STRIDE
            WallSegment(
                id = wallsI[n],
                a = PlanVec2(wallsD[d], wallsD[d + 1]),
                b = PlanVec2(wallsD[d + 2], wallsD[d + 3]),
                thicknessM = wallsD[d + 4],
                evidence = WallEvidence.fromCode(wallsI[n + 1]),
                rmsResidualM = wallsD[d + 5],
                coverage = wallsD[d + 6],
                supportCells = wallsI[n + 2],
                confidence = wallsD[d + 7].toFloat(),
                snapped = wallsI[n + 3] != 0,
            )
        }

        val openingCount = minOf(openingsD.size / OPENING_D_STRIDE, openingsI.size / OPENING_I_STRIDE)
        val openings = (0 until openingCount).map { i ->
            val d = i * OPENING_D_STRIDE
            val n = i * OPENING_I_STRIDE
            Opening(
                id = openingsI[n],
                wallId = openingsI[n + 1],
                a = PlanVec2(openingsD[d], openingsD[d + 1]),
                b = PlanVec2(openingsD[d + 2], openingsD[d + 3]),
                widthM = openingsD[d + 4],
                kind = OpeningKind.fromCode(openingsI[n + 2]),
                sill = SillCheck.fromCode(openingsI[n + 3]),
                confidence = openingsD[d + 5].toFloat(),
            )
        }

        val roomCount = minOf(roomsD.size / ROOM_D_STRIDE, roomsI.size / ROOM_I_STRIDE)
        var polyCursor = 0
        val rooms = (0 until roomCount).map { i ->
            val d = i * ROOM_D_STRIDE
            val n = i * ROOM_I_STRIDE
            val vertexCount = roomsI[n + 2]
            val poly = ArrayList<PlanVec2>(vertexCount)
            for (v in 0 until vertexCount) {
                val at = polyCursor + v * 2
                if (at + 1 < roomPolygons.size) poly.add(PlanVec2(roomPolygons[at], roomPolygons[at + 1]))
            }
            polyCursor += vertexCount * 2
            Room(
                id = roomsI[n],
                label = roomLabels.getOrElse(i) { "R${roomsI[n]}" },
                polygon = poly,
                areaM2 = roomsD[d],
                perimeterM = roomsD[d + 1],
                centroid = PlanVec2(roomsD[d + 2], roomsD[d + 3]),
                confidence = roomsD[d + 4].toFloat(),
                fullyMeasured = roomsI[n + 1] != 0,
            )
        }

        fun s(i: Int) = summary.getOrElse(i) { 0.0 }
        return PlanModel(
            walls = walls,
            openings = openings,
            rooms = rooms,
            bounds = PlanBounds(
                minX = s(SUMMARY_MIN_X),
                minY = s(SUMMARY_MIN_Y),
                maxX = s(SUMMARY_MAX_X),
                maxY = s(SUMMARY_MAX_Y),
                valid = s(SUMMARY_BOUNDS_VALID) != 0.0,
            ),
            stats = PlanStats(
                pointsConsidered = s(SUMMARY_POINTS_CONSIDERED).toLong(),
                pointsInBand = s(SUMMARY_POINTS_IN_BAND).toLong(),
                gridW = s(SUMMARY_GRID_W).toInt(),
                gridH = s(SUMMARY_GRID_H).toInt(),
                occupiedCells = s(SUMMARY_OCCUPIED_CELLS).toInt(),
                ransacLines = s(SUMMARY_RANSAC_LINES).toInt(),
                snappedWalls = s(SUMMARY_SNAPPED_WALLS).toInt(),
                pairedWalls = s(SUMMARY_PAIRED_WALLS).toInt(),
                dominantAngleRad = s(SUMMARY_DOMINANT_ANGLE_RAD),
                totalWallLengthM = s(SUMMARY_TOTAL_WALL_LENGTH),
                totalRoomAreaM2 = s(SUMMARY_TOTAL_ROOM_AREA),
            ),
            sliceZMinM = s(SUMMARY_SLICE_Z_MIN),
            sliceZMaxM = s(SUMMARY_SLICE_Z_MAX),
            gridResM = s(SUMMARY_GRID_RES),
        )
    }
}
