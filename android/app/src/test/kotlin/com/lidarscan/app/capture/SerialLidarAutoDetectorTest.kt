package com.lidarscan.app.capture

import com.lidarscan.core.model.SensorType
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 25 item 119 — **the order the two serial probes run in, proved.**
 *
 * The rule this suite defends is not "the STL-27L works" (no such hardware
 * exists to work). It is narrower and far more important: *the two probes must
 * never run at the same time on the same port, and the second must only run
 * when the first has genuinely declined.* Both bauds cannot be set on one
 * CH340 at once, so a ladder that ran its rungs eagerly — or that raced them,
 * which is what a second `SensorAutoDetector` in
 * `CaptureAutoConnectController`'s list would have done — would produce a
 * device that connects and streams nothing.
 *
 * That ordering is testable on a bare JVM precisely because
 * [SerialLidarAutoDetector] takes its rungs as [SerialLidarProbeStep]s and its
 * device list as a lambda: no `UsbManager`, no `UsbSerialDriver`, no
 * Robolectric. The production wiring is
 * `SerialLidarAutoDetector.fromRegistry`.
 */
class SerialLidarAutoDetectorTest {

    /** A rung that answers however the test says, and records that it was asked. */
    private class FakeStep(
        override val sensor: SensorType,
        private val log: MutableList<SensorType>,
        private val outcome: (String) -> SerialProbeOutcome,
    ) : SerialLidarProbeStep {
        override suspend fun probe(devicePath: String): SerialProbeOutcome {
            log += sensor
            return outcome(devicePath)
        }
    }

    private fun detector(
        paths: List<String> = listOf(PORT),
        vararg steps: SerialLidarProbeStep,
    ) = SerialLidarAutoDetector(attachedDevicePaths = { paths }, ladder = steps.toList())

    @Test
    fun `a COIN-D6 answers on the first rung and the STL-27L rung is never opened`() {
        val asked = CopyOnWriteArrayList<SensorType>()
        val d = detector(
            steps = arrayOf(
                FakeStep(SensorType.COIN_D6, asked) { SerialProbeOutcome.Identified(it) },
                FakeStep(SensorType.STL27L, asked) { error("must not be reached") },
            ),
        )

        val found = runBlocking { d.detect() }

        assertNotNull(found)
        assertEquals(SensorType.COIN_D6, found!!.sensor)
        assertEquals(PORT, found.transportHint)
        assertEquals(listOf(SensorType.COIN_D6), asked.toList())
    }

    @Test
    fun `the STL-27L rung runs only after the D6 rung has declined`() {
        val asked = CopyOnWriteArrayList<SensorType>()
        val d = detector(
            steps = arrayOf(
                FakeStep(SensorType.COIN_D6, asked) { SerialProbeOutcome.Declined },
                FakeStep(SensorType.STL27L, asked) { SerialProbeOutcome.Identified(it) },
            ),
        )

        val found = runBlocking { d.detect() }

        assertNotNull(found)
        assertEquals(SensorType.STL27L, found!!.sensor)
        assertEquals(PORT, found.transportHint)
        // Order, not just membership: the D6 was asked first and the STL-27L
        // second. A racing implementation would fail this even when it happened
        // to return the right sensor.
        assertEquals(listOf(SensorType.COIN_D6, SensorType.STL27L), asked.toList())
    }

    @Test
    fun `the detection an STL-27L produces names the STL-27L on screen`() {
        val d = detector(
            paths = listOf("/dev/bus/usb/001/007"),
            steps = arrayOf(
                FakeStep(SensorType.COIN_D6, mutableListOf()) { SerialProbeOutcome.Declined },
                FakeStep(SensorType.STL27L, mutableListOf()) { SerialProbeOutcome.Identified(it) },
            ),
        )

        val found = runBlocking { d.detect() }!!

        assertEquals("LDROBOT STL-27L · 007", found.label)
        // Neither serial lidar carries an IMU, so both say the same true thing
        // about where the trajectory comes from.
        assertTrue(found.detail!!.contains("phone-tracked"))
    }

    @Test
    fun `nothing on the port means every rung is tried and nothing is claimed`() {
        val asked = CopyOnWriteArrayList<SensorType>()
        val d = detector(
            steps = arrayOf(
                FakeStep(SensorType.COIN_D6, asked) { SerialProbeOutcome.Declined },
                FakeStep(SensorType.STL27L, asked) { SerialProbeOutcome.Declined },
            ),
        )

        assertNull(runBlocking { d.detect() })
        assertEquals(listOf(SensorType.COIN_D6, SensorType.STL27L), asked.toList())
    }

    @Test
    fun `an unusable port stops the ladder rather than re-prompting at the next baud`() {
        // A refused USB permission is the case this protects: reopening the same
        // port at 921600 would put a second system dialog in front of somebody
        // who has already said no.
        val asked = CopyOnWriteArrayList<SensorType>()
        val d = detector(
            steps = arrayOf(
                FakeStep(SensorType.COIN_D6, asked) { SerialProbeOutcome.Unusable("USB permission denied") },
                FakeStep(SensorType.STL27L, asked) { error("must not be reached") },
            ),
        )

        assertNull(runBlocking { d.detect() })
        assertEquals(listOf(SensorType.COIN_D6), asked.toList())
    }

    @Test
    fun `no attached device probes nothing at all`() {
        val asked = CopyOnWriteArrayList<SensorType>()
        val d = detector(
            paths = emptyList(),
            steps = arrayOf(FakeStep(SensorType.COIN_D6, asked) { error("must not be reached") }),
        )

        assertNull(runBlocking { d.detect() })
        assertTrue(asked.isEmpty())
    }

    @Test
    fun `two attached devices are left to the manual panel, unchanged from round 5`() {
        // A rig with a lidar AND a UM982 enumerates two CH340-class devices;
        // guessing between them is how you hold a GNSS receiver's port open on
        // a coin flip.
        val asked = CopyOnWriteArrayList<SensorType>()
        val d = detector(
            paths = listOf("/dev/bus/usb/001/003", "/dev/bus/usb/001/004"),
            steps = arrayOf(FakeStep(SensorType.COIN_D6, asked) { error("must not be reached") }),
        )

        assertNull(runBlocking { d.detect() })
        assertTrue(asked.isEmpty())
    }

    @Test
    fun `an enumeration that throws is reported as nothing found, never as a crash`() {
        val d = SerialLidarAutoDetector(
            attachedDevicePaths = { error("USB service unavailable") },
            ladder = listOf(FakeStep(SensorType.COIN_D6, mutableListOf()) { error("must not be reached") }),
        )
        assertNull(runBlocking { d.detect() })
    }

    private companion object {
        const val PORT = "/dev/bus/usb/001/003"
    }
}
