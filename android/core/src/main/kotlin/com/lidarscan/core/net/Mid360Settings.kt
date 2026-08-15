package com.lidarscan.core.net

import kotlinx.serialization.Serializable

/**
 * The Mid-360's network configuration, as the connect wizard collects it and
 * as it is persisted per project in `manifest.json`.
 *
 * Both IPs are mandatory and neither has a usable default beyond the one this
 * class ships, because **the Mid-360 does not discover anything and is not
 * discovered** (A3 §3, "Explicit IP is mandatory, everywhere"):
 *
 *  * `lidarIp` — there is no broadcast discovery on the engine's supported
 *    path. On macOS it is structurally impossible (patch 0003 removes the
 *    broadcast bind); everywhere else it is the deliberate default, because
 *    "which of the two units on this switch did I just connect to" is not a
 *    question we want the network answering.
 *  * `hostIp` — the device is *told* where to stream, through the
 *    `kKeyLidarPointDataHostIpCfg` / `kKeyLidarImuHostIpCfg` keys in SDK2's
 *    `0x0100` configuration push. Get it wrong and the device streams into
 *    the void, **silently**: no error, no packets, nothing to see. That
 *    silent-failure mode is why [validate] treats a host IP that is not one
 *    of this device's own addresses as an error and not a warning.
 *
 * The port defaults are `UdpConfig`'s (`engine/include/scanengine/transport/
 * udp_source.h`): device 56100/56200/56300/56400/56500, host the same +1.
 * Only the three that a user has any reason to change are exposed by the
 * wizard, matching desktop C2's Mid-360 tab.
 */
@Serializable
data class Mid360Settings(
    val lidarIp: String = DEFAULT_LIDAR_IP,
    val hostIp: String = DEFAULT_HOST_IP,
    val devicePointPort: Int = 56300,
    val deviceImuPort: Int = 56400,
    val deviceCmdPort: Int = 56100,
    val hostPointPort: Int = 56301,
    val hostImuPort: Int = 56401,
    val hostCmdPort: Int = 56101,
    /** `scanengine::Mid360Backend`: 0 = SDK2 (default), 1 = raw UDP over a pre-bound socket. */
    val backend: Int = BACKEND_SDK2,
) {
    companion object {
        /**
         * Tech Spec §3.1 / `udp_source.h`: "lidar on 192.168.1.1xx, host
         * static on 192.168.1.x". `.100` is the address a factory-default
         * Mid-360 ships with.
         */
        const val DEFAULT_LIDAR_IP = "192.168.1.100"
        const val DEFAULT_HOST_IP = "192.168.1.5"

        const val BACKEND_SDK2 = 0
        const val BACKEND_RAW_UDP = 1
    }
}

/** One validation finding. [fatal] findings block the self-test; the rest are shown but do not. */
data class Mid360Issue(val field: Mid360Field, val message: String, val fatal: Boolean)

enum class Mid360Field { LIDAR_IP, HOST_IP, PORTS, BACKEND }

data class Mid360Validation(val issues: List<Mid360Issue>) {
    val fatalIssues: List<Mid360Issue> get() = issues.filter { it.fatal }
    val warnings: List<Mid360Issue> get() = issues.filterNot { it.fatal }
    val isUsable: Boolean get() = fatalIssues.isEmpty()

    fun firstFor(field: Mid360Field): Mid360Issue? = issues.firstOrNull { it.field == field }
}

/**
 * Pure IPv4 helpers. Deliberately hand-rolled rather than `InetAddress.getByName`:
 * that one performs a **DNS lookup** for anything that is not a literal, which on
 * Android is a network call on whatever thread you happened to be on. The wizard
 * validates on every keystroke.
 */
object Ipv4 {
    /** Returns the address as an unsigned 32-bit value in a Long, or null if it is not a dotted quad. */
    fun parse(text: String): Long? {
        val parts = text.trim().split('.')
        if (parts.size != 4) return null
        var value = 0L
        for (part in parts) {
            // Reject "01", "+1", " 1" and the empty string: a Mid-360 IP is
            // typed by a human under time pressure and a silently-reinterpreted
            // octet is worse than a red field.
            if (part.isEmpty() || part.length > 3) return null
            if (!part.all { it in '0'..'9' }) return null
            if (part.length > 1 && part[0] == '0') return null
            val octet = part.toInt()
            if (octet > 255) return null
            value = (value shl 8) or octet.toLong()
        }
        return value
    }

    fun isValid(text: String): Boolean = parse(text) != null

    /** True when both addresses share the given prefix length (e.g. 24 for a /24). */
    fun sameSubnet(a: String, b: String, prefixLength: Int): Boolean {
        val av = parse(a) ?: return false
        val bv = parse(b) ?: return false
        if (prefixLength <= 0) return true
        if (prefixLength >= 32) return av == bv
        val mask = (0xFFFFFFFFL shl (32 - prefixLength)) and 0xFFFFFFFFL
        return (av and mask) == (bv and mask)
    }

    fun isLoopback(text: String): Boolean = (parse(text) ?: return false) ushr 24 == 127L

    fun isMulticastOrBroadcast(text: String): Boolean {
        val v = parse(text) ?: return false
        val firstOctet = v ushr 24
        return firstOctet >= 224L || v == 0xFFFFFFFFL
    }
}

/**
 * One address the *device* currently holds, as read from the Ethernet
 * `Network`'s `LinkProperties`. Kept in `:core` as a plain data class (not
 * `android.net.LinkAddress`) so [validateMid360Settings] stays JVM-testable.
 */
data class LocalAddress(val ip: String, val prefixLength: Int)

/**
 * Validates a [Mid360Settings] against what the phone's Ethernet interface
 * actually has.
 *
 * @param localAddresses the IPv4 addresses currently on the Ethernet
 *   interface. **Empty means "not known yet"** (no Ethernet network, or
 *   `LinkProperties` not delivered), and in that case the host-IP locality
 *   check is reported as a warning rather than an error — refusing to let a
 *   user configure the wizard before the cable is in would be the wrong
 *   trade.
 *
 * This is deliberately stricter than desktop C2, which validates only that
 * the lidar IP is non-empty and lets `add_device` be the validator. That is a
 * defensible desktop trade (fail late, with a good message) but a poor one
 * here: the Mid-360's characteristic failure is *silence*, and an 8-second
 * self-test that fails with "no packet" is a much worse diagnosis than a
 * field that says the host IP is not an address this phone holds.
 */
fun validateMid360Settings(
    settings: Mid360Settings,
    localAddresses: List<LocalAddress> = emptyList(),
): Mid360Validation {
    val issues = mutableListOf<Mid360Issue>()

    val lidar = settings.lidarIp.trim()
    val host = settings.hostIp.trim()

    if (lidar.isEmpty()) {
        issues += Mid360Issue(
            Mid360Field.LIDAR_IP,
            "Required. The Mid-360 is never found by broadcast on this path — type the address it is configured with (factory default 192.168.1.100).",
            fatal = true,
        )
    } else if (!Ipv4.isValid(lidar)) {
        issues += Mid360Issue(Mid360Field.LIDAR_IP, "Not an IPv4 address (expected four dotted octets, e.g. 192.168.1.100).", fatal = true)
    } else if (Ipv4.isMulticastOrBroadcast(lidar)) {
        issues += Mid360Issue(Mid360Field.LIDAR_IP, "A multicast or broadcast address cannot be a lidar address.", fatal = true)
    }

    if (host.isEmpty()) {
        issues += Mid360Issue(
            Mid360Field.HOST_IP,
            "Required. The device is told where to stream and never discovers this phone — a wrong host IP means it streams into the void with no error at all.",
            fatal = true,
        )
    } else if (!Ipv4.isValid(host)) {
        issues += Mid360Issue(Mid360Field.HOST_IP, "Not an IPv4 address (expected four dotted octets, e.g. 192.168.1.5).", fatal = true)
    } else if (Ipv4.isMulticastOrBroadcast(host)) {
        issues += Mid360Issue(Mid360Field.HOST_IP, "A multicast or broadcast address cannot be this phone's address.", fatal = true)
    } else if (Ipv4.isLoopback(host)) {
        // The engine's own sim config uses 127.000.000.001 to slip past
        // SDK2's self-IP filter; A3 §7 says in as many words not to copy that
        // into production, so the wizard refuses it outright.
        issues += Mid360Issue(
            Mid360Field.HOST_IP,
            "Loopback. A Mid-360 on the wire cannot reach 127.x — that address is only used by the engine's own loopback simulator.",
            fatal = true,
        )
    }

    if (lidar.isNotEmpty() && host.isNotEmpty() && lidar == host) {
        issues += Mid360Issue(Mid360Field.HOST_IP, "The host and lidar cannot share one address.", fatal = true)
    }

    if (Ipv4.isValid(lidar) && Ipv4.isValid(host) && lidar != host && !Ipv4.sameSubnet(lidar, host, 24)) {
        issues += Mid360Issue(
            Mid360Field.HOST_IP,
            "Host and lidar are on different /24 subnets. With no router on a direct USB-Ethernet link, nothing will route between them.",
            fatal = false,
        )
    }

    if (Ipv4.isValid(host)) {
        if (localAddresses.isEmpty()) {
            issues += Mid360Issue(
                Mid360Field.HOST_IP,
                "Not checked against the Ethernet interface yet — no Ethernet network is available to read addresses from.",
                fatal = false,
            )
        } else if (localAddresses.none { it.ip == host }) {
            issues += Mid360Issue(
                Mid360Field.HOST_IP,
                "This phone's Ethernet interface holds " +
                    localAddresses.joinToString(", ") { "${it.ip}/${it.prefixLength}" } +
                    ". The host IP must be one of them, or the device will stream to an address nothing is listening on.",
                fatal = true,
            )
        }
    }

    val ports = listOf(
        "device point" to settings.devicePointPort,
        "device IMU" to settings.deviceImuPort,
        "device command" to settings.deviceCmdPort,
        "host point" to settings.hostPointPort,
        "host IMU" to settings.hostImuPort,
        "host command" to settings.hostCmdPort,
    )
    ports.firstOrNull { it.second !in 1..65535 }?.let {
        issues += Mid360Issue(Mid360Field.PORTS, "${it.first} port ${it.second} is outside 1–65535.", fatal = true)
    }
    val hostPorts = listOf(settings.hostPointPort, settings.hostImuPort, settings.hostCmdPort)
    if (hostPorts.toSet().size != hostPorts.size) {
        issues += Mid360Issue(
            Mid360Field.PORTS,
            "The three host ports must differ — each one is a separate bound socket.",
            fatal = true,
        )
    }

    if (settings.backend == Mid360Settings.BACKEND_RAW_UDP) {
        issues += Mid360Issue(
            Mid360Field.BACKEND,
            "Raw UDP is listen-only: it cannot run discovery, the handshake or the host-IP configuration push, so it works only against a device already configured to stream to this phone. IMU is off on this path (see NOTES.md).",
            fatal = false,
        )
    }

    return Mid360Validation(issues)
}
