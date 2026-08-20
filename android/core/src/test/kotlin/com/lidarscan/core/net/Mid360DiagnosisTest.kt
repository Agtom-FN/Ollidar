package com.lidarscan.core.net

import com.lidarscan.core.WordingLaw
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 25 (owner item 118).
 *
 * Every rung of the ladder, driven by a **synthetic** interface/USB list, so
 * the states the owner cannot reproduce on a bench (an adapter the kernel
 * will not drive; a hub browning out) are asserted here rather than hoped for
 * in the field.
 *
 * The addresses are the owner's real hardware throughout: host `192.168.1.5`,
 * against the `192.168.1.100` an Android USB-Ethernet adapter picks up by
 * DHCP and the `10.0.0.x` a phone gets from a hotel switch.
 */
class Mid360DiagnosisTest {

    private val host = Mid360Settings.DEFAULT_HOST_IP

    private fun classify(
        adapterPresent: Boolean = true,
        linkUp: Boolean = true,
        interfaceAddresses: List<String> = listOf("192.168.1.5"),
        usbDeviceNames: List<String> = emptyList(),
        expectedHostIp: String? = Mid360Settings.DEFAULT_HOST_IP,
        heartbeatAgeMillis: Long? = 500L,
    ) = Mid360Diagnosis.classify(
        adapterPresent = adapterPresent,
        linkUp = linkUp,
        interfaceAddresses = interfaceAddresses,
        usbDeviceNames = usbDeviceNames,
        expectedHostIp = expectedHostIp,
        heartbeatAgeMillis = heartbeatAgeMillis,
    )

    // ── state 1: the owner's field failure, and the split that is the point ──

    /**
     * The exact shape of the 18:53/18:55 log lines: no Ethernet interface, and
     * — because the hub never enumerated either — nothing else on USB.
     */
    @Test
    fun `no adapter and no USB devices at all reads as nothing plugged in`() {
        val step = classify(adapterPresent = false, linkUp = false, usbDeviceNames = emptyList())

        assertEquals(Mid360Diagnosis.State.NO_ADAPTER, step.state)
        assertEquals("no-adapter", step.logToken)
        assertEquals(Mid360Diagnosis.NO_ADAPTER_NOTHING_PLUGGED, step.detail)
        assertTrue("the USB list is the evidence — it must be shown", step.showsUsbDevices)
    }

    /**
     * The whole reason item 118 exists: the SAME "no Ethernet adapter" the
     * owner saw, but with USB devices enumerated. The kernel is fine, USB is
     * fine, and this adapter is either undriveable or under-powered — which is
     * a completely different next step from "plug something in".
     */
    @Test
    fun `no adapter but USB devices present reads as an adapter that did not come up`() {
        val step = classify(
            adapterPresent = false,
            linkUp = false,
            usbDeviceNames = listOf("USB2.0 Hub", "CH340 Serial"),
        )

        assertEquals(Mid360Diagnosis.State.NO_ADAPTER, step.state)
        assertEquals(Mid360Diagnosis.NO_ADAPTER_UNSUPPORTED, step.detail)
    }

    /** The two NO_ADAPTER details must actually differ, or the split is decorative. */
    @Test
    fun `the two no-adapter details are different sentences`() {
        assertNotEquals(
            Mid360Diagnosis.NO_ADAPTER_NOTHING_PLUGGED,
            Mid360Diagnosis.NO_ADAPTER_UNSUPPORTED,
        )
    }

    /** Both of them name the powered hub, because that is the fix the owner did not have. */
    @Test
    fun `both no-adapter details name a powered hub`() {
        assertTrue(Mid360Diagnosis.NO_ADAPTER_NOTHING_PLUGGED.contains("powered"))
        assertTrue(Mid360Diagnosis.NO_ADAPTER_UNSUPPORTED.contains("powered"))
    }

    // ── state 2: hardware there, no link ────────────────────────────────────

    @Test
    fun `an enumerated adapter with no OS network is a link problem`() {
        val step = classify(
            adapterPresent = true,
            linkUp = false,
            usbDeviceNames = listOf("AX88179A Gigabit Ethernet"),
        )

        assertEquals(Mid360Diagnosis.State.ADAPTER_NO_LINK, step.state)
        assertEquals("adapter-no-link", step.logToken)
        assertTrue(step.detail.contains("cable"))
        assertTrue(step.detail.contains("power"))
    }

    // ── state 3: link, no address ───────────────────────────────────────────

    @Test
    fun `a link with no address names the static IP to set`() {
        val step = classify(linkUp = true, interfaceAddresses = emptyList())

        assertEquals(Mid360Diagnosis.State.LINK_NO_IP, step.state)
        assertEquals("link-no-ip", step.logToken)
        assertTrue(step.detail.contains(host))
        assertTrue("255.255.255.0 is the netmask to type", step.detail.contains("255.255.255.0"))
        assertTrue("Settings is where this is fixed", step.showsEthernetSettings)
    }

    /** Garbage on the interface is no address at all, not a wrong one. */
    @Test
    fun `unparseable interface addresses are treated as no address`() {
        val step = classify(interfaceAddresses = listOf("", "fe80::1", "not-an-ip"))
        assertEquals(Mid360Diagnosis.State.LINK_NO_IP, step.state)
    }

    // ── states 3b/3c: wrong subnet vs right subnet, wrong host ──────────────

    /** A phone that joined some other network entirely. Nothing can route. */
    @Test
    fun `an address on another subnet is wrong-subnet`() {
        val step = classify(interfaceAddresses = listOf("10.0.0.42"))

        assertEquals(Mid360Diagnosis.State.WRONG_SUBNET, step.state)
        assertEquals("wrong-subnet", step.logToken)
        assertTrue(step.showsAddresses)
    }

    /**
     * The DHCP address, which is the owner's round-14 zero-byte capture: right
     * cable, right /24, and the lidar is unicasting to an address this phone
     * does not hold. It must NOT be reported as a subnet problem — the fix is
     * one octet, not a new network.
     */
    @Test
    fun `the DHCP address on the right subnet is wrong-host-ip and not wrong-subnet`() {
        val step = classify(interfaceAddresses = listOf("192.168.1.100"))

        assertEquals(Mid360Diagnosis.State.WRONG_HOST_IP, step.state)
        assertEquals("wrong-host-ip", step.logToken)
        assertTrue(step.detail.contains(host))
    }

    /** /24 arithmetic, not a shared string prefix: 192.168.1.x and 192.168.10.x are different networks. */
    @Test
    fun `a shared text prefix is not a shared subnet`() {
        val step = classify(interfaceAddresses = listOf("192.168.10.5"))
        assertEquals(Mid360Diagnosis.State.WRONG_SUBNET, step.state)
    }

    /** Multi-homed: one right address among wrong ones still counts. */
    @Test
    fun `the host address counts wherever it sits in the list`() {
        val step = classify(interfaceAddresses = listOf("10.0.0.42", "192.168.1.5"))
        assertEquals(Mid360Diagnosis.State.OK, step.state)
    }

    /** Android hands addresses back as `ip/prefix` in some places and bare in others. */
    @Test
    fun `a slash-prefix address parses the same as a bare one`() {
        assertEquals(Mid360Diagnosis.State.OK, classify(interfaceAddresses = listOf("192.168.1.5/24")).state)
        assertEquals(
            Mid360Diagnosis.State.WRONG_HOST_IP,
            classify(interfaceAddresses = listOf("192.168.1.100/24")).state,
        )
    }

    // ── state 4: addressing right, lidar silent ─────────────────────────────

    @Test
    fun `correct addressing with no heartbeat ever runs discovery`() {
        val step = classify(heartbeatAgeMillis = null)

        assertEquals(Mid360Diagnosis.State.IP_OK_NO_LIDAR, step.state)
        assertEquals("ip-ok-no-lidar", step.logToken)
        assertTrue("this is the state that listens on 56201", step.runsDiscovery)
    }

    @Test
    fun `a stale heartbeat is not a heartbeat`() {
        val step = classify(heartbeatAgeMillis = Mid360Diagnosis.HEARTBEAT_FRESH_MILLIS + 1)
        assertEquals(Mid360Diagnosis.State.IP_OK_NO_LIDAR, step.state)
    }

    @Test
    fun `a heartbeat right on the freshness boundary still counts`() {
        val step = classify(heartbeatAgeMillis = Mid360Diagnosis.HEARTBEAT_FRESH_MILLIS)
        assertEquals(Mid360Diagnosis.State.OK, step.state)
    }

    // ── the top of the ladder ───────────────────────────────────────────────

    @Test
    fun `everything right is OK and says so with the address`() {
        val step = classify()

        assertEquals(Mid360Diagnosis.State.OK, step.state)
        assertEquals("ok", step.logToken)
        assertTrue(step.isOk)
        assertTrue(step.detail.contains(host))
        assertFalse("nothing left to discover", step.runsDiscovery)
    }

    /** No state may be a dead end — the wizard always offers the next thing. */
    @Test
    fun `every state carries a non-empty instruction and detail`() {
        for (step in everyStep()) {
            assertTrue("blank instruction for ${step.state}", step.instruction.isNotBlank())
            assertTrue("blank detail for ${step.state}", step.detail.isNotBlank())
            assertTrue("blank log token for ${step.state}", step.logToken.isNotBlank())
        }
    }

    @Test
    fun `every state is reachable and every log token is distinct`() {
        val byState = everyStep().associateBy { it.state }
        assertEquals(
            "a state no input can produce is a state nobody tested",
            Mid360Diagnosis.State.entries.toSet(),
            byState.keys,
        )
        val tokens = byState.values.map { it.logToken }
        assertEquals("log tokens must be distinct", tokens.size, tokens.toSet().size)
    }

    // ── the ladder is ordered ───────────────────────────────────────────────

    /**
     * A rung is only reported once every rung below it holds. Asserted by
     * breaking the bottom rung on an otherwise-perfect input and checking the
     * bottom rung is what comes back.
     */
    @Test
    fun `a missing adapter outranks every later problem`() {
        val step = classify(
            adapterPresent = false,
            linkUp = false,
            interfaceAddresses = listOf("10.0.0.42"),
            heartbeatAgeMillis = null,
        )
        assertEquals(Mid360Diagnosis.State.NO_ADAPTER, step.state)
    }

    @Test
    fun `a missing link outranks an addressing problem`() {
        val step = classify(linkUp = false, interfaceAddresses = listOf("10.0.0.42"))
        assertEquals(Mid360Diagnosis.State.ADAPTER_NO_LINK, step.state)
    }

    // ── the expected host IP ────────────────────────────────────────────────

    /** A half-typed field must not break the wizard; it falls back to the factory default. */
    @Test
    fun `a blank or unparseable expected host falls back to the default`() {
        for (bad in listOf(null, "", "   ", "192.168.1", "999.1.1.1")) {
            val step = classify(expectedHostIp = bad, interfaceAddresses = listOf("192.168.1.5"))
            assertEquals("expectedHostIp=$bad", Mid360Diagnosis.State.OK, step.state)
            assertTrue(step.detail.contains(Mid360Settings.DEFAULT_HOST_IP))
        }
    }

    /** The heartbeat's own persisted host wins over the default when one was heard. */
    @Test
    fun `a heartbeat-supplied host is the one compared against`() {
        val step = classify(
            interfaceAddresses = listOf("192.168.1.5"),
            expectedHostIp = "192.168.1.50",
        )
        assertEquals(Mid360Diagnosis.State.WRONG_HOST_IP, step.state)
        assertTrue(step.detail.contains("192.168.1.50"))
    }

    /** There is exactly one copy of the default host address in the codebase. */
    @Test
    fun `the default host address is not duplicated here`() {
        assertEquals("192.168.1.5", Mid360Settings.DEFAULT_HOST_IP)
        assertTrue(Mid360Diagnosis.okDetail(host).contains(Mid360Settings.DEFAULT_HOST_IP))
    }

    // ── the adapter hint ────────────────────────────────────────────────────

    @Test
    fun `the common USB-Ethernet chipsets are recognised`() {
        for (name in listOf(
            "AX88179A Gigabit Ethernet",
            "USB 10-100-1000M LAN",
            "RTL8153 Gigabit Ethernet Adapter",
            "ASIX Elec. Corp. AX88772C",
            "USB Ethernet",
        )) {
            assertTrue(name, Mid360Diagnosis.looksLikeEthernetAdapter(name))
        }
    }

    @Test
    fun `a serial adapter and a plain hub are not Ethernet adapters`() {
        for (name in listOf("CH340 Serial", "USB2.0 Hub", "Android Accessory Interface", "")) {
            assertFalse(name, Mid360Diagnosis.looksLikeEthernetAdapter(name))
        }
    }

    // ── the wording law ─────────────────────────────────────────────────────

    /**
     * The round-22 guard, applied to every string this item adds. The
     * instruction/detail split is checked per-state rather than over one flat
     * list, so a detail cannot be smuggled in as an instruction.
     */
    @Test
    fun `every instruction is six words or fewer`() {
        for (step in everyStep()) {
            assertTrue(
                "${step.state} instruction is ${WordingLaw.wordCount(step.instruction)} words " +
                    "(max ${WordingLaw.MAX_INSTRUCTION_WORDS}): \"${step.instruction}\"",
                WordingLaw.isInstruction(step.instruction),
            )
        }
    }

    @Test
    fun `every detail line is twelve words or fewer`() {
        for (step in everyStep()) {
            assertTrue(
                "${step.state} detail is ${WordingLaw.wordCount(step.detail)} words " +
                    "(max ${WordingLaw.MAX_DETAIL_WORDS}): \"${step.detail}\"",
                WordingLaw.isDetail(step.detail),
            )
        }
    }

    /** Including the labels and the button, which the screen also draws. */
    @Test
    fun `every string in ALL obeys the detail ceiling`() {
        for (line in Mid360Diagnosis.ALL) {
            assertTrue(
                "\"$line\" is ${WordingLaw.wordCount(line)} words",
                WordingLaw.isDetail(line),
            )
        }
    }

    @Test
    fun `no string carries design-document jargon`() {
        for (line in Mid360Diagnosis.ALL) {
            assertTrue("jargon ${WordingLaw.jargonIn(line)} in \"$line\"", WordingLaw.jargonIn(line).isEmpty())
        }
    }

    /**
     * The law's other half: an error says what happened AND what to do. Every
     * non-OK rung's detail must contain something the operator can go and do.
     */
    @Test
    fun `every failure detail tells the operator what to do`() {
        for (step in everyStep().filterNot { it.isOk }) {
            assertTrue(
                "${step.state} detail is not actionable: \"${step.detail}\"",
                WordingLaw.isActionable(step.detail),
            )
        }
    }

    @Test
    fun `ALL contains every rendered instruction and detail`() {
        for (step in everyStep()) {
            assertTrue(
                "${step.state} instruction missing from ALL: \"${step.instruction}\"",
                Mid360Diagnosis.ALL.contains(step.instruction),
            )
            assertTrue(
                "${step.state} detail missing from ALL: \"${step.detail}\"",
                Mid360Diagnosis.ALL.contains(step.detail),
            )
        }
    }

    /**
     * One input per state, at the default host address so the rendered strings
     * match [Mid360Diagnosis.ALL] exactly. Both NO_ADAPTER details are
     * included even though they share a state.
     */
    private fun everyStep(): List<Mid360Diagnosis.Step> = listOf(
        classify(adapterPresent = false, linkUp = false, usbDeviceNames = emptyList()),
        classify(adapterPresent = false, linkUp = false, usbDeviceNames = listOf("USB2.0 Hub")),
        classify(linkUp = false),
        classify(interfaceAddresses = emptyList()),
        classify(interfaceAddresses = listOf("10.0.0.42")),
        classify(interfaceAddresses = listOf("192.168.1.100")),
        classify(heartbeatAgeMillis = null),
        classify(),
    )
}
