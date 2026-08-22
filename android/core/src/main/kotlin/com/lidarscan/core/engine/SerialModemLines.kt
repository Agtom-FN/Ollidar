package com.lidarscan.core.engine

import com.lidarscan.core.model.SensorType

/**
 * ROUND 32 item 178(a) — **the two wires nobody had thought about.**
 *
 * The owner's 0.9.16 retest: the STL-27L is **spinning**, and the line is
 * silent. 42 bytes in two seconds, 552 in twenty-five, `first1=00`, and not one
 * `54 2C` header, ever. Not garbled — *absent*. A sensor that is powered,
 * enabled and rotating while its host receives nothing is not a baud problem
 * and it is not a protocol problem. Something is holding the data path down.
 *
 * `D6UsbConnectionRegistry.open` ended with, verbatim, `port.setDTR(false)` —
 * and never touched RTS at all, which usb-serial-for-android leaves de-asserted
 * from `open()`. So **both** modem control lines were low on every port this
 * app has ever opened. On a bare COIN-D6 that is invisible, because its adapter
 * ignores them and every scan in `captures/` proves it. The STL-27L arrives on
 * a **CH340 dev-kit adapter board** (`1a86:7523`, product string `USB Serial`),
 * and that class of board routinely wires DTR and/or RTS to the sensor's enable
 * or to the level shifter's output-enable. Low means the board is listening to
 * a spinning sensor and telling the host nothing.
 *
 * ## Why this is per-sensor and not global
 *
 * The obvious fix is "assert both lines on every open", and it is the wrong
 * one. The COIN-D6 path currently sets DTR **false explicitly** — that is a
 * deliberate line of code, it has field history behind it across a hundred
 * recorded scans, and on some adapters DTR is a *reset* line where asserting it
 * holds the device down. Changing the one path that works, to fix the one that
 * does not, would risk trading a known failure for an unknown one on the
 * sensor the owner actually scans with.
 *
 * So the D6 keeps exactly the state it has always had, stated here rather than
 * left implicit, and the STL-27L gets both lines asserted. If the retest proves
 * assertion is harmless, [COIN_D6] is one boolean pair away from joining it —
 * and the `[net-debug]` line now records which state each open used, so that
 * decision can be made from a log instead of from an argument.
 */
object SerialModemLines {

    /**
     * DTR and RTS as the host will drive them, immediately after
     * `setParameters`.
     */
    data class State(val dtr: Boolean, val rts: Boolean) {
        /** For the `[net-debug]` port-open line. `dtr=1 rts=1`. */
        val log: String get() = "dtr=${if (dtr) 1 else 0} rts=${if (rts) 1 else 0}"
    }

    /**
     * The COIN-D6: **unchanged**, and unchanged on purpose. This is the state
     * every recorded capture in `captures/` was taken with.
     */
    val COIN_D6 = State(dtr = false, rts = false)

    /**
     * The STL-27L: both asserted. Item 178(a).
     *
     * The dev-kit board is the thing being driven here, not the sensor — the
     * STL-27L itself is a three-wire device (power, ground, TX) with no modem
     * control of its own, so there is nothing on the lidar these lines could
     * confuse. They only reach the adapter, which is the part that is refusing
     * to forward.
     */
    val STL27L = State(dtr = true, rts = true)

    /** Which state to open a port in for [sensor]. Null for a sensor that has no serial port. */
    fun forSensorOrNull(sensor: SensorType): State? = when (sensor) {
        SensorType.COIN_D6 -> COIN_D6
        SensorType.STL27L -> STL27L
        SensorType.MID360 -> null
    }
}
