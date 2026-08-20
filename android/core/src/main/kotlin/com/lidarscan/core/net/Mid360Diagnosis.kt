package com.lidarscan.core.net

/**
 * ROUND 25 (owner item 118) — **"no Ethernet adapter" is three different
 * failures, and the app has been printing one string for all of them.**
 *
 * ## What happened
 *
 * The Mid-360 never came up in the field. The whole diagnosis the app
 * produced, twice, two minutes apart, was:
 *
 * ```
 * 18:53 [net] mid360 preflight: no-ethernet — No Ethernet adapter
 * 18:55 [net] mid360 preflight: no-ethernet — No Ethernet adapter
 * ```
 *
 * The OS never enumerated an Ethernet interface at all. That single line is
 * true, and it is useless, because at least three unrelated things produce
 * it and each has a different fix:
 *
 *  1. **nothing is plugged in** — the operator's fix is a cable;
 *  2. **something is plugged in but the kernel has no driver for it** — the
 *     operator's fix is a different adapter, and no amount of re-seating
 *     will help;
 *  3. **the adapter is enumerated but there is not enough bus power** to run
 *     it — the operator's fix is a *powered* USB-C hub, which is the case
 *     that costs a whole site visit when it is guessed wrong.
 *
 * The distinguishing evidence is free and was already sitting there: **what
 * the OS does enumerate on USB**. Zero USB devices and zero Ethernet
 * interfaces is case 1. Several USB devices, none of them an Ethernet
 * transport, is case 2 or 3 — and naming the devices that ARE seen is what
 * lets the operator (or a field report a week later) tell which.
 *
 * ## Why this is a second class and not more branches in [Mid360Preflight]
 *
 * [Mid360Preflight] answers one question — *may this capture start?* — and
 * its output is a refusal with a sentence. This answers a different one:
 * *where along the chain did it break, and what is the next thing to try?*
 * It is a **ladder**, evaluated in physical order (adapter, link, address,
 * subnet, host, lidar), because each rung is meaningless until the one below
 * it holds, and because the rung you are stuck on IS the instruction. The
 * preflight gate stays exactly as it is; this drives the wizard.
 *
 * ## The wording
 *
 * Every operator string lives in this file and is listed in [ALL], so the
 * round-22 wording guard can see all of them: an instruction is at most six
 * words, its one detail line at most twelve. The instruction says what is
 * wrong; the detail says what to do about it. Nothing here explains the
 * mechanism — the mechanism is in this comment, where the operator standing
 * in a stairwell does not have to read it.
 *
 * Pure `:core`: no Android types anywhere, so every rung of the ladder is
 * asserted in a unit test against a synthetic interface list.
 */
object Mid360Diagnosis {

    /**
     * The rungs, in the order the physical chain fails.
     *
     * Ordered deliberately: `ordinal` is "how far along the chain we got",
     * which is what makes a progress readout in the wizard possible without
     * a second table.
     */
    enum class State {
        /** No Ethernet transport at all. The owner's field failure. */
        NO_ADAPTER,

        /** An adapter is enumerated on USB, but the OS has no Ethernet network for it. */
        ADAPTER_NO_LINK,

        /** The interface is up and holds no IPv4 address — there is no DHCP server on a lidar cable. */
        LINK_NO_IP,

        /** The interface holds an address, but on a different /24 than the lidar's. */
        WRONG_SUBNET,

        /** Right /24, wrong address: the lidar unicasts to one host and this phone is not it. */
        WRONG_HOST_IP,

        /** Addressing is correct and no heartbeat has been heard. Run discovery. */
        IP_OK_NO_LIDAR,

        /** Heartbeat heard, on the right address. */
        OK,
    }

    /**
     * One rung, fully rendered.
     *
     * The three `shows*` flags are here rather than in the composable because
     * "which evidence does this state need on screen" is a property of the
     * state, and a `when` at the draw site is how the four states quietly
     * become five without the fifth getting its evidence.
     *
     * @param instruction what is wrong. Six words at most.
     * @param detail what to do about it. Twelve words at most.
     * @param logToken the stable token for the capture log, so a field report
     *   a month later carries the diagnosis and not just the symptom.
     */
    data class Step(
        val state: State,
        val instruction: String,
        val detail: String,
        val logToken: String,
        /** Name the USB devices the OS DOES see — the evidence that splits "nothing plugged" from "unsupported". */
        val showsUsbDevices: Boolean = false,
        /** Show the addresses the interface currently holds. */
        val showsAddresses: Boolean = false,
        /** Offer the deep link to Android's Ethernet settings (the caller still checks the intent resolves). */
        val showsEthernetSettings: Boolean = false,
        /** Run the existing UDP 56201 heartbeat discovery from this state. */
        val runsDiscovery: Boolean = false,
    ) {
        val isOk: Boolean get() = state == State.OK
    }

    /** How stale a heartbeat may be and still count as "the lidar is talking". Shared with the preflight gate. */
    const val HEARTBEAT_FRESH_MILLIS: Long = Mid360Preflight.HEARTBEAT_FRESH_MILLIS

    /** The lidar's default /24. Not a second copy of the address — [Mid360Settings.DEFAULT_HOST_IP] is the one. */
    const val HOST_PREFIX_LENGTH = 24

    // ── the words ──────────────────────────────────────────────────────────

    const val TITLE = "Ethernet check"
    const val RETRY = "Retry"
    const val OPEN_ETHERNET_SETTINGS = "Open Ethernet settings"
    const val USB_DEVICES_LABEL = "USB devices seen"
    const val NO_USB_DEVICES = "None. Nothing on USB."
    const val ADDRESSES_LABEL = "This phone holds"
    const val LISTENING = "Listening for the lidar."
    const val HEARD_NOTHING = "Heard nothing on the cable."

    const val NO_ADAPTER = "No Ethernet adapter found."

    /** Nothing at all on USB: the cable, not the adapter, is the first suspect. */
    const val NO_ADAPTER_NOTHING_PLUGGED = "Nothing is plugged in. Try a powered USB-C hub."

    /**
     * USB works, this adapter does not. Either the kernel has no driver for
     * it or it is browning out on bus power — both end at the same next step,
     * and the device list above the line says which one to believe.
     */
    const val NO_ADAPTER_UNSUPPORTED = "USB works, this adapter does not. Try a powered hub."

    const val ADAPTER_NO_LINK = "Adapter found, no link."
    const val ADAPTER_NO_LINK_DETAIL = "Check the cable, then the lidar's own power supply."

    const val LINK_NO_IP = "Link up, no address."

    const val WRONG_SUBNET = "Wrong network for the lidar."

    const val WRONG_HOST_IP = "Right network, wrong address."

    const val IP_OK_NO_LIDAR = "Address set, lidar silent."
    const val IP_OK_NO_LIDAR_DETAIL = "Check the lidar's power, then retry."

    const val OK = "Mid-360 heard. Ready."

    /** "Set a static IP: 192.168.1.5, mask 255.255.255.0." */
    fun staticIpDetail(hostIp: String): String = "Set a static IP: $hostIp, mask 255.255.255.0."

    /** "The lidar streams to 192.168.1.5. Set that." */
    fun wrongAddressDetail(hostIp: String): String = "The lidar streams to $hostIp. Set that."

    /** "This phone holds 192.168.1.5. Start the scan." */
    fun okDetail(hostIp: String): String = "This phone holds $hostIp. Start the scan."

    /**
     * Every operator-facing string this item adds, at its default address, for
     * the wording guard. The three interpolated ones are rendered with
     * [Mid360Settings.DEFAULT_HOST_IP]: a longer address cannot make any of
     * them longer, because an IPv4 literal is one word however many digits it
     * has.
     */
    val ALL: List<String> = listOf(
        TITLE,
        RETRY,
        OPEN_ETHERNET_SETTINGS,
        USB_DEVICES_LABEL,
        NO_USB_DEVICES,
        ADDRESSES_LABEL,
        LISTENING,
        HEARD_NOTHING,
        NO_ADAPTER,
        NO_ADAPTER_NOTHING_PLUGGED,
        NO_ADAPTER_UNSUPPORTED,
        ADAPTER_NO_LINK,
        ADAPTER_NO_LINK_DETAIL,
        LINK_NO_IP,
        WRONG_SUBNET,
        WRONG_HOST_IP,
        IP_OK_NO_LIDAR,
        IP_OK_NO_LIDAR_DETAIL,
        OK,
        staticIpDetail(Mid360Settings.DEFAULT_HOST_IP),
        wrongAddressDetail(Mid360Settings.DEFAULT_HOST_IP),
        okDetail(Mid360Settings.DEFAULT_HOST_IP),
    )

    // ── the ladder ─────────────────────────────────────────────────────────

    /**
     * Which rung the chain is stuck on.
     *
     * @param adapterPresent an Ethernet-capable adapter exists — either the OS
     *   has an Ethernet transport, or one of [usbDeviceNames] is recognisably
     *   an Ethernet adapter (see [looksLikeEthernetAdapter]). Splitting this
     *   from [linkUp] is the whole of the NO_ADAPTER/ADAPTER_NO_LINK
     *   distinction.
     * @param linkUp the OS has actually brought an Ethernet network up.
     * @param interfaceAddresses the IPv4 addresses the Ethernet interface
     *   holds. `"192.168.1.5"` and `"192.168.1.5/24"` are both accepted — the
     *   Android side formats them one way and a log the other.
     * @param usbDeviceNames every USB device the OS enumerates, however it
     *   names them. Evidence, never a decision on its own — except for the
     *   one decision it is uniquely qualified to make, which is whether
     *   NO_ADAPTER means "nothing plugged" or "this adapter did not come up".
     * @param expectedHostIp the address the lidar unicasts to — from its own
     *   heartbeat when one was heard, otherwise what the operator configured.
     *   Blank or unparseable falls back to [Mid360Settings.DEFAULT_HOST_IP],
     *   because a wizard that stops working when a field is half-typed is
     *   worse than one that assumes the factory default.
     * @param heartbeatAgeMillis how long ago a heartbeat was parsed, or null
     *   for never.
     */
    fun classify(
        adapterPresent: Boolean,
        linkUp: Boolean,
        interfaceAddresses: List<String>,
        usbDeviceNames: List<String>,
        expectedHostIp: String? = Mid360Settings.DEFAULT_HOST_IP,
        heartbeatAgeMillis: Long? = null,
    ): Step {
        val host = expectedHostIp?.trim()
            ?.takeIf { Ipv4.isValid(it) }
            ?: Mid360Settings.DEFAULT_HOST_IP

        if (!adapterPresent) {
            return Step(
                state = State.NO_ADAPTER,
                instruction = NO_ADAPTER,
                // The one branch the owner's log could not make. Note it keys
                // off "any USB device at all", not off "any Ethernet-looking
                // one": an adapter the kernel cannot drive still enumerates,
                // and an adapter browning out on bus power may enumerate and
                // then vanish — in both of those the list is non-empty and
                // the answer is a powered hub, not a different cable.
                detail = if (usbDeviceNames.isEmpty()) NO_ADAPTER_NOTHING_PLUGGED else NO_ADAPTER_UNSUPPORTED,
                logToken = "no-adapter",
                showsUsbDevices = true,
            )
        }

        if (!linkUp) {
            return Step(
                state = State.ADAPTER_NO_LINK,
                instruction = ADAPTER_NO_LINK,
                detail = ADAPTER_NO_LINK_DETAIL,
                logToken = "adapter-no-link",
                showsUsbDevices = true,
            )
        }

        val addresses = interfaceAddresses
            .map { it.trim().substringBefore('/') }
            .filter { Ipv4.isValid(it) }

        if (addresses.isEmpty()) {
            return Step(
                state = State.LINK_NO_IP,
                instruction = LINK_NO_IP,
                detail = staticIpDetail(host),
                logToken = "link-no-ip",
                showsEthernetSettings = true,
            )
        }

        // Real /24 arithmetic, not a string prefix: 192.168.1.5 and
        // 192.168.10.5 share three leading characters and nothing else.
        if (addresses.none { Ipv4.sameSubnet(it, host, HOST_PREFIX_LENGTH) }) {
            return Step(
                state = State.WRONG_SUBNET,
                instruction = WRONG_SUBNET,
                detail = staticIpDetail(host),
                logToken = "wrong-subnet",
                showsAddresses = true,
                showsEthernetSettings = true,
            )
        }

        // On the right cable, on the right /24, and still silent — because a
        // Mid-360 unicasts to ONE address and does not care that a neighbour
        // of it is listening. This is the owner's round-14 zero-byte capture.
        if (addresses.none { it == host }) {
            return Step(
                state = State.WRONG_HOST_IP,
                instruction = WRONG_HOST_IP,
                detail = wrongAddressDetail(host),
                logToken = "wrong-host-ip",
                showsAddresses = true,
                showsEthernetSettings = true,
            )
        }

        val heartbeatFresh = heartbeatAgeMillis != null && heartbeatAgeMillis <= HEARTBEAT_FRESH_MILLIS
        if (!heartbeatFresh) {
            return Step(
                state = State.IP_OK_NO_LIDAR,
                instruction = IP_OK_NO_LIDAR,
                detail = IP_OK_NO_LIDAR_DETAIL,
                logToken = "ip-ok-no-lidar",
                showsAddresses = true,
                runsDiscovery = true,
            )
        }

        return Step(
            state = State.OK,
            instruction = OK,
            detail = okDetail(host),
            logToken = "ok",
            showsAddresses = true,
        )
    }

    /**
     * Does this USB device name look like an Ethernet adapter?
     *
     * Used by the Android side to answer [classify]'s `adapterPresent` when
     * the OS has produced no Ethernet network — i.e. exactly the owner's
     * failure, where the interesting question is whether the hardware is
     * there at all.
     *
     * The chipset list is the four families that make up essentially every
     * USB-C Ethernet dongle sold: ASIX AX88772/AX88179, Realtek
     * RTL8152/RTL8153, and the generic strings vendors put in the product
     * descriptor. It is a **hint** and it is only ever used to make the
     * message more specific — a false negative shows the generic
     * "nothing plugged" detail plus the device list, which is still strictly
     * better than what the owner got.
     */
    fun looksLikeEthernetAdapter(name: String): Boolean {
        val lower = name.lowercase()
        return ETHERNET_HINTS.any { lower.contains(it) }
    }

    private val ETHERNET_HINTS: List<String> = listOf(
        "ethernet", "gigabit", "lan", "rtl81", "r8152", "r8153", "ax887", "asix", "network adapter",
    )
}
