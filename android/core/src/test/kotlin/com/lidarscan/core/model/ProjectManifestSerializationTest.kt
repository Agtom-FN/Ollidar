package com.lidarscan.core.model

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
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
