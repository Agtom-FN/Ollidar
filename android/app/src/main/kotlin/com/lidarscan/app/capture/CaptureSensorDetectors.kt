package com.lidarscan.app.capture

import com.lidarscan.app.net.ConnectionDebugSweeper
import com.lidarscan.app.net.ConnectionDebugTrace
import com.lidarscan.app.net.EthernetMonitor
import com.lidarscan.app.usb.D6AutoProbe
import com.lidarscan.app.usb.D6AutoProbeResult
import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.app.usb.Stl27lAutoProbe
import com.lidarscan.app.usb.Stl27lAutoProbeResult
import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.net.Mid360DetectionResult
import com.lidarscan.core.net.Mid360Detector
import com.lidarscan.core.net.SerialProbeRecord

/**
 * ROUND 5: the Android halves of [SensorAutoDetector] — the two probes the
 * Capture tab races on entry so a detected sensor goes straight to a live
 * preview with no wizard in between.
 *
 * ROUND 25 item 119: still exactly TWO racing detectors, and that is now a
 * rule rather than an accident. The serial one grew a second sensor (the
 * STL-27L) and absorbed it as a second RUNG inside itself — see
 * [SerialLidarAutoDetector] for why a third racing detector on the same USB
 * port would have been a bug, not a feature.
 *
 * Both reuse the AUTO-DETECT machinery that already exists and is already
 * tested/field-checked ([D6AutoProbe]'s `AA 55` signature read,
 * [com.lidarscan.app.net.UdpMid360Detector]'s heartbeat listen); nothing about
 * the probing changed in round 5, only *who asks* — the capture screen rather
 * than a wizard step.
 */

/**
 * ROUND 25 item 119 — what one baud-specific probe concluded about the single
 * attached serial port.
 *
 * Deliberately narrower than the two probes' own result types: the ladder
 * below only needs to know "yes / not mine / stop asking", and flattening the
 * two sealed hierarchies into one here is what lets the ordering be tested
 * without a USB stack anywhere near it.
 */
sealed interface SerialProbeOutcome {
    /** This step's sensor answered, and its port is open and ready for the engine. */
    data class Identified(val devicePath: String) : SerialProbeOutcome

    /** The port opened and was read; nothing of this step's shape arrived. Try the next step. */
    data object Declined : SerialProbeOutcome

    /**
     * The port could not be used at all — permission refused, or it would not
     * open. **Stops the ladder**: a different baud does not fix a refused
     * permission, and re-prompting the operator once per sensor is exactly the
     * dialog storm auto-detect exists to avoid.
     */
    data class Unusable(val reason: String) : SerialProbeOutcome
}

/** One rung of the serial ladder: "open the port at MY baud and tell me if MY sensor is there." */
interface SerialLidarProbeStep {
    val sensor: SensorType
    suspend fun probe(devicePath: String): SerialProbeOutcome
}

/**
 * ROUND 25 item 119 — **one** detector for every lidar that arrives as bytes on
 * a USB-serial port, walking a ladder of (sensor, baud) rungs in order.
 *
 * Was `D6UsbAutoDetector`, which probed for one sensor at one baud.
 *
 * ## Why one detector and not two racing ones
 *
 * `CaptureAutoConnectController.detectFirst` **races** its detectors — first
 * past the post wins and the rest are cancelled — which is right for the D6
 * and the Mid-360, because those two touch completely different hardware (a
 * USB serial port, a UDP socket) and neither can interfere with the other.
 *
 * Two serial detectors would not have that property. They would both call
 * `UsbManager.openDevice` on the *same* port, both call `setParameters` with
 * *different* divisors, and both attach a reader thread — on a device that
 * physically cannot be clocked at two rates at once. The winner would be
 * whichever coroutine got the CH340 last, the loser would report garbage, and
 * the operator would see a sensor that connects and streams nothing. So the
 * sequencing lives *inside* one detector, where it is a `for` loop rather than
 * a race.
 *
 * ## The ordering, and its one honest weakness
 *
 * D6 at 230 400 first, then STL-27L at 921 600. That order is item 119's, and
 * it is the conservative one for the installed base: the D6 is the sensor
 * every recorded scan in `captures/` came from, and it keeps its existing
 * probe, its existing window and its existing first-look behaviour untouched.
 *
 * The direction this ordering **provably** cannot get wrong is a D6 claimed as
 * an STL-27L. Reading a 230 400 device at 921 600 yields framing garbage, and
 * `Stl27lSignatureScanner` will not accept garbage: it demands `54 2C` *and* a
 * matching CRC8, ~1 chance in 16.7 million per byte offset, and
 * [com.lidarscan.app.usb.Stl27lAutoProbe] demands four such packets. That is
 * not going to happen by accident.
 *
 * The other direction is **not** proven, and saying so is better than implying
 * otherwise: an STL-27L read at 230 400 also yields garbage, and the D6 probe
 * accepts any adjacent `AA 55` — about 1 chance in 65 536 per byte offset,
 * across roughly 34 kB of a 1.5 s window. A misidentification there is
 * genuinely possible. It is not fixed by reordering (that only moves the risk
 * onto the sensor that has field history), and strengthening the D6 probe to
 * demand a full checksummed packet is a change to the one detection path that
 * is known to work in the field — not something to do blind, in the same round
 * that adds a sensor nobody has held. What item 119 ships instead is the
 * escape hatch: the Advanced sheet's manual row now lets the operator name
 * which serial lidar is on the port, and that choice bypasses this ladder
 * entirely.
 *
 * ## The port is never open twice
 *
 * Each rung's probe closes the connection it speculatively opened when it
 * declines (both [D6AutoProbe] and [com.lidarscan.app.usb.Stl27lAutoProbe]
 * document this), and the rungs run strictly one after another, so at most one
 * connection to the port exists at any instant — and exactly one is left open,
 * at the right baud, when a rung identifies.
 *
 * More than one serial device attached is still left to the manual panel,
 * unchanged from the AUTO-DETECT round: a rig with a D6 *and* a UM982
 * enumerates two CH340-class devices and guessing between them is how you hold
 * a GNSS receiver's port open on a coin flip.
 */
class SerialLidarAutoDetector(
    /** Device paths of the currently attached serial devices. A lambda so the ordering is JVM-testable. */
    private val attachedDevicePaths: () -> List<String>,
    /** The rungs, tried in order. Production order is D6 then STL-27L — see the class doc. */
    private val ladder: List<SerialLidarProbeStep>,
    /**
     * ROUND 25 item 118 (owner amendment): where one `[net-debug]` sweep is
     * written at the end of a detection run. Nullable and defaulted, so every
     * existing caller — and `SerialLidarAutoDetectorTest`, which proves the
     * ladder's ordering on a bare JVM — is untouched, and so the diagnostic
     * can never be the reason detection fails.
     */
    private val connectionDebug: ConnectionDebugSweeper? = null,
) : SensorAutoDetector {

    /**
     * The interface's single value, and the honest reading of it: **the sensor
     * this detector looks for FIRST**, not a promise about what it will return.
     *
     * `SensorAutoDetector.sensor` is not consumed anywhere in production —
     * `CaptureAutoConnectController` only ever calls [detect], and every
     * consumer downstream (the ViewModel's `_sensor`, the manifest, the status
     * line) reads [AutoDetection.sensor], which IS per-detection and therefore
     * always right. Widening the interface to a set would mean adding API that
     * nothing reads, to describe a property nothing asks about; the useful
     * thing to do instead is to write down what the existing value means.
     */
    override val sensor: SensorType = SensorType.COIN_D6

    override suspend fun detect(): AutoDetection? {
        val paths = runCatching { attachedDevicePaths() }.getOrDefault(emptyList())
        if (paths.size != 1) {
            // Item 118 amendment: "wrong number of serial devices" is itself a
            // detection outcome and used to be a silent `return null` — the
            // two-CH340 rig (a D6 *and* a UM982) looks from the outside
            // exactly like nothing being plugged in.
            ConnectionDebugTrace.noteSerialProbe(
                sensor = "serial-ladder",
                devicePath = "-",
                outcome = SerialProbeRecord.OUTCOME_UNUSABLE,
                detail = "${paths.size} serial devices attached; the ladder needs exactly 1",
            )
            logSweep()
            return null
        }
        val devicePath = paths.first()

        try {
            for (step in ladder) {
                // Each rung is recorded as it finishes, in order, because the
                // ORDER is half the diagnosis: an Unusable on rung 1 stops the
                // ladder, so the absence of a rung-2 line means "never ran"
                // rather than "declined".
                when (val outcome = step.probe(devicePath)) {
                    is SerialProbeOutcome.Identified -> {
                        note(step, devicePath, SerialProbeRecord.OUTCOME_IDENTIFIED)
                        return detectionFor(step.sensor, outcome.devicePath)
                    }

                    SerialProbeOutcome.Declined -> {
                        note(step, devicePath, SerialProbeRecord.OUTCOME_DECLINED)
                        // next rung, at the next baud
                    }

                    is SerialProbeOutcome.Unusable -> {
                        note(step, devicePath, SerialProbeRecord.OUTCOME_UNUSABLE, outcome.reason)
                        return null
                    }
                }
            }
            return null
        } finally {
            logSweep()
        }
    }

    private fun note(step: SerialLidarProbeStep, devicePath: String, outcome: String, detail: String = "") {
        ConnectionDebugTrace.noteSerialProbe(step.sensor.name, devicePath, outcome, detail)
    }

    /**
     * One rate-limited `[net-debug]` sweep per detection run, which is what
     * carries the [ConnectionDebugTrace] records above into the log.
     *
     * Wrapped and never allowed to throw: auto-detect deciding what sensor is
     * attached must not be able to fail because a diagnostic did.
     */
    private suspend fun logSweep() {
        val debug = connectionDebug ?: return
        runCatching { debug.logSweep(ConnectionDebugSweeper.TRIGGER_AUTO_DETECT) }
    }

    private fun detectionFor(sensor: SensorType, devicePath: String) = AutoDetection(
        sensor = sensor,
        transportHint = devicePath,
        label = "${sensor.displayName} · ${shortPath(devicePath)}",
        // Round 5 item 11: neither serial lidar has an IMU of its own, so the
        // phone is not an accessory here — it IS the trajectory. Said in the
        // detection line so it is on screen before recording starts.
        detail = "3D scan · phone-tracked (ARCore VIO supplies the pose)",
    )

    private fun shortPath(devicePath: String): String = devicePath.substringAfterLast('/')

    companion object {
        /**
         * The production ladder, wired to a real [D6UsbConnectionRegistry].
         *
         * A factory rather than a constructor default so the class itself has
         * no Android types in its signature — which is what lets
         * `SerialLidarAutoDetectorTest` prove the ordering on a bare JVM.
         */
        fun fromRegistry(
            registry: D6UsbConnectionRegistry,
            connectionDebug: ConnectionDebugSweeper? = null,
        ): SerialLidarAutoDetector =
            SerialLidarAutoDetector(
                attachedDevicePaths = { registry.findDrivers().map { it.device.deviceName } },
                ladder = listOf(
                    D6ProbeStep(registry),
                    Stl27lProbeStep(registry),
                ),
                connectionDebug = connectionDebug,
            )
    }
}

/** Rung 1: the COIN-D6's `AA 55` read at 230 400. Unchanged behaviour, moved behind an interface. */
class D6ProbeStep(
    private val registry: D6UsbConnectionRegistry,
    private val probe: D6AutoProbe = D6AutoProbe(registry),
) : SerialLidarProbeStep {

    override val sensor: SensorType = SensorType.COIN_D6

    override suspend fun probe(devicePath: String): SerialProbeOutcome {
        val driver = registry.findDrivers().firstOrNull { it.device.deviceName == devicePath }
            ?: return SerialProbeOutcome.Unusable("$devicePath is no longer attached")
        return when (val result = probe.probe(driver)) {
            is D6AutoProbeResult.Identified -> SerialProbeOutcome.Identified(result.devicePath)
            D6AutoProbeResult.NotIdentified -> SerialProbeOutcome.Declined
            D6AutoProbeResult.PermissionDenied -> SerialProbeOutcome.Unusable("USB permission denied")
            is D6AutoProbeResult.Error -> SerialProbeOutcome.Unusable(result.message)
        }
    }
}

/** ROUND 25 item 119 — rung 2: the STL-27L's CRC-checked LD packets, read at 921 600. */
class Stl27lProbeStep(
    private val registry: D6UsbConnectionRegistry,
    private val probe: Stl27lAutoProbe = Stl27lAutoProbe(registry),
) : SerialLidarProbeStep {

    override val sensor: SensorType = SensorType.STL27L

    override suspend fun probe(devicePath: String): SerialProbeOutcome {
        val driver = registry.findDrivers().firstOrNull { it.device.deviceName == devicePath }
            ?: return SerialProbeOutcome.Unusable("$devicePath is no longer attached")
        return when (val result = probe.probe(driver)) {
            is Stl27lAutoProbeResult.Identified -> SerialProbeOutcome.Identified(result.devicePath)
            Stl27lAutoProbeResult.NotIdentified -> SerialProbeOutcome.Declined
            Stl27lAutoProbeResult.PermissionDenied -> SerialProbeOutcome.Unusable("USB permission denied")
            is Stl27lAutoProbeResult.Error -> SerialProbeOutcome.Unusable(result.message)
        }
    }
}

/**
 * Mid-360 over USB-C Ethernet, found by its own once-a-second broadcast.
 *
 * The heartbeat carries the lidar's IP *and* the host IP the device is already
 * configured to stream to, which is the pair `EngineTarget.transportHint`
 * wants — so a Mid-360 that has been configured once is connectable with
 * nothing typed. [onFound] is where the app persists those addresses as capture
 * defaults (AUTO-DETECT §3's DataStore keys), kept as a callback so this class
 * has no opinion about storage.
 *
 * When the heartbeat's persisted host IP is **not** one of this phone's current
 * Ethernet addresses, the detection still reports (the operator needs to see
 * that a device is there) but names the mismatch in [AutoDetection.detail]: the
 * engine will not receive points until the phone holds that address, and
 * silently connecting into a black viewport is precisely the "manual IP entry
 * defeated the GUI" failure round 4 was about.
 */
class Mid360HeartbeatAutoDetector(
    private val detector: Mid360Detector,
    private val ethernetMonitor: EthernetMonitor,
    private val timeoutMs: Long = DEFAULT_TIMEOUT_MS,
    private val onFound: suspend (lidarIp: String, hostIp: String, serialNumber: String) -> Unit = { _, _, _ -> },
    /** ROUND 25 item 118 (owner amendment): one `[net-debug]` sweep per detection run. Nullable; never throws out. */
    private val connectionDebug: ConnectionDebugSweeper? = null,
) : SensorAutoDetector {

    override val sensor: SensorType = SensorType.MID360

    override suspend fun detect(): AutoDetection? {
        val result = detector.detect(timeoutMs)
        val heartbeat = (result as? Mid360DetectionResult.Found)?.heartbeat
        // Item 118 amendment: the sweep is written whichever way this went,
        // and BEFORE the early return, because the interesting run is the one
        // that found nothing — that is the owner's failure, and it is the run
        // that used to leave no trace at all beyond a preflight refusal.
        // The expected host comes from the beacon when there was one, and from
        // the wizard's default otherwise: the same rule Mid360Diagnosis uses.
        runCatching {
            connectionDebug?.logSweep(
                trigger = ConnectionDebugSweeper.TRIGGER_AUTO_DETECT,
                context = ConnectionDebugSweeper.SweepContext(
                    expectedHostIp = heartbeat?.persistedHostIp
                        ?: com.lidarscan.core.net.Mid360Settings.DEFAULT_HOST_IP,
                    heartbeatAgeMillis = if (heartbeat != null) 0L else null,
                ),
            )
        }
        if (heartbeat == null) return null
        onFound(heartbeat.lidarIp, heartbeat.persistedHostIp, heartbeat.serialNumber)

        val localAddresses = ethernetMonitor.state.value.addresses.map { it.ip }
        val hostMatches = heartbeat.persistedHostIp in localAddresses
        return AutoDetection(
            sensor = SensorType.MID360,
            transportHint = "${heartbeat.lidarIp}|${heartbeat.persistedHostIp}",
            label = "Mid-360 · ${heartbeat.lidarIp}",
            detail = if (hostMatches) {
                "SN ${heartbeat.serialNumber} · host ${heartbeat.persistedHostIp} ✓"
            } else {
                "SN ${heartbeat.serialNumber} · set this phone's static IP to " +
                    "${heartbeat.persistedHostIp}/24 — the address the lidar streams to"
            },
        )
    }

    private companion object {
        /** Same 5 s window the Mid-360 wizard uses; the device broadcasts every second. */
        const val DEFAULT_TIMEOUT_MS = 5_000L
    }
}

/**
 * ROUND 5 manual fallback: opens (and permissions) one serial port by its device
 * path, so the engine can be pointed at a device the operator picked by hand.
 *
 * The auto-detect path does not need this — [D6AutoProbe] leaves the port it
 * identified open, which is exactly what `RealEngineBridge.connect` requires.
 * The manual path skips the probe (that is the point: it is for the device the
 * probe would not or could not identify), so somebody has to open the port, and
 * this is that somebody.
 *
 * ROUND 25 item 119: [baud] defaults to the D6's rate, so every pre-existing
 * caller behaves exactly as it did. The manual panel passes the STL-27L's
 * 921 600 when that is what the operator picked.
 */
suspend fun openSerialPortByPath(
    registry: D6UsbConnectionRegistry,
    devicePath: String,
    baud: Int = D6UsbConnectionRegistry.BAUD_RATE,
): Result<Unit> {
    val driver = registry.findDrivers().firstOrNull { it.device.deviceName == devicePath }
        ?: return Result.failure(IllegalStateException("$devicePath is no longer attached"))
    if (!registry.hasPermission(driver)) {
        val granted = registry.requestPermission(driver)
        if (!granted) return Result.failure(IllegalStateException("USB permission denied for $devicePath"))
    }
    return runCatching { registry.open(driver, baud) }.map { }
}
