package com.lidarscan.core.net

/**
 * ROUND 25 item 118, **owner amendment** — renders a [ConnectionSweep] into
 * the `[net-debug]` block that goes into the capture log and onto the
 * developer-mode "Connection debug" screen.
 *
 * ## The design constraint, in the owner's words
 *
 * > "The app said only 'No Ethernet adapter found.' Nobody can tell from that
 * > whether the hub was never enumerated on USB at all, or enumerated but no
 * > network interface appeared, or an interface came up on the wrong subnet."
 *
 * A wall of undifferentiated dump does not fix that — it is the same problem
 * with more scrolling. So **every sweep leads with a one-line verdict naming
 * which of the three cases it is**, and the detail follows underneath. The
 * verdict line is the thing a person reading a field log a month later greps
 * for and reads; the detail is what they read once the verdict has told them
 * where to look.
 *
 * ## The verdict is the item-118 ladder, not a second opinion
 *
 * [verdictFor] does not re-derive anything. It feeds the sweep's own evidence
 * into [Mid360Diagnosis.classify] — the same ladder the wizard shows the
 * operator — and then names the rung it landed on in developer language. Two
 * tables that can disagree about the same cable is exactly the failure mode
 * this amendment exists to remove, so there is one table.
 *
 * The one split the ladder makes internally and does not expose in its
 * [Mid360Diagnosis.State] is `NO_ADAPTER` with an empty USB list versus a
 * non-empty one — which is the owner's case 1 versus case 2, i.e. the single
 * most important distinction in this file. It is made here, from the same
 * `usbDeviceNames.isEmpty()` test [Mid360Diagnosis.classify] uses to choose
 * between its two detail strings, so the two can never drift.
 *
 * ## Wording law
 *
 * [com.lidarscan.core.WordingLaw] does not apply to any string in this file.
 * Everything here is developer-mode diagnostic output — it is never shown to
 * an operator, it is read by whoever is debugging a connection, and it is
 * supposed to be dense with numbers. This is the same exemption
 * `StartHoldTrimGate.refusalLogLine` has and for the same reason. Do not
 * shorten these to six words.
 */
object ConnectionSweepFormat {

    /** The log tag every periodic and on-demand sweep is written under. */
    const val TAG = "net-debug"

    /**
     * What the sweep concluded, in developer language.
     *
     * The first three are the owner's three cases, in the order he named them.
     * [headline] is deliberately a whole clause rather than a label: the point
     * of the verdict line is that reading it alone is enough.
     */
    enum class Verdict(val token: String, val headline: String) {
        /**
         * **Owner case 1.** Nothing at all in `getDeviceList()` and no Ethernet
         * interface. Nothing downstream of the connector ever ran — suspect the
         * cable, the port, or a hub that needs its own power before it will
         * even enumerate.
         */
        NOTHING_ON_USB(
            "nothing-on-usb",
            "nothing is enumerated on USB and there is no Ethernet interface " +
                "(case 1 of 3: dead cable, dead port, or a hub that never powered up)",
        ),

        /**
         * **Owner case 2.** USB devices ARE enumerated and the OS still produced
         * no Ethernet interface. The hardware is present and not working: either
         * the kernel has no driver for its Ethernet function, or the function is
         * browning out on bus power. The per-interface classes printed below say
         * which is more likely — an interface announcing a network function that
         * never produced an `eth*` is a driver problem, and no network function
         * at all means the device is not an Ethernet adapter in the first place.
         */
        USB_PRESENT_NO_ETHERNET(
            "usb-present-no-ethernet",
            "USB devices are enumerated but no Ethernet interface appeared " +
                "(case 2 of 3: unsupported chipset, or not enough bus power — try a POWERED hub)",
        ),

        /**
         * **Owner case 2, the sharper half.** An Ethernet FUNCTION is
         * enumerated on USB — either its interface descriptors announce one or
         * its name is a known chipset — and the kernel produced no wired
         * interface at all. The hardware is there and the OS will not drive
         * it: no driver, or it is browning out on bus power.
         *
         * This and [USB_PRESENT_NO_ETHERNET] are one rung of
         * [Mid360Diagnosis]'s ladder between them; the split is made here
         * rather than there because it is only answerable with the descriptor
         * detail this sweep collects and the wizard does not. It is a
         * refinement of one rung, not a second ladder.
         */
        ETHERNET_FUNCTION_NO_INTERFACE(
            "ethernet-function-no-interface",
            "a USB Ethernet function is enumerated and NO network interface appeared " +
                "(case 2 of 3: no kernel driver, or not enough bus power — try a POWERED hub)",
        ),

        /**
         * A wired interface exists in the kernel view and is DOWN. Different
         * from [ETHERNET_FUNCTION_NO_INTERFACE] in the way that matters: the
         * driver bound, the interface was created, and the link did not come
         * up — so the next suspect is the cable and the lidar's own power
         * supply, not the hub.
         */
        INTERFACE_DOWN(
            "interface-down",
            "a wired interface exists and is DOWN (the driver bound — check the cable, then the lidar's power supply)",
        ),

        /** Interface up, no IPv4 address. There is no DHCP server on a lidar cable; a static IP is required. */
        LINK_NO_IP(
            "link-no-ip",
            "the Ethernet interface is up and holds no IPv4 address (no DHCP on a direct lidar cable — set a static IP)",
        ),

        /**
         * **Owner case 3.** An interface is up with an address, on a different
         * /24 from the lidar's. Everything physical is healthy and the
         * addressing is wrong, which from the outside looks exactly like
         * silence.
         */
        INTERFACE_WRONG_SUBNET(
            "wrong-subnet",
            "an Ethernet interface is up on the WRONG subnet (case 3 of 3: the chain is healthy, the addressing is not)",
        ),

        /** Right /24, wrong host address — a Mid-360 unicasts to one address and does not care who else is listening. */
        WRONG_HOST_IP(
            "wrong-host-ip",
            "the interface is on the right subnet at the wrong address (the lidar unicasts to ONE host)",
        ),

        /** Addressing correct, nothing heard. Now it is the lidar's power or the discovery listener. */
        ADDRESS_OK_NO_LIDAR(
            "ip-ok-no-lidar",
            "addressing is correct and no heartbeat has been heard (check the lidar's power, then the listener below)",
        ),

        /** A fresh heartbeat, on the right address. Nothing to diagnose. */
        OK("ok", "heartbeat heard on the expected host address — the chain is healthy"),
        ;
    }

    /**
     * The rung the chain is stuck on, as [Mid360Diagnosis] would classify it.
     *
     * The three Android-shaped facts are derived here rather than by the
     * caller, so the Settings row, the wizard poll and the auto-detect hook
     * cannot each derive them slightly differently:
     *
     *  * **linkUp** — ConnectivityManager has an Ethernet network, OR the raw
     *    kernel view has a wired-looking interface that is up. The `OR` is not
     *    redundancy: an interface can be up and addressed while
     *    `ConnectivityManager` has no `Network` for it (no `EthernetNetworkFactory`
     *    on the build), and calling that "no adapter" would be a lie the
     *    interface list underneath it immediately contradicts.
     *  * **adapterPresent** — that, OR any enumerated USB device that either
     *    announces a network function in its interface descriptors or whose
     *    name matches [Mid360Diagnosis.looksLikeEthernetAdapter].
     *  * **interfaceAddresses** — ConnectivityManager's when it has a network,
     *    the wired interfaces' otherwise.
     */
    fun stepFor(sweep: ConnectionSweep): Mid360Diagnosis.Step {
        val wired = sweep.interfaces.filter { it.looksWired }
        val linkUp = sweep.ethernet.present || wired.any { it.up }
        val adapterPresent = linkUp ||
            sweep.usb.any { it.hasNetworkFunction || Mid360Diagnosis.looksLikeEthernetAdapter(it.searchableName) }
        val addresses = if (sweep.ethernet.present && sweep.ethernet.addresses.isNotEmpty()) {
            sweep.ethernet.addresses
        } else {
            wired.flatMap { it.addresses }
        }
        return Mid360Diagnosis.classify(
            adapterPresent = adapterPresent,
            linkUp = linkUp,
            interfaceAddresses = addresses,
            usbDeviceNames = sweep.usb.map { it.searchableName },
            expectedHostIp = sweep.expectedHostIp,
            heartbeatAgeMillis = sweep.heartbeatAgeMillis,
        )
    }

    /** [stepFor], named in developer language. See the class doc for why there is only one ladder. */
    fun verdictFor(sweep: ConnectionSweep): Verdict = when (stepFor(sweep).state) {
        // The one split the ladder makes in its DETAIL string and not in its
        // state — and the owner's case 1 vs case 2. Same test, one place.
        Mid360Diagnosis.State.NO_ADAPTER ->
            if (sweep.usb.isEmpty()) Verdict.NOTHING_ON_USB else Verdict.USB_PRESENT_NO_ETHERNET
        // One rung of the ladder, two verdicts — split on whether the kernel
        // made an interface at all. "No interface was ever created" and "an
        // interface exists and is down" have different next steps (the hub
        // versus the cable), and the owner's case 2 is the first of them.
        Mid360Diagnosis.State.ADAPTER_NO_LINK ->
            if (sweep.interfaces.any { it.looksWired }) Verdict.INTERFACE_DOWN
            else Verdict.ETHERNET_FUNCTION_NO_INTERFACE
        Mid360Diagnosis.State.LINK_NO_IP -> Verdict.LINK_NO_IP
        Mid360Diagnosis.State.WRONG_SUBNET -> Verdict.INTERFACE_WRONG_SUBNET
        Mid360Diagnosis.State.WRONG_HOST_IP -> Verdict.WRONG_HOST_IP
        Mid360Diagnosis.State.IP_OK_NO_LIDAR -> Verdict.ADDRESS_OK_NO_LIDAR
        Mid360Diagnosis.State.OK -> Verdict.OK
    }

    /**
     * The one line that leads every sweep, and the only line a hurried reader
     * has to read.
     *
     * `sweep verdict=usb-present-no-ethernet trigger=wizard-poll usb=2 ifaces=3 eth=absent — USB devices are …`
     *
     * The counts are on the verdict line on purpose: `usb=0` versus `usb=2` IS
     * the owner's case-1/case-2 split, restated as a number so a reader
     * skimming a hundred lines of log can see it change without parsing the
     * prose.
     */
    fun verdictLine(sweep: ConnectionSweep): String {
        val verdict = verdictFor(sweep)
        return buildString {
            append("sweep verdict=").append(verdict.token)
            append(" trigger=").append(sweep.trigger)
            append(" usb=").append(sweep.usb.size)
            append(" ifaces=").append(sweep.interfaces.size)
            append(" eth=").append(if (sweep.ethernet.present) sweep.ethernet.interfaceName ?: "present" else "absent")
            append(" host=").append(sweep.expectedHostIp)
            append(" hb=").append(sweep.heartbeatAgeMillis?.let { "${it}ms" } ?: "never")
            append(" — ").append(verdict.headline)
        }
    }

    /**
     * The full block: the verdict line, then USB, then interfaces, then
     * ConnectivityManager's view, then discovery.
     *
     * That order is the physical order of the chain, same as the ladder's, so
     * the section the verdict points at is always the next one down rather
     * than somewhere in the middle.
     *
     * @param extraVerdictSuffix appended to the verdict line only — this is
     *   where the rate limiter's "(+N more since the last line)" goes, so the
     *   suppressed count lands on the line people actually read instead of at
     *   the bottom of a twenty-line block.
     */
    fun format(sweep: ConnectionSweep, extraVerdictSuffix: String = ""): String {
        val out = StringBuilder()
        out.append(verdictLine(sweep)).append(extraVerdictSuffix).append('\n')

        out.append("  usb: ").append(sweep.usb.size).append(" device(s)")
        if (sweep.usb.isEmpty()) {
            // Said in words as well as in the count, because "0" in a list
            // header is the single most skimmed-past number in any log.
            out.append(" — NOTHING enumerated on USB")
        }
        out.append('\n')
        sweep.usb.forEach { out.append("    ").append(it.describe()).append('\n') }

        out.append("  interfaces: ").append(sweep.interfaces.size).append('\n')
        sweep.interfaces.forEach { out.append("    ").append(it.describe()).append('\n') }

        out.append("  connectivity: ")
        if (sweep.ethernet.present) {
            out.append("ethernet network on ").append(sweep.ethernet.interfaceName ?: "?")
            out.append(" addr=")
                .append(if (sweep.ethernet.addresses.isEmpty()) "[]" else sweep.ethernet.addresses.joinToString(",", "[", "]"))
            out.append(" internet=").append(sweep.ethernet.hasInternet)
        } else {
            out.append("no ethernet-transport network")
        }
        // Whether the OS was ASKED for the network changes what its absence
        // means — see EthernetMonitor's requestNetwork/registerNetworkCallback note.
        out.append(" via=").append(if (sweep.ethernet.usingRequest) "requestNetwork" else "registerNetworkCallback")
        if (sweep.ethernet.lastEvent.isNotBlank()) out.append(" last=\"").append(sweep.ethernet.lastEvent).append('"')
        out.append('\n')

        val discovery = sweep.discovery
        out.append("  discovery: udp/").append(discovery.port)
        out.append(" listening=").append(discovery.listening)
        out.append(" heard=").append(discovery.datagrams.size)
        if (discovery.datagramsDropped > 0) out.append(" dropped=").append(discovery.datagramsDropped)
        out.append('\n')
        discovery.datagrams.forEach { out.append("    rx ").append(it.describe()).append('\n') }

        out.append("  serial probes: ").append(discovery.probes.size).append('\n')
        discovery.probes.forEach { out.append("    ").append(it.describe()).append('\n') }

        return out.toString().trimEnd('\n')
    }
}
