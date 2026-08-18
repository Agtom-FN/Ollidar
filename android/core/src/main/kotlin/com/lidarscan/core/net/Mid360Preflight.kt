package com.lidarscan.core.net

/**
 * ROUND 14 (owner item 53) — **refuse a Mid-360 capture that cannot possibly
 * receive data, and say why in one sentence.**
 *
 * ## What happened
 *
 * The owner plugged the Mid-360 into the Pixel over a USB-C Ethernet adapter
 * and pressed Start twice. From his log:
 *
 * ```
 * 17:09:33 [session] start: … sensor=MID360 …
 * 17:09:35 [session] NO DATA after 2128ms: bytesIn=0 packetsOk=0 packetsBad=0 pointsOut=0
 * 17:10:03 [session] start: … sensor=MID360 …
 * 17:10:05 [session] NO DATA after 2002ms: bytesIn=0 packetsOk=0 packetsBad=0 pointsOut=0
 * 17:10:18 [seal] … NO-DATA=true reason="… re-seat the USB-C cable, and start again."
 * ```
 *
 * Two sealed scans of nothing, and advice ("re-seat the cable") that was
 * almost certainly wrong. `connect()` had returned success — but a Mid-360
 * connect is `Engine::add_device`, which *constructs a driver* and performs no
 * I/O whatsoever, and `Engine::start_session` logs a driver that fails to start
 * and carries on. So CONNECTED was never evidence of a link, and the first
 * moment the app could tell the truth was two seconds after it had already
 * begun recording.
 *
 * ## Why the addressing is the thing to check
 *
 * A Mid-360 does not broadcast its point stream. It **unicasts to a host IP it
 * has been configured with and persists** — on the owner's unit,
 * `192.168.1.5`, which it also advertises in the heartbeat it broadcasts on UDP
 * 56201. If the phone's Ethernet interface does not OWN that address, the
 * packets are addressed to a machine that is not on the cable and nothing
 * arrives, forever, with the link light on and the cable perfectly seated.
 * Android USB-Ethernet DHCPs by default, so out of the box the interface holds
 * something like `192.168.1.100` and the answer is 0 bytes.
 *
 * There is no public API to set it: `EthernetManager` and
 * `StaticIpConfiguration` are `@SystemApi` behind signature-level
 * `MANAGE_ETHERNET_NETWORKS`. So this cannot be FIXED in software — it can only
 * be diagnosed precisely and handed to the operator with the exact value to
 * type. That is what this class is for, and why its output is a sentence rather
 * than a boolean.
 *
 * ## What it does not claim
 *
 * Passing every check here does NOT guarantee data. The remaining known gap is
 * routing rather than addressing: the SDK2 backend creates its own sockets
 * inside the vendored Livox SDK, and nothing binds them to the Ethernet
 * `Network`, so with Wi-Fi up the kernel may still choose the wrong interface
 * (`android/NOTES.md` §8 finding 3). That is a separate fix and is named on the
 * backlog. This gate removes the failure that CAN be checked before a byte is
 * recorded, and the NO-DATA watchdog stays as the backstop for the one that
 * cannot.
 *
 * Pure `:core`: no Android types, so every verdict is asserted in a unit test
 * against the owner's real addresses.
 */
object Mid360Preflight {

    /**
     * `blocking = true` means "do not start a session" — the scan would record
     * zero bytes and the operator would learn that two seconds later.
     */
    data class Verdict(
        val ok: Boolean,
        val blocking: Boolean,
        val summary: String,
        /** The concrete thing to type, when there is one. Null when there is not. */
        val fix: String? = null,
        /** The stable token for the capture log, so a field report carries the diagnosis. */
        val logToken: String,
    )

    /** How stale a heartbeat may be and still count as "the lidar is talking". */
    const val HEARTBEAT_FRESH_MILLIS: Long = 15_000L

    /**
     * @param adapterPresent an Ethernet transport exists at all
     * @param interfaceAddresses the IPv4 addresses the phone's Ethernet interface holds
     * @param expectedHostIp the host IP the lidar unicasts to — from its own heartbeat when
     *   one was heard, otherwise whatever the operator configured
     * @param heartbeatAgeMillis how long ago a heartbeat was parsed, or null for never
     */
    fun evaluate(
        adapterPresent: Boolean,
        interfaceAddresses: List<String>,
        expectedHostIp: String?,
        heartbeatAgeMillis: Long?,
    ): Verdict {
        if (!adapterPresent) {
            return Verdict(
                ok = false,
                blocking = true,
                summary = "No Ethernet adapter. The Mid-360 is a network sensor — the phone " +
                    "cannot see it over USB alone.",
                fix = "Plug the USB-C Ethernet adapter in, then the lidar's cable into it.",
                logToken = "no-ethernet",
            )
        }
        if (interfaceAddresses.isEmpty()) {
            return Verdict(
                ok = false,
                blocking = true,
                summary = "The Ethernet adapter is connected but has no IP address yet.",
                fix = "Wait a few seconds, or set it manually in Settings > Network & internet > " +
                    "Ethernet.",
                logToken = "no-address",
            )
        }
        val heartbeatSeen = heartbeatAgeMillis != null && heartbeatAgeMillis <= HEARTBEAT_FRESH_MILLIS
        val host = expectedHostIp?.trim().orEmpty()

        // The decisive check, and the one that explains the owner's zero bytes.
        if (host.isNotEmpty() && !interfaceAddresses.contains(host)) {
            return Verdict(
                ok = false,
                blocking = true,
                summary = "The Mid-360 sends its data to $host, and this phone's Ethernet is " +
                    interfaceAddresses.joinToString(", ") + ". Nothing would be recorded.",
                fix = "Settings > Network & internet > Ethernet > IP settings > Static. " +
                    "IP address $host, netmask 255.255.255.0. Then come back and press Start.",
                logToken = "host-ip-mismatch",
            )
        }
        if (!heartbeatSeen) {
            return Verdict(
                ok = false,
                blocking = true,
                summary = "Mid-360 heartbeat not seen on the cable. The addressing looks right, " +
                    "so either the lidar is not powered or the cable is not through to it.",
                fix = "Check the lidar's power, then re-seat the Ethernet cable at both ends.",
                logToken = "no-heartbeat",
            )
        }
        return Verdict(
            ok = true,
            blocking = false,
            summary = "Mid-360 heartbeat seen; this phone holds $host.",
            logToken = "ok",
        )
    }
}
