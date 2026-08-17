package com.lidarscan.app.capture

import com.lidarscan.app.net.EthernetMonitor
import com.lidarscan.app.usb.D6AutoProbe
import com.lidarscan.app.usb.D6AutoProbeResult
import com.lidarscan.app.usb.D6UsbConnectionRegistry
import com.lidarscan.core.capture.AutoDetection
import com.lidarscan.core.capture.SensorAutoDetector
import com.lidarscan.core.model.SensorType
import com.lidarscan.core.net.Mid360DetectionResult
import com.lidarscan.core.net.Mid360Detector

/**
 * ROUND 5: the Android halves of [SensorAutoDetector] — the two probes the
 * Capture tab races on entry so a detected sensor goes straight to a live
 * preview with no wizard in between.
 *
 * Both reuse the AUTO-DETECT machinery that already exists and is already
 * tested/field-checked ([D6AutoProbe]'s `AA 55` signature read,
 * [com.lidarscan.app.net.UdpMid360Detector]'s heartbeat listen); nothing about
 * the probing changed in round 5, only *who asks* — the capture screen rather
 * than a wizard step.
 */

/**
 * D6 over USB-C serial.
 *
 * On success the port is **left open** in [registry] with permission granted
 * (that is [D6AutoProbe]'s documented contract), which is exactly what
 * `RealEngineBridge.connect(EngineTarget(COIN_D6, devicePath))` needs — it
 * refuses to connect to a path the registry has no open connection for. So
 * "detected" and "connectable" are the same event here, with no second open.
 *
 * More than one serial device attached is left to the manual wizard, unchanged
 * from the AUTO-DETECT round: a rig with a D6 *and* a UM982 enumerates two
 * CH340-class devices and guessing between them is how you hold a GNSS
 * receiver's port open on a coin flip.
 */
class D6UsbAutoDetector(
    private val registry: D6UsbConnectionRegistry,
    private val probe: D6AutoProbe = D6AutoProbe(registry),
) : SensorAutoDetector {

    override val sensor: SensorType = SensorType.COIN_D6

    override suspend fun detect(): AutoDetection? {
        val drivers = registry.findDrivers()
        if (drivers.size != 1) return null
        val driver = drivers.first()
        return when (val result = probe.probe(driver)) {
            is D6AutoProbeResult.Identified -> AutoDetection(
                sensor = SensorType.COIN_D6,
                transportHint = result.devicePath,
                label = "COIN-D6 · ${shortPath(result.devicePath)}",
                // Round 5 item 11: the D6 has no IMU of its own, so the phone
                // is not an accessory here — it IS the trajectory. Said in the
                // detection line so it is on screen before recording starts.
                detail = "3D scan · phone-tracked (ARCore VIO supplies the pose)",
            )

            D6AutoProbeResult.NotIdentified,
            D6AutoProbeResult.PermissionDenied,
            is D6AutoProbeResult.Error,
            -> null
        }
    }

    private fun shortPath(devicePath: String): String = devicePath.substringAfterLast('/')
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
) : SensorAutoDetector {

    override val sensor: SensorType = SensorType.MID360

    override suspend fun detect(): AutoDetection? {
        val result = detector.detect(timeoutMs)
        val heartbeat = (result as? Mid360DetectionResult.Found)?.heartbeat ?: return null
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
 */
suspend fun openSerialPortByPath(registry: D6UsbConnectionRegistry, devicePath: String): Result<Unit> {
    val driver = registry.findDrivers().firstOrNull { it.device.deviceName == devicePath }
        ?: return Result.failure(IllegalStateException("$devicePath is no longer attached"))
    if (!registry.hasPermission(driver)) {
        val granted = registry.requestPermission(driver)
        if (!granted) return Result.failure(IllegalStateException("USB permission denied for $devicePath"))
    }
    return runCatching { registry.open(driver) }.map { }
}
