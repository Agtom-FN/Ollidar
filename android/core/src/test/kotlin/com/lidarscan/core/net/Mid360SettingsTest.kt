package com.lidarscan.core.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * B3. Plain-JVM tests for the only part of the Mid-360 path that can be
 * tested without hardware: the address/port validation and the self-test
 * verdict logic. Everything else in B3 is the network, and android/NOTES.md
 * says so explicitly rather than pretending otherwise.
 */
class Ipv4Test {

    @Test
    fun `parses a dotted quad`() {
        assertEquals(0xC0A80164L, Ipv4.parse("192.168.1.100"))
        assertEquals(0L, Ipv4.parse("0.0.0.0"))
        assertEquals(0xFFFFFFFFL, Ipv4.parse("255.255.255.255"))
    }

    @Test
    fun `rejects the shapes a human actually types`() {
        // Each of these is a real thing someone types into an IP field, and
        // each would be silently reinterpreted by InetAddress/inet_addr.
        assertNull(Ipv4.parse("192.168.1"))
        assertNull(Ipv4.parse("192.168.1.256"))
        assertNull(Ipv4.parse("192.168.1.100.1"))
        assertNull(Ipv4.parse("192.168.1."))
        assertNull(Ipv4.parse(""))
        assertNull(Ipv4.parse("192.168.1.-1"))
        assertNull(Ipv4.parse("192.168.1.1a"))
        // Leading zeros: inet_addr reads 010 as OCTAL 8. Refusing is the only
        // safe answer for a field whose failure mode is total silence.
        assertNull(Ipv4.parse("192.168.001.100"))
    }

    @Test
    fun `same subnet honours the prefix length`() {
        assertTrue(Ipv4.sameSubnet("192.168.1.5", "192.168.1.100", 24))
        assertFalse(Ipv4.sameSubnet("192.168.1.5", "192.168.2.100", 24))
        assertTrue(Ipv4.sameSubnet("192.168.1.5", "192.168.2.100", 16))
        assertFalse(Ipv4.sameSubnet("10.0.0.1", "192.168.1.1", 8))
        // A /32 is "the same address", not "the same subnet".
        assertFalse(Ipv4.sameSubnet("192.168.1.5", "192.168.1.6", 32))
        assertTrue(Ipv4.sameSubnet("192.168.1.5", "192.168.1.5", 32))
    }

    @Test
    fun `classifies loopback, multicast and broadcast`() {
        assertTrue(Ipv4.isLoopback("127.0.0.1"))
        // The engine's own loopback sim writes 127.000.000.001 to slip past
        // SDK2's self-IP filter; that string is not a valid quad here, which
        // is exactly right — A3 §7 says not to copy it into production.
        assertNull(Ipv4.parse("127.000.000.001"))
        assertFalse(Ipv4.isLoopback("192.168.1.5"))
        assertTrue(Ipv4.isMulticastOrBroadcast("239.255.0.1"))
        assertTrue(Ipv4.isMulticastOrBroadcast("255.255.255.255"))
        assertFalse(Ipv4.isMulticastOrBroadcast("192.168.1.100"))
    }
}

class Mid360SettingsValidationTest {

    private val ethernet = listOf(LocalAddress("192.168.1.5", 24))

    @Test
    fun `the shipped defaults validate against a matching interface`() {
        val v = validateMid360Settings(Mid360Settings(), ethernet)
        assertTrue("defaults should be usable: ${v.issues.map { it.message }}", v.isUsable)
        assertTrue(v.fatalIssues.isEmpty())
    }

    @Test
    fun `both addresses are required and each says why`() {
        val v = validateMid360Settings(Mid360Settings(lidarIp = "", hostIp = ""))
        assertEquals(2, v.fatalIssues.size)
        val lidar = v.firstFor(Mid360Field.LIDAR_IP)!!
        val host = v.firstFor(Mid360Field.HOST_IP)!!
        assertTrue(lidar.fatal && host.fatal)
        // The reasons are different and must stay different: one is "we
        // cannot find it", the other is "it cannot find us".
        assertTrue(lidar.message.contains("broadcast"))
        assertTrue(host.message.contains("void"))
    }

    @Test
    fun `a host IP the interface does not hold is fatal, not a warning`() {
        // The single highest-value check in the whole wizard: this is the
        // misconfiguration that produces no error at all, just an empty
        // .lscan 20 minutes later.
        val v = validateMid360Settings(Mid360Settings(hostIp = "192.168.1.9"), ethernet)
        assertFalse(v.isUsable)
        val issue = v.firstFor(Mid360Field.HOST_IP)!!
        assertTrue(issue.fatal)
        assertTrue(issue.message.contains("192.168.1.5/24"))
    }

    @Test
    fun `with no interface addresses the locality check degrades to a warning`() {
        // Refusing to let the user fill the form in before the cable is in
        // would be the wrong trade — but silently claiming the host IP is
        // fine would be worse, so it is stated as unchecked.
        val v = validateMid360Settings(Mid360Settings(), localAddresses = emptyList())
        assertTrue(v.isUsable)
        assertTrue(v.warnings.any { it.message.contains("Not checked") })
    }

    @Test
    fun `loopback host is refused outright`() {
        val v = validateMid360Settings(Mid360Settings(hostIp = "127.0.0.1"))
        assertFalse(v.isUsable)
        assertTrue(v.firstFor(Mid360Field.HOST_IP)!!.message.contains("simulator"))
    }

    @Test
    fun `host and lidar may not share an address`() {
        val v = validateMid360Settings(
            Mid360Settings(lidarIp = "192.168.1.5", hostIp = "192.168.1.5"),
            ethernet,
        )
        assertFalse(v.isUsable)
    }

    @Test
    fun `a cross-subnet pair is a warning, not an error`() {
        // It is legal with a router in between, and the wizard should not
        // block a setup that can work — but on a direct cable it cannot, and
        // that is worth saying.
        val v = validateMid360Settings(
            Mid360Settings(lidarIp = "10.0.0.100", hostIp = "192.168.1.5"),
            ethernet,
        )
        assertTrue(v.isUsable)
        assertTrue(v.warnings.any { it.message.contains("/24 subnets") })
    }

    @Test
    fun `port ranges and host-port collisions are caught`() {
        assertFalse(validateMid360Settings(Mid360Settings(devicePointPort = 0), ethernet).isUsable)
        assertFalse(validateMid360Settings(Mid360Settings(devicePointPort = 70000), ethernet).isUsable)
        val collided = validateMid360Settings(
            Mid360Settings(hostPointPort = 56301, hostImuPort = 56301),
            ethernet,
        )
        assertFalse(collided.isUsable)
        assertTrue(collided.firstFor(Mid360Field.PORTS)!!.message.contains("must differ"))
    }

    @Test
    fun `the raw-UDP backend warns about what it cannot do, without blocking`() {
        val v = validateMid360Settings(
            Mid360Settings(backend = Mid360Settings.BACKEND_RAW_UDP),
            ethernet,
        )
        assertTrue(v.isUsable)
        val note = v.firstFor(Mid360Field.BACKEND)!!
        assertFalse(note.fatal)
        assertTrue(note.message.contains("listen-only"))
        assertTrue(note.message.contains("IMU is off"))
    }
}
