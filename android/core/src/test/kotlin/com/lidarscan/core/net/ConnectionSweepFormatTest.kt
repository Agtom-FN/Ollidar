package com.lidarscan.core.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 25 item 118, **owner amendment**.
 *
 * The three cases the owner needs distinguished are the first three tests in
 * this file, and they are named after his words rather than after the enum:
 *
 *  1. nothing on USB at all;
 *  2. a USB device is enumerated and no network interface appeared;
 *  3. an interface came up on the wrong subnet.
 *
 * They are driven by **synthetic** device and interface lists — an Acer
 * HY41-T9-shaped hub, an ASIX dongle, a phone on hotel Wi-Fi — because none of
 * these can be produced on a bench on demand and all three have cost a site
 * visit.
 *
 * The formatter's exact prose is deliberately NOT asserted character by
 * character; what is asserted is that the verdict token is right, that it is
 * on the FIRST line (a reader who reads one line must get the answer), and
 * that the evidence which distinguishes the case is present in the block.
 */
class ConnectionSweepFormatTest {

    // ── synthetic hardware ─────────────────────────────────────────────────

    /** The owner's Acer HY41-T9-shaped hub: a hub function and a card reader, no network function. */
    private val acerHub = UsbDeviceRecord(
        deviceName = "/dev/bus/usb/001/004",
        vendorId = 0x2109,
        productId = 0x0813,
        productName = "HY41-T9",
        manufacturerName = "Acer",
        deviceClass = 0x09,
        interfaces = listOf(
            UsbInterfaceRecord(index = 0, interfaceClass = 0x09, interfaceSubclass = 0x00, interfaceProtocol = 0x02),
            UsbInterfaceRecord(index = 1, interfaceClass = 0x08, interfaceSubclass = 0x06, interfaceProtocol = 0x50),
        ),
    )

    /** A CDC-ECM dongle: announces a network function in its descriptors, so the verdict can say so. */
    private val cdcDongle = UsbDeviceRecord(
        deviceName = "/dev/bus/usb/001/005",
        vendorId = 0x0b95,
        productId = 0x1790,
        productName = "AX88179 Gigabit Ethernet",
        manufacturerName = "ASIX",
        interfaces = listOf(
            UsbInterfaceRecord(index = 0, interfaceClass = 0x02, interfaceSubclass = 0x06, interfaceProtocol = 0x00),
            UsbInterfaceRecord(index = 1, interfaceClass = 0x0a, interfaceSubclass = 0x00, interfaceProtocol = 0x00),
        ),
    )

    private val loopback = NetInterfaceRecord(
        name = "lo",
        up = true,
        loopback = true,
        mtu = 65536,
        addresses = listOf("127.0.0.1/8"),
    )

    private val wifi = NetInterfaceRecord(
        name = "wlan0",
        up = true,
        loopback = false,
        mtu = 1500,
        addresses = listOf("192.168.0.24/24"),
    )

    private fun eth(vararg addresses: String, up: Boolean = true) = NetInterfaceRecord(
        name = "eth0",
        up = up,
        loopback = false,
        mtu = 1500,
        addresses = addresses.toList(),
    )

    // ── owner case 1: nothing on USB ───────────────────────────────────────

    @Test
    fun `nothing on usb and no ethernet is case one`() {
        val sweep = ConnectionSweep(
            trigger = "wizard-poll",
            usb = emptyList(),
            interfaces = listOf(loopback, wifi),
        )

        assertEquals(ConnectionSweepFormat.Verdict.NOTHING_ON_USB, ConnectionSweepFormat.verdictFor(sweep))

        val block = ConnectionSweepFormat.format(sweep)
        val first = block.lineSequence().first()
        assertTrue("verdict must be on the first line: $first", first.contains("verdict=nothing-on-usb"))
        assertTrue("the first line must name the case: $first", first.contains("case 1 of 3"))
        assertTrue("the count that makes the case must be on the first line: $first", first.contains("usb=0"))
        // The zero is said in words too — a bare "0" in a list header is the
        // most skimmed-past number in any log.
        assertTrue(block.contains("NOTHING enumerated on USB"))
    }

    // ── owner case 2: USB device present, no network interface ─────────────

    @Test
    fun `usb device present with no ethernet interface is case two`() {
        val sweep = ConnectionSweep(
            trigger = "wizard-poll",
            usb = listOf(acerHub),
            interfaces = listOf(loopback, wifi),
        )

        assertEquals(
            ConnectionSweepFormat.Verdict.USB_PRESENT_NO_ETHERNET,
            ConnectionSweepFormat.verdictFor(sweep),
        )

        val block = ConnectionSweepFormat.format(sweep)
        val first = block.lineSequence().first()
        assertTrue(first.contains("verdict=usb-present-no-ethernet"))
        assertTrue("the first line must name the case: $first", first.contains("case 2 of 3"))
        assertTrue("a powered hub is the fix and must be on the first line: $first", first.contains("POWERED"))
        assertTrue("the count that makes the case must be on the first line: $first", first.contains("usb=1"))

        // The evidence that separates case 2 from case 1 is the descriptor
        // detail: what the device IS, and what functions it does not have.
        assertTrue("VID:PID must be printed in hex", block.contains("2109:0813"))
        assertTrue(block.contains("HY41-T9"))
        assertTrue(block.contains("Acer"))
        assertTrue("per-interface classes are the whole point", block.contains("class=0x09(hub)"))
        assertTrue(block.contains("class=0x08(mass-storage)"))
        assertFalse("this hub has no network function and must not be marked as one", block.contains(" NET"))
    }

    @Test
    fun `a cdc ethernet function that produced no interface is the sharper half of case two`() {
        // The sharper half of case 2: the device ANNOUNCES an Ethernet
        // function and the OS still brought nothing up. That is a driver or a
        // power problem, not "this is not an adapter", and the log must say
        // which — the fix (a powered hub) is the same, the confidence is not.
        val sweep = ConnectionSweep(
            trigger = "settings-row",
            usb = listOf(cdcDongle),
            interfaces = listOf(loopback, wifi),
        )

        assertEquals(
            ConnectionSweepFormat.Verdict.ETHERNET_FUNCTION_NO_INTERFACE,
            ConnectionSweepFormat.verdictFor(sweep),
        )
        val block = ConnectionSweepFormat.format(sweep)
        val first = block.lineSequence().first()
        assertTrue("it is still case 2 for the reader: $first", first.contains("case 2 of 3"))
        assertTrue(first.contains("POWERED"))
        assertTrue("a CDC control interface must be flagged", block.contains("class=0x02(cdc-control)"))
        assertTrue("a CDC data interface must be flagged", block.contains("class=0x0a(cdc-data)"))
        assertTrue("network functions carry a NET marker", block.contains(" NET"))
    }

    @Test
    fun `both halves of case two say case two on the first line`() {
        // The owner's requirement is that the three cases are distinguishable
        // AT A GLANCE. A reader who only reads the first line must land in the
        // right one of the three whichever half of case 2 this is.
        val unrecognised = ConnectionSweep(trigger = "t", usb = listOf(acerHub), interfaces = listOf(loopback, wifi))
        val announced = ConnectionSweep(trigger = "t", usb = listOf(cdcDongle), interfaces = listOf(loopback, wifi))
        listOf(unrecognised, announced).forEach { sweep ->
            val first = ConnectionSweepFormat.format(sweep).lineSequence().first()
            assertTrue("expected 'case 2 of 3' in: $first", first.contains("case 2 of 3"))
        }
        // …and they are still two different tokens, because the evidence
        // differs and a grep should be able to separate them.
        assertNotEquals(
            ConnectionSweepFormat.verdictFor(unrecognised),
            ConnectionSweepFormat.verdictFor(announced),
        )
    }

    // ── owner case 3: interface up on the wrong subnet ─────────────────────

    @Test
    fun `interface up on the wrong subnet is case three`() {
        val sweep = ConnectionSweep(
            trigger = "wizard-poll",
            usb = listOf(cdcDongle),
            interfaces = listOf(loopback, eth("10.0.0.42/24")),
            ethernet = ConnectivityEthernetRecord(
                present = true,
                interfaceName = "eth0",
                addresses = listOf("10.0.0.42/24"),
                usingRequest = false,
                lastEvent = "Ethernet network available on eth0",
            ),
            expectedHostIp = "192.168.1.5",
        )

        assertEquals(
            ConnectionSweepFormat.Verdict.INTERFACE_WRONG_SUBNET,
            ConnectionSweepFormat.verdictFor(sweep),
        )
        val block = ConnectionSweepFormat.format(sweep)
        val first = block.lineSequence().first()
        assertTrue(first.contains("verdict=wrong-subnet"))
        assertTrue("the first line must name the case: $first", first.contains("case 3 of 3"))
        assertTrue("the expected host belongs on the verdict line: $first", first.contains("host=192.168.1.5"))
        assertTrue("the address actually held must be in the block", block.contains("10.0.0.42/24"))
        assertTrue(
            "which registration path was used changes what an absence means",
            block.contains("via=registerNetworkCallback"),
        )
    }

    @Test
    fun `the three owner cases produce three different verdict tokens`() {
        // The whole amendment in one assertion: these three were one string
        // before, and one string is what cost the site visit.
        val nothing = ConnectionSweep(trigger = "t", interfaces = listOf(loopback))
        val present = ConnectionSweep(trigger = "t", usb = listOf(acerHub), interfaces = listOf(loopback))
        val wrongSubnet = ConnectionSweep(
            trigger = "t",
            usb = listOf(cdcDongle),
            interfaces = listOf(loopback, eth("10.0.0.42/24")),
            ethernet = ConnectivityEthernetRecord(present = true, interfaceName = "eth0", addresses = listOf("10.0.0.42/24")),
        )
        val tokens = listOf(nothing, present, wrongSubnet).map { ConnectionSweepFormat.verdictFor(it).token }
        assertEquals(3, tokens.toSet().size)
        assertEquals(listOf("nothing-on-usb", "usb-present-no-ethernet", "wrong-subnet"), tokens)
    }

    // ── the rest of the ladder ─────────────────────────────────────────────

    @Test
    fun `an interface that exists and is down is not case two`() {
        // eth0 exists and is DOWN: the driver bound and the interface WAS
        // created, so this is not "no network interface appeared" and pointing
        // the reader at the hub would send them to the wrong shop. The next
        // suspect is the cable and the lidar's power.
        val sweep = ConnectionSweep(
            trigger = "t",
            usb = listOf(cdcDongle),
            interfaces = listOf(loopback, eth(up = false)),
        )
        assertEquals(ConnectionSweepFormat.Verdict.INTERFACE_DOWN, ConnectionSweepFormat.verdictFor(sweep))
        val first = ConnectionSweepFormat.format(sweep).lineSequence().first()
        assertFalse("an existing-but-down interface is not one of the three cases", first.contains("case 2 of 3"))
        assertTrue(first.contains("verdict=interface-down"))
    }

    @Test
    fun `link up with no address reads as link-no-ip`() {
        val sweep = ConnectionSweep(
            trigger = "t",
            usb = listOf(cdcDongle),
            interfaces = listOf(loopback, eth()),
            ethernet = ConnectivityEthernetRecord(present = true, interfaceName = "eth0"),
        )
        assertEquals(ConnectionSweepFormat.Verdict.LINK_NO_IP, ConnectionSweepFormat.verdictFor(sweep))
    }

    @Test
    fun `right subnet wrong address reads as wrong-host-ip`() {
        val sweep = ConnectionSweep(
            trigger = "t",
            usb = listOf(cdcDongle),
            interfaces = listOf(loopback, eth("192.168.1.100/24")),
            ethernet = ConnectivityEthernetRecord(
                present = true,
                interfaceName = "eth0",
                addresses = listOf("192.168.1.100/24"),
            ),
            expectedHostIp = "192.168.1.5",
        )
        assertEquals(ConnectionSweepFormat.Verdict.WRONG_HOST_IP, ConnectionSweepFormat.verdictFor(sweep))
    }

    @Test
    fun `correct addressing with no heartbeat reads as ip-ok-no-lidar`() {
        val sweep = okSweep(heartbeatAgeMillis = null)
        assertEquals(ConnectionSweepFormat.Verdict.ADDRESS_OK_NO_LIDAR, ConnectionSweepFormat.verdictFor(sweep))
        assertTrue(ConnectionSweepFormat.verdictLine(sweep).contains("hb=never"))
    }

    @Test
    fun `a fresh heartbeat on the right address reads as ok`() {
        val sweep = okSweep(heartbeatAgeMillis = 400L)
        assertEquals(ConnectionSweepFormat.Verdict.OK, ConnectionSweepFormat.verdictFor(sweep))
        assertTrue(ConnectionSweepFormat.verdictLine(sweep).contains("hb=400ms"))
    }

    @Test
    fun `a stale heartbeat is not ok`() {
        // A device that answered ten minutes ago and has since been unplugged
        // must not read as healthy. Same rule as the wizard's.
        val sweep = okSweep(heartbeatAgeMillis = Mid360Diagnosis.HEARTBEAT_FRESH_MILLIS + 1)
        assertEquals(ConnectionSweepFormat.Verdict.ADDRESS_OK_NO_LIDAR, ConnectionSweepFormat.verdictFor(sweep))
    }

    // ── discovery and probes ───────────────────────────────────────────────

    @Test
    fun `discovery activity is summarised and never dumped`() {
        val sweep = okSweep(heartbeatAgeMillis = 400L).copy(
            discovery = DiscoveryRecord(
                listening = true,
                datagrams = listOf(
                    DatagramRecord("192.168.1.100", 56201, 430, "mid360 sn=MCP7K0034759 host=192.168.1.5"),
                    DatagramRecord("192.168.0.1", 56201, 64, "unparsed prefix=<?xml versio"),
                ),
                probes = listOf(
                    SerialProbeRecord("COIN_D6", "/dev/bus/usb/001/003", SerialProbeRecord.OUTCOME_DECLINED, "no AA55 in 1500ms"),
                    SerialProbeRecord("STL27L", "/dev/bus/usb/001/003", SerialProbeRecord.OUTCOME_UNUSABLE, "USB permission denied"),
                ),
                datagramsDropped = 7,
            ),
        )
        val block = ConnectionSweepFormat.format(sweep)
        assertTrue(block.contains("listening=true"))
        assertTrue(block.contains("heard=2"))
        assertTrue("a bounded buffer must say what it dropped", block.contains("dropped=7"))
        assertTrue("source and size, not the bytes", block.contains("192.168.1.100:56201 430B"))
        assertTrue("a non-heartbeat broadcast is itself a diagnosis", block.contains("192.168.0.1:56201 64B"))
        // The ladder's ORDER is half the diagnosis: an unusable rung 1 means
        // rung 2 never ran, and the rungs must therefore be printed in order.
        val d6 = block.indexOf("COIN_D6")
        val stl = block.indexOf("STL27L")
        assertTrue(d6 in 0 until stl)
        assertTrue(block.contains("-> declined (no AA55 in 1500ms)"))
        assertTrue(block.contains("-> unusable (USB permission denied)"))
    }

    @Test
    fun `the rate limiter suffix lands on the verdict line and nowhere else`() {
        // The suppressed count has to be where people read, not at the bottom
        // of a twenty-line block.
        val block = ConnectionSweepFormat.format(
            okSweep(heartbeatAgeMillis = 400L),
            extraVerdictSuffix = " (+4 more since the last line)",
        )
        assertTrue(block.lineSequence().first().endsWith("(+4 more since the last line)"))
        assertEquals(1, block.lineSequence().count { it.contains("more since the last line") })
    }

    @Test
    fun `the trigger is on the verdict line so a long log stays greppable`() {
        val line = ConnectionSweepFormat.verdictLine(okSweep(heartbeatAgeMillis = 400L).copy(trigger = "settings-row"))
        assertTrue(line.contains("trigger=settings-row"))
    }

    @Test
    fun `connectivity manager disagreeing with the kernel is visible`() {
        // eth0 is up and addressed and ConnectivityManager has no network for
        // it. Calling that "no adapter" would be a lie the interface list
        // immediately contradicts, so the sweep must read the kernel view too.
        val sweep = ConnectionSweep(
            trigger = "t",
            usb = listOf(cdcDongle),
            interfaces = listOf(loopback, eth("192.168.1.5/24")),
            ethernet = ConnectivityEthernetRecord(present = false),
            expectedHostIp = "192.168.1.5",
            heartbeatAgeMillis = 200L,
        )
        assertEquals(ConnectionSweepFormat.Verdict.OK, ConnectionSweepFormat.verdictFor(sweep))
        assertTrue(ConnectionSweepFormat.format(sweep).contains("no ethernet-transport network"))
    }

    // ── the never-dump rule ────────────────────────────────────────────────

    @Test
    fun `an unparseable datagram becomes a bounded printable prefix and never the bytes`() {
        // 4 KB of noise on the discovery port must cost the log a few dozen
        // characters, not four kilobytes. `UdpMid360Detector` keeps listening
        // past a bad parse precisely because this happens, so it happens a lot.
        val noise = ByteArray(4096) { (it % 256).toByte() }
        val summary = DatagramRecord.summarise(noise)
        assertTrue(summary.startsWith("not-a-heartbeat prefix="))
        assertTrue("bounded at ${DatagramRecord.MAX_PREFIX_CHARS} chars, was ${summary.length}", summary.length < 64)
        // Non-printables are replaced, so a control byte cannot inject a
        // newline and forge a second log line.
        assertFalse(summary.contains('\n'))
        assertFalse(summary.contains('\u0000'))
    }

    @Test
    fun `a printable non-heartbeat keeps enough prefix to be recognisable`() {
        // "something else is broadcasting on 56201" is a real diagnosis and
        // needs just enough of the payload to name the something else.
        val ssdp = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n".toByteArray()
        val summary = DatagramRecord.summarise(ssdp)
        assertTrue(summary, summary.contains("M-SEARCH"))
        assertTrue(summary.length < 64)
    }

    @Test
    fun `an empty datagram is summarised rather than crashing the sweep`() {
        assertEquals("not-a-heartbeat prefix=", DatagramRecord.summarise(ByteArray(0)))
    }

    private fun okSweep(heartbeatAgeMillis: Long?) = ConnectionSweep(
        trigger = "wizard-poll",
        usb = listOf(cdcDongle),
        interfaces = listOf(loopback, eth("192.168.1.5/24")),
        ethernet = ConnectivityEthernetRecord(
            present = true,
            interfaceName = "eth0",
            addresses = listOf("192.168.1.5/24"),
            usingRequest = true,
            lastEvent = "Ethernet network available on eth0",
        ),
        expectedHostIp = "192.168.1.5",
        heartbeatAgeMillis = heartbeatAgeMillis,
    )
}
