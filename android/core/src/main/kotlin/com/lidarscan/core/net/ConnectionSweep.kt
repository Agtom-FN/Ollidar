package com.lidarscan.core.net

/**
 * ROUND 25 item 118, **owner amendment** — the connection-detection debug
 * sweep: everything the OS will tell us about the chain from the USB port to
 * a heartbeat, rendered so it can be read at a glance in a field log.
 *
 * ## Why this exists on top of [Mid360Diagnosis]
 *
 * Item 118 shipped the *operator's* answer: a ladder that names the rung the
 * chain is stuck on in six words. It works, and it is deliberately thin — an
 * operator standing in a stairwell needs an instruction, not a dump.
 *
 * The owner's Acer HY41-T9 hub is the case that shows what the thin answer
 * costs the person reading the log a week later. The hub did not work; the app
 * said `No Ethernet adapter found.`; and from that line nobody — not the
 * operator, not the owner, not whoever reads the capture log afterwards — can
 * tell which of these three actually happened:
 *
 *  1. **the hub was never enumerated on USB at all** (dead cable, dead port,
 *     dead hub) — nothing downstream of the connector ever ran;
 *  2. **the hub enumerated, and no network interface appeared** — the kernel
 *     has no driver for the Ethernet function, or the function is browning out
 *     on bus power. The hub is *there*, in `getDeviceList()`, with its
 *     interface descriptors, and the OS still produced no `eth*`;
 *  3. **an interface came up on the wrong subnet** — the chain is entirely
 *     healthy and the addressing is wrong, which looks like silence and is not.
 *
 * Case 1 and case 2 are one string apart on screen and a whole site visit
 * apart in the field. The evidence that splits them is free and was already
 * being thrown away: the **USB interface descriptors**. A CDC/RNDIS Ethernet
 * function announces itself as interface class 0x02/0x0A (or the RNDIS
 * `0xEF/0x04/0x01` misc triple), and that is what distinguishes "this hub has
 * an Ethernet function the kernel refused to drive" from "this hub is a hub
 * and a card reader and nothing else". [UsbDeviceRecord.interfaces] is
 * therefore the load-bearing field of this whole file.
 *
 * ## Why the formatter is in `:core`
 *
 * Because it is the part that has to be *right*, and `UsbDevice`,
 * `NetworkInterface` and `ConnectivityManager` cannot be constructed in a unit
 * test. Everything here is a plain data class; the Android side
 * (`app/.../net/ConnectionDebugSweeper.kt`) does nothing but copy platform
 * objects into these records, so the three cases above are asserted against
 * synthetic device lists on a bare JVM and stay asserted.
 *
 * ## Wording law
 *
 * [com.lidarscan.core.WordingLaw] does **not** apply to anything in this file,
 * and that is deliberate rather than an oversight. Every string here is
 * developer-mode diagnostic output — read by someone debugging a connection,
 * never shown to an operator — and the entire lesson of rounds 17–22 is that a
 * diagnostic line cannot carry too many numbers. Do not "fix" these into six
 * words. The six-word law governs the wizard ([Mid360Diagnosis]'s constants),
 * which is a different surface with a different reader.
 */

/**
 * One USB interface descriptor, as the OS reports it.
 *
 * This is the record that answers "is there an Ethernet function inside this
 * device at all". A hub, a card reader and an Ethernet dongle behind one
 * USB-C plug are three interfaces on (possibly) one device, and only the
 * interface class says which is which — the device-level class is very often
 * `0x00` ("see the interfaces") and tells you nothing.
 */
data class UsbInterfaceRecord(
    val index: Int,
    val interfaceClass: Int,
    val interfaceSubclass: Int,
    val interfaceProtocol: Int,
    val endpointCount: Int = 0,
) {
    /** Human name for [interfaceClass], for the log line. Unknown classes render as their hex. */
    val className: String get() = UsbClasses.name(interfaceClass)

    /**
     * Does this interface look like a network function?
     *
     * Three shapes cover essentially every USB Ethernet device sold:
     *
     *  * **CDC control (0x02)** — ECM/NCM/EEM control interfaces;
     *  * **CDC data (0x0A)** — the bulk-data half of the same;
     *  * **RNDIS**, which Microsoft shipped as the miscellaneous-device triple
     *    `0xEF / 0x04 / 0x01` and which is what a great many cheap USB-C hubs
     *    still present.
     *
     * A vendor-specific (`0xFF`) Ethernet chipset — AX88179 and RTL8153 both
     * do this — will **not** match, and that is honest rather than a gap: for
     * those, the VID:PID on the same line is the identification, and the whole
     * point of printing the full descriptor set is that the reader can see
     * both. This flag makes the verdict *more* specific when it fires and
     * never less specific when it does not.
     */
    val isNetworkFunction: Boolean
        get() = interfaceClass == UsbClasses.CDC_CONTROL ||
            interfaceClass == UsbClasses.CDC_DATA ||
            (interfaceClass == UsbClasses.MISCELLANEOUS && interfaceSubclass == 0x04 && interfaceProtocol == 0x01)

    /** `0: class=0x02(cdc-control) sub=0x06 proto=0x00 eps=1` */
    fun describe(): String = buildString {
        append(index).append(": class=").append(hex2(interfaceClass)).append('(').append(className).append(')')
        append(" sub=").append(hex2(interfaceSubclass))
        append(" proto=").append(hex2(interfaceProtocol))
        append(" eps=").append(endpointCount)
        if (isNetworkFunction) append(" NET")
    }
}

/**
 * One USB device, as the OS enumerates it.
 *
 * [productName] and [manufacturerName] are null on plenty of devices (they
 * require reading string descriptors the device may simply not have), which is
 * exactly why [vendorId]/[productId] are not optional: the VID:PID always
 * exists and is the thing that can be looked up against a kernel driver table.
 */
data class UsbDeviceRecord(
    val deviceName: String,
    val vendorId: Int,
    val productId: Int,
    val productName: String? = null,
    val manufacturerName: String? = null,
    val deviceClass: Int = 0,
    val deviceSubclass: Int = 0,
    val deviceProtocol: Int = 0,
    val interfaces: List<UsbInterfaceRecord> = emptyList(),
) {
    /** `2109:0813`, lower-case hex, the form every driver table and lsusb line uses. */
    val ids: String get() = hex4(vendorId) + ":" + hex4(productId)

    /** True when at least one interface announces a network function — see [UsbInterfaceRecord.isNetworkFunction]. */
    val hasNetworkFunction: Boolean get() = interfaces.any { it.isNetworkFunction }

    /**
     * The name half, for [Mid360Diagnosis.looksLikeEthernetAdapter] and for
     * the log. Joined rather than picked so a match on either the product
     * string or the manufacturer string counts.
     */
    val searchableName: String
        get() = listOfNotNull(productName, manufacturerName, deviceName).joinToString(" ")

    fun describe(): String = buildString {
        append(ids)
        append(" dev=").append(deviceName)
        append(" product=").append(quoted(productName))
        append(" mfr=").append(quoted(manufacturerName))
        append(" class=").append(hex2(deviceClass)).append('/')
            .append(hex2(deviceSubclass)).append('/').append(hex2(deviceProtocol))
        append(" ifaces=").append(interfaces.size)
        if (interfaces.isNotEmpty()) {
            append(" [").append(interfaces.joinToString("; ") { it.describe() }).append(']')
        }
    }
}

/**
 * One entry from `NetworkInterface.getNetworkInterfaces()`.
 *
 * Deliberately the raw kernel view and **not** ConnectivityManager's: an
 * interface can exist, be up and hold an address while `ConnectivityManager`
 * has no `Network` for it at all (no `EthernetNetworkFactory` on the build, or
 * the network scored below something else). Printing both views side by side
 * is what makes that disagreement visible, and the disagreement is itself a
 * diagnosis.
 *
 * @param addresses IPv4 addresses in `ip/prefix` form. IPv6 is dropped for the
 *   same reason [Mid360Settings] is dotted-quad throughout: nothing downstream
 *   can use it, and it would triple the length of every line.
 */
data class NetInterfaceRecord(
    val name: String,
    val up: Boolean,
    val loopback: Boolean,
    val mtu: Int,
    val addresses: List<String> = emptyList(),
    val virtual: Boolean = false,
) {
    /**
     * Does the NAME look like a wired interface?
     *
     * A hint, and used only as a hint. Linux/Android name USB Ethernet
     * functions `eth*`, `usb*`, `rndis*`, `ncm*` or the predictable `enx*`/
     * `enp*`; cellular is `rmnet*`/`ccmni*` and Wi-Fi is `wlan*`. Getting this
     * wrong in either direction costs a slightly less specific verdict line
     * and nothing else — the full interface list is printed underneath it
     * either way, which is the actual evidence.
     */
    val looksWired: Boolean
        get() = !loopback && WIRED_PREFIXES.any { name.startsWith(it) }

    fun describe(): String = buildString {
        append(name)
        append(" up=").append(up)
        append(" loop=").append(loopback)
        append(" mtu=").append(mtu)
        if (virtual) append(" virtual=true")
        append(" addr=").append(if (addresses.isEmpty()) "[]" else addresses.joinToString(",", "[", "]"))
        if (looksWired) append(" WIRED?")
    }

    private companion object {
        val WIRED_PREFIXES = listOf("eth", "usb", "rndis", "ncm", "enx", "enp", "usbnet")
    }
}

/**
 * `ConnectivityManager`'s view of an Ethernet-transport network, copied
 * straight out of `EthernetMonitor`'s [EthernetState]-shaped state.
 *
 * Reused rather than re-derived: `EthernetMonitor` already owns the
 * `requestNetwork`-then-fall-back-to-`registerNetworkCallback` dance and the
 * IPv4 filtering, and a second implementation of that in a diagnostic would be
 * a second thing to keep right.
 *
 * [usingRequest] is carried because it changes what "no Ethernet network"
 * means: with a plain callback registration the OS was never *asked* to bring
 * one up, and that is worth knowing before blaming the hub.
 */
data class ConnectivityEthernetRecord(
    val present: Boolean = false,
    val interfaceName: String? = null,
    val addresses: List<String> = emptyList(),
    val hasInternet: Boolean = false,
    val usingRequest: Boolean = false,
    val lastEvent: String = "",
)

/**
 * One datagram heard on the discovery port.
 *
 * [summary] is a **summary** and never the payload. A Mid-360 heartbeat is
 * ~430 bytes of mostly-binary device state; dumping it would bury the sweep,
 * would rotate the 512 KB `capture.log` out of existence inside a wizard
 * session, and would put a device serial number into a file in a form nobody
 * can read anyway. Parsed fields when the parse succeeded, a short printable
 * prefix when it did not — that is the whole contract, and [byteCount] carries
 * the size the bytes no longer do.
 */
data class DatagramRecord(
    val sourceIp: String,
    val sourcePort: Int,
    val byteCount: Int,
    val summary: String,
) {
    fun describe(): String = "$sourceIp:$sourcePort ${byteCount}B ${quoted(summary)}"

    companion object {
        /** How much of an unparsed payload may appear in the log. See [summarise]. */
        const val MAX_PREFIX_CHARS = 24

        /**
         * The ONE place a datagram payload is turned into log text, so the
         * never-dump rule has exactly one implementation to get right.
         *
         * A recognised heartbeat becomes its parsed fields — which is strictly
         * more useful than its bytes, since the whole reason to look at a
         * heartbeat is the pair of addresses inside it. Anything else becomes
         * a short printable prefix with non-printables replaced, because
         * "something else is broadcasting on this port" needs enough of the
         * payload to be recognisable (`<?xml`, `M-SEARCH`, `{"`) and not one
         * byte more.
         *
         * Bounded at [MAX_PREFIX_CHARS] regardless of what arrives: a hostile
         * or broken sender on the discovery port must not be able to fill the
         * capture log through this path.
         */
        fun summarise(payload: ByteArray): String {
            val heartbeat = Mid360HeartbeatParser.parse(payload)
            if (heartbeat != null) {
                return "mid360 sn=${heartbeat.serialNumber} type=${heartbeat.deviceType} " +
                    "fw=${heartbeat.firmwareVersion} lidar=${heartbeat.lidarIp} host=${heartbeat.persistedHostIp}"
            }
            val prefix = payload.take(MAX_PREFIX_CHARS).map { byte ->
                val c = byte.toInt().toChar()
                if (c.code in 0x20..0x7E) c else '.'
            }.joinToString("")
            return "not-a-heartbeat prefix=$prefix"
        }
    }
}

/**
 * One serial auto-detect probe attempt and what it concluded.
 *
 * Mirrors `SerialLidarAutoDetectors`' D6 → STL-27L ladder one rung per record,
 * in the order the rungs ran, because the *order* is half the diagnosis: an
 * `unusable` on rung 1 means rung 2 never ran at all, and a log that shows one
 * line for the D6 and nothing for the STL-27L would otherwise be read as "the
 * STL-27L declined".
 */
data class SerialProbeRecord(
    val sensor: String,
    val devicePath: String,
    val outcome: String,
    val detail: String = "",
) {
    fun describe(): String = buildString {
        append(sensor).append(' ').append(devicePath).append(" -> ").append(outcome)
        if (detail.isNotBlank()) append(" (").append(detail).append(')')
    }

    companion object {
        const val OUTCOME_IDENTIFIED = "identified"
        const val OUTCOME_DECLINED = "declined"
        const val OUTCOME_UNUSABLE = "unusable"
    }
}

/**
 * What the discovery side of the app has been doing.
 *
 * @param listening whether a UDP listener is bound to [port] right now. "No
 *   heartbeat heard" means two completely different things depending on this
 *   flag, and until this record existed the log could not tell them apart.
 * @param datagrams every datagram heard since the last sweep, oldest first —
 *   including ones that were NOT Mid-360 heartbeats, because "something is
 *   broadcasting on 56201 and it is not the lidar" is a real and otherwise
 *   invisible failure.
 * @param probes the serial ladder's attempts, in the order they ran.
 */
data class DiscoveryRecord(
    val listening: Boolean = false,
    val port: Int = Mid360HeartbeatParser.HEARTBEAT_PORT,
    val datagrams: List<DatagramRecord> = emptyList(),
    val probes: List<SerialProbeRecord> = emptyList(),
    /** Datagrams dropped from the bounded buffer since the last sweep, so the count stays honest. */
    val datagramsDropped: Int = 0,
)

/**
 * One full connection-detection sweep: the state of every link in the chain at
 * one instant, plus what triggered the look.
 *
 * @param trigger `wizard-poll`, `auto-detect`, `settings-row` — the reason
 *   this sweep exists. It is the field that makes a long log greppable: one
 *   `grep 'trigger=settings-row'` is the on-demand sweep the operator ran and
 *   nothing else.
 * @param expectedHostIp the address the lidar unicasts to, from a heartbeat if
 *   one was heard and from the configured field otherwise — the same rule
 *   [Mid360Diagnosis.classify] uses, for the same reason.
 */
data class ConnectionSweep(
    val trigger: String,
    val usb: List<UsbDeviceRecord> = emptyList(),
    val interfaces: List<NetInterfaceRecord> = emptyList(),
    val ethernet: ConnectivityEthernetRecord = ConnectivityEthernetRecord(),
    val discovery: DiscoveryRecord = DiscoveryRecord(),
    val expectedHostIp: String = Mid360Settings.DEFAULT_HOST_IP,
    val heartbeatAgeMillis: Long? = null,
)

// ── internal formatting helpers, shared by every describe() above ─────────

private fun hex2(value: Int): String = "0x%02x".format(value and 0xFF)
private fun hex4(value: Int): String = "%04x".format(value and 0xFFFF)
private fun quoted(value: String?): String = if (value == null) "-" else "\"" + value.replace('"', '\'') + "\""

/** USB base-class names, for the interface lines. Only the classes a phone actually meets. */
internal object UsbClasses {
    const val PER_INTERFACE = 0x00
    const val AUDIO = 0x01
    const val CDC_CONTROL = 0x02
    const val HID = 0x03
    const val PHYSICAL = 0x05
    const val IMAGE = 0x06
    const val PRINTER = 0x07
    const val MASS_STORAGE = 0x08
    const val HUB = 0x09
    const val CDC_DATA = 0x0A
    const val SMART_CARD = 0x0B
    const val VIDEO = 0x0E
    const val MISCELLANEOUS = 0xEF
    const val APPLICATION = 0xFE
    const val VENDOR_SPECIFIC = 0xFF

    fun name(value: Int): String = when (value and 0xFF) {
        PER_INTERFACE -> "per-interface"
        AUDIO -> "audio"
        CDC_CONTROL -> "cdc-control"
        HID -> "hid"
        PHYSICAL -> "physical"
        IMAGE -> "image"
        PRINTER -> "printer"
        MASS_STORAGE -> "mass-storage"
        HUB -> "hub"
        CDC_DATA -> "cdc-data"
        SMART_CARD -> "smart-card"
        VIDEO -> "video"
        MISCELLANEOUS -> "misc"
        APPLICATION -> "application"
        VENDOR_SPECIFIC -> "vendor"
        else -> hex2(value)
    }
}
