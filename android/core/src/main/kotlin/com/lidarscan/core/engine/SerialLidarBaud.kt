package com.lidarscan.core.engine

import com.lidarscan.core.model.SensorType

/**
 * ROUND 25 item 119 — **the one place that says how fast each serial lidar
 * talks.**
 *
 * Two entirely separate pieces of code have to agree on this number, and until
 * item 119 there was only one sensor so they could not disagree:
 *
 *  * `com.lidarscan.app.usb.D6UsbConnectionRegistry.open` sets the *host's*
 *    UART divisor — this is what the CH340/CP210x actually clocks at;
 *  * `RealEngineBridge.connect` passes `serial_baud` into
 *    `scan_engine_add_device`, which is what the engine records and reports.
 *
 * If those two ever differ, the failure is not a clean error: the port opens,
 * the engine reports a happy device, and the operator watches a black viewport
 * while the checksum pass rate sits near zero. So the mapping lives here,
 * in `:core`, where both modules can see it, rather than as a literal at
 * either call site.
 *
 * Sensors, not device kinds: the app chooses a [SensorType] long before it has
 * an engine handle, and the JNI's `kind` is derived from the same value.
 */
object SerialLidarBaud {

    /**
     * COIN-D6: 230 400 8N1. Field-verified — this is the rate every recorded
     * scan in `captures/` was taken at, and it is what `D6UsbConnectionRegistry`
     * has always opened with. Do not "round it up".
     */
    const val COIN_D6: Int = 230_400

    /**
     * LDROBOT STL-27L: 921 600 8N1.
     *
     * PROTOCOL-DERIVED, NOT OBSERVED — no STL-27L hardware exists on this
     * project. The public LD-series references give 921 600 for the STL-27L
     * (the LD06/LD19 in the same family run 230 400, which is exactly the trap
     * this constant exists to keep somebody out of), and the engine's
     * `stl27l::kDefaultBaud` carries the same number. A ~92 kB/s payload at
     * ~1800 packets/s is about 92 % duty on a 921 600 8N1 link, which is a
     * consistency check on the figure but not a measurement of it.
     */
    const val STL27L: Int = 921_600

    /**
     * The rate to open a port at for [sensor], or **null when the sensor is not
     * on a serial port at all**.
     *
     * Null rather than a fallback number: the Mid-360 is a UDP device over
     * USB-C Ethernet and has no baud in any meaningful sense. Handing back a
     * plausible-looking default would let a caller open a serial port for a
     * sensor that does not have one, and the whole point of this object is that
     * a wrong baud fails silently.
     */
    fun forSensorOrNull(sensor: SensorType): Int? = when (sensor) {
        SensorType.COIN_D6 -> COIN_D6
        SensorType.STL27L -> STL27L
        SensorType.MID360 -> null
    }

    /** True when [sensor] is reached over USB serial rather than Ethernet. */
    fun isSerial(sensor: SensorType): Boolean = forSensorOrNull(sensor) != null
}
