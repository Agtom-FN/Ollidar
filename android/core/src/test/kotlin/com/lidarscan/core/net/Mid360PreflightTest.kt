package com.lidarscan.core.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ROUND 14 (owner item 53).
 *
 * The addresses are the owner's real hardware: lidar `192.168.1.159`, persisted
 * host `192.168.1.5` (`captures/FIELD_SESSION_2026-08-17.md`), against the
 * `192.168.1.100` an Android USB-Ethernet adapter picks up by DHCP.
 */
class Mid360PreflightTest {

    private val OWNER_HOST = "192.168.1.5"

    @Test
    fun `no adapter is refused before anything is recorded`() {
        val v = Mid360Preflight.evaluate(
            adapterPresent = false,
            interfaceAddresses = emptyList(),
            expectedHostIp = OWNER_HOST,
            heartbeatAgeMillis = 500L,
        )
        assertFalse(v.ok)
        assertTrue(v.blocking)
        assertEquals("no-ethernet", v.logToken)
    }

    @Test
    fun `an adapter with no address yet is refused`() {
        val v = Mid360Preflight.evaluate(
            adapterPresent = true,
            interfaceAddresses = emptyList(),
            expectedHostIp = OWNER_HOST,
            heartbeatAgeMillis = 500L,
        )
        assertEquals("no-address", v.logToken)
        assertTrue(v.blocking)
    }

    @Test
    fun `the DHCP address is the owner's actual failure and it names the fix`() {
        // This is scan-031 and scan-032: heartbeat heard (so the lidar IS on
        // the cable and powered), addressing wrong, therefore zero bytes.
        val v = Mid360Preflight.evaluate(
            adapterPresent = true,
            interfaceAddresses = listOf("192.168.1.100"),
            expectedHostIp = OWNER_HOST,
            heartbeatAgeMillis = 800L,
        )
        assertFalse(v.ok)
        assertTrue(v.blocking)
        assertEquals("host-ip-mismatch", v.logToken)
        assertTrue(v.summary, v.summary.contains("192.168.1.5"))
        assertTrue(v.summary, v.summary.contains("192.168.1.100"))
        val fix = v.fix
        assertNotNull(fix)
        // The operator must be able to act on this without leaving the sentence.
        assertTrue(fix!!, fix.contains("Static"))
        assertTrue(fix, fix.contains("192.168.1.5"))
        assertTrue(fix, fix.contains("255.255.255.0"))
        // And it must NOT be the 0.8.0 advice, which was wrong.
        assertFalse(v.summary + fix, (v.summary + fix).contains("re-seat", ignoreCase = true))
    }

    @Test
    fun `correct addressing with no heartbeat blames power and cable, not the IP`() {
        val v = Mid360Preflight.evaluate(
            adapterPresent = true,
            interfaceAddresses = listOf(OWNER_HOST),
            expectedHostIp = OWNER_HOST,
            heartbeatAgeMillis = null,
        )
        assertEquals("no-heartbeat", v.logToken)
        assertTrue(v.blocking)
        assertTrue(v.fix!!, v.fix!!.contains("power"))
    }

    @Test
    fun `a stale heartbeat does not count as a live link`() {
        val v = Mid360Preflight.evaluate(
            adapterPresent = true,
            interfaceAddresses = listOf(OWNER_HOST),
            expectedHostIp = OWNER_HOST,
            heartbeatAgeMillis = Mid360Preflight.HEARTBEAT_FRESH_MILLIS + 1,
        )
        assertEquals("no-heartbeat", v.logToken)
    }

    @Test
    fun `the configured rig passes`() {
        val v = Mid360Preflight.evaluate(
            adapterPresent = true,
            interfaceAddresses = listOf(OWNER_HOST),
            expectedHostIp = OWNER_HOST,
            heartbeatAgeMillis = 1_000L,
        )
        assertTrue(v.summary, v.ok)
        assertFalse(v.blocking)
        assertEquals("ok", v.logToken)
    }

    @Test
    fun `a phone holding several addresses passes if one of them is the host`() {
        // An adapter can carry a link-local alongside the static address.
        val v = Mid360Preflight.evaluate(
            adapterPresent = true,
            interfaceAddresses = listOf("169.254.3.7", OWNER_HOST),
            expectedHostIp = OWNER_HOST,
            heartbeatAgeMillis = 1_000L,
        )
        assertTrue(v.ok)
    }

    @Test
    fun `the addressing check runs before the heartbeat check`() {
        // Order matters for the operator: being told "no heartbeat, check the
        // power" when the real fault is the IP is exactly the wrong-diagnosis
        // failure this class exists to end.
        val v = Mid360Preflight.evaluate(
            adapterPresent = true,
            interfaceAddresses = listOf("192.168.1.100"),
            expectedHostIp = OWNER_HOST,
            heartbeatAgeMillis = null,
        )
        assertEquals("host-ip-mismatch", v.logToken)
    }
}
