package com.lidarscan.core.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * AUTO-DETECT: [Mid360HeartbeatParser] against two **real** captured
 * heartbeat payloads — not synthetic fixtures. Extracted byte-for-byte from
 * `captures/mid360_real_30s.livoxdump` (port 56201, the first two records),
 * the same file the field session in `captures/FIELD_SESSION_2026-08-17.md`
 * came from: SN `MCP7K0034759`, lidar IP `192.168.1.159`, persisted host
 * `192.168.1.5`. Copies of the same two files live in
 * `app/src/androidTest/assets/mid360_heartbeat/` for the instrumented
 * counterpart of this test (real `AssetManager` + `DatagramPacket`-shaped
 * byte array, not just a JVM resource stream).
 */
class Mid360HeartbeatParserTest {

    private fun fixture(name: String): ByteArray {
        val stream = javaClass.classLoader.getResourceAsStream("mid360_heartbeat/$name")
            ?: error("test resource mid360_heartbeat/$name not found")
        return stream.use { it.readBytes() }
    }

    @Test
    fun `real fixture 0 decodes the exact field session values`() {
        val heartbeat = Mid360HeartbeatParser.parse(fixture("heartbeat_0.bin"))
        assertTrue("expected a decoded heartbeat", heartbeat != null)
        heartbeat!!
        assertEquals("MCP7K0034759", heartbeat.serialNumber)
        assertEquals("Mid-360", heartbeat.deviceType)
        assertEquals("35010108", heartbeat.firmwareVersion)
        assertEquals("192.168.1.159", heartbeat.lidarIp)
        assertEquals("255.255.255.0", heartbeat.lidarNetmask)
        assertEquals("192.168.1.1", heartbeat.lidarGateway)
        assertEquals("192.168.1.5", heartbeat.persistedHostIp)
        assertEquals(56301, heartbeat.persistedHostPointPort)
        assertEquals(56401, heartbeat.persistedHostImuPort)
    }

    @Test
    fun `real fixture 1 (a later beacon in the same 30s capture) decodes identically`() {
        val heartbeat = Mid360HeartbeatParser.parse(fixture("heartbeat_1.bin"))
        assertTrue("expected a decoded heartbeat", heartbeat != null)
        heartbeat!!
        assertEquals("MCP7K0034759", heartbeat.serialNumber)
        assertEquals("192.168.1.159", heartbeat.lidarIp)
        assertEquals("192.168.1.5", heartbeat.persistedHostIp)
    }

    @Test
    fun `too-short payload is rejected, not crashed on`() {
        assertNull(Mid360HeartbeatParser.parse(ByteArray(10)))
        assertNull(Mid360HeartbeatParser.parse(ByteArray(0)))
    }

    @Test
    fun `wrong start-of-frame byte is rejected`() {
        val real = fixture("heartbeat_0.bin").copyOf()
        real[0] = 0x00
        assertNull(Mid360HeartbeatParser.parse(real))
    }

    @Test
    fun `a length field that does not match the payload size is rejected`() {
        val real = fixture("heartbeat_0.bin").copyOf()
        real[2] = 0x01 // corrupt the little-endian `length` field
        assertNull(Mid360HeartbeatParser.parse(real))
    }

    @Test
    fun `a frame with the SDK envelope but no lidar-IP TLV in range is rejected`() {
        // Same SOF/length/cmd_id shape but padded with zero TLVs beyond
        // that -- there is no 0x0004 tag anywhere, so parsing must fail
        // closed rather than guess.
        val fake = ByteArray(120)
        fake[0] = 0xAA.toByte()
        fake[2] = (fake.size and 0xFF).toByte()
        fake[3] = ((fake.size shr 8) and 0xFF).toByte()
        fake[8] = 0x02
        fake[9] = 0x01
        assertNull(Mid360HeartbeatParser.parse(fake))
    }

    @Test
    fun `withDetectedHeartbeat overwrites only the two addresses`() {
        val heartbeat = Mid360HeartbeatParser.parse(fixture("heartbeat_0.bin"))!!
        val original = Mid360Settings(
            lidarIp = "10.0.0.1",
            hostIp = "10.0.0.2",
            backend = Mid360Settings.BACKEND_RAW_UDP,
        )
        val applied = original.withDetectedHeartbeat(heartbeat)
        assertEquals("192.168.1.159", applied.lidarIp)
        assertEquals("192.168.1.5", applied.hostIp)
        // Everything else (ports, backend) untouched.
        assertEquals(original.backend, applied.backend)
        assertEquals(original.devicePointPort, applied.devicePointPort)
    }
}
