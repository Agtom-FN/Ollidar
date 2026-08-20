package com.lidarscan.core.model

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ProjectManifestSerializationTest {

    private val json = Json { prettyPrint = true; ignoreUnknownKeys = true }

    @Test
    fun `manifest round-trips through JSON with all fields set`() {
        val original = ProjectManifest(
            name = "North Wing",
            sensor = SensorType.MID360,
            profile = WorkflowProfile.SURVEY,
            createdAtEpochMillis = 1_755_100_000_000L,
            appVersion = "0.1.0",
            pointCountEstimate = 4_200_000L,
            mountCalibrationId = "calib-abc123",
            crsEpsg = 32633,
        )

        val encoded = json.encodeToString(ProjectManifest.serializer(), original)
        val decoded = json.decodeFromString(ProjectManifest.serializer(), encoded)

        assertEquals(original, decoded)
    }

    @Test
    fun `manifest round-trips through JSON with only required fields`() {
        val original = ProjectManifest(
            name = "Bare Minimum",
            sensor = SensorType.COIN_D6,
            profile = WorkflowProfile.QUICK_SCAN,
            createdAtEpochMillis = 1_755_100_000_000L,
            appVersion = "0.1.0",
        )

        val encoded = json.encodeToString(ProjectManifest.serializer(), original)
        val decoded = json.decodeFromString(ProjectManifest.serializer(), encoded)

        assertEquals(original, decoded)
        assertEquals(1, decoded.schemaVersion)
        assertEquals(null, decoded.pointCountEstimate)
        assertEquals(null, decoded.mountCalibrationId)
        assertEquals(null, decoded.crsEpsg)
    }

    /**
     * ROUND 25 item 119. The STL-27L has to survive the manifest exactly as the
     * other two sensors do — the enum's **declared name** is what lands in the
     * JSON, so this pins `"STL27L"` as the stored string. Rename the constant
     * and this test fails, which is the point: `manifest.json` files are on
     * operators' phones.
     */
    @Test
    fun `an STL-27L manifest round-trips and stores the enum name verbatim`() {
        val original = ProjectManifest(
            name = "Stairwell",
            sensor = SensorType.STL27L,
            profile = WorkflowProfile.QUICK_SCAN,
            createdAtEpochMillis = 1_755_100_000_000L,
            appVersion = "0.9.10",
        )

        val encoded = json.encodeToString(ProjectManifest.serializer(), original)
        assertTrue("expected the enum NAME in the JSON, got:\n$encoded", encoded.contains("\"STL27L\""))

        val decoded = json.decodeFromString(ProjectManifest.serializer(), encoded)
        assertEquals(original, decoded)
        assertEquals(SensorType.STL27L, decoded.sensor)
        // Adding an enum VALUE is not a schema change — see SensorType's KDoc
        // for why bumping this would not have helped an older reader anyway.
        assertEquals(1, decoded.schemaVersion)
    }

    /**
     * ROUND 25 item 119, the direction that actually matters: a manifest
     * written **before** the STL-27L existed still decodes unchanged. Adding a
     * value cannot break a decoder, because it matches the string it finds
     * rather than enumerating the strings it might have found.
     */
    @Test
    fun `manifests written before the STL-27L existed still decode`() {
        val legacy = """
            {
              "schemaVersion": 1,
              "name": "Old Scan",
              "sensor": "COIN_D6",
              "profile": "QUICK_SCAN",
              "createdAtEpochMillis": 1755100000000,
              "appVersion": "0.9.0"
            }
        """.trimIndent()

        val decoded = json.decodeFromString(ProjectManifest.serializer(), legacy)

        assertEquals(SensorType.COIN_D6, decoded.sensor)
        assertEquals("Old Scan", decoded.name)
    }

    @Test
    fun `decoding tolerates unknown future fields for forward compatibility`() {
        val encodedWithExtraField = """
            {
              "schemaVersion": 1,
              "name": "Future Field",
              "sensor": "COIN_D6",
              "profile": "RESEARCH",
              "createdAtEpochMillis": 1755100000000,
              "appVersion": "0.1.0",
              "somethingAddedInSchemaVersionTwo": "value"
            }
        """.trimIndent()

        val decoded = json.decodeFromString(ProjectManifest.serializer(), encodedWithExtraField)

        assertEquals("Future Field", decoded.name)
        assertEquals(SensorType.COIN_D6, decoded.sensor)
        assertEquals(WorkflowProfile.RESEARCH, decoded.profile)
    }
}
