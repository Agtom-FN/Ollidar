package com.lidarscan.app.net

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.lidarscan.core.net.Mid360HeartbeatParser
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * AUTO-DETECT, instrumented counterpart of `:core`'s
 * `Mid360HeartbeatParserTest`: the same two real captured heartbeat
 * payloads (`captures/mid360_real_30s.livoxdump`, port 56201), but read
 * through Android's real `AssetManager` on a real device/emulator rather
 * than a JVM classpath resource stream — exercising the actual byte path
 * [UdpMid360Detector] would hand the parser (an `InputStream` → `ByteArray`,
 * not a synthetic fixture built in code).
 *
 * The `:core` test is the fast, always-run one; this one is what proves the
 * asset packaging itself (`app/src/androidTest/assets/mid360_heartbeat/`)
 * actually ships and reads back byte-for-byte on device.
 */
@RunWith(AndroidJUnit4::class)
class Mid360HeartbeatFixtureAndroidTest {

    /**
     * `InstrumentationRegistry.getInstrumentation().context` — the **test**
     * package's Context (`com.lidarscan.app.test`), not
     * `ApplicationProvider.getApplicationContext()`/`.getTargetContext()`
     * (the app-under-test's Context, `com.lidarscan.app`). The fixtures live
     * in `app/src/androidTest/assets/`, which AGP packages into the test
     * APK, not the app APK — asking the target context's `AssetManager` for
     * them throws `FileNotFoundException` even though the file is right
     * there in the project, because it is genuinely not part of that
     * package's assets at runtime.
     */
    private fun readAsset(name: String): ByteArray {
        val context = InstrumentationRegistry.getInstrumentation().context
        return context.assets.open("mid360_heartbeat/$name").use { it.readBytes() }
    }

    @Test
    fun bothRealFixturesAreShippedAndDecodeToTheSameDevice() {
        val heartbeat0 = Mid360HeartbeatParser.parse(readAsset("heartbeat_0.bin"))
        val heartbeat1 = Mid360HeartbeatParser.parse(readAsset("heartbeat_1.bin"))

        assertTrue("heartbeat_0.bin did not decode", heartbeat0 != null)
        assertTrue("heartbeat_1.bin did not decode", heartbeat1 != null)
        heartbeat0!!
        heartbeat1!!

        assertEquals("MCP7K0034759", heartbeat0.serialNumber)
        assertEquals("MCP7K0034759", heartbeat1.serialNumber)
        assertEquals("192.168.1.159", heartbeat0.lidarIp)
        assertEquals("192.168.1.5", heartbeat0.persistedHostIp)
        // Same physical device, two different beacons a second apart in the
        // same capture — identity fields must agree even though per-frame
        // sequence/CRC bytes differ.
        assertEquals(heartbeat0.serialNumber, heartbeat1.serialNumber)
        assertEquals(heartbeat0.lidarIp, heartbeat1.lidarIp)
        assertEquals(heartbeat0.persistedHostIp, heartbeat1.persistedHostIp)
    }
}
