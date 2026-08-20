package com.lidarscan.app.net

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import com.lidarscan.core.net.ConnectionDebugRateLimiter
import com.lidarscan.core.net.ConnectionSweep
import com.lidarscan.core.net.ConnectionSweepFormat
import com.lidarscan.core.net.ConnectivityEthernetRecord
import com.lidarscan.core.net.Mid360Settings
import com.lidarscan.core.net.NetInterfaceRecord
import com.lidarscan.core.net.UsbDeviceRecord
import com.lidarscan.core.net.UsbInterfaceRecord
import java.net.Inet4Address
import java.net.NetworkInterface
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * ROUND 25 item 118, **owner amendment** — the Android half of the connection
 * sweep: copy what the platform knows into the plain `:core` records, hand
 * them to [ConnectionSweepFormat], and write the block to the capture log
 * under `[net-debug]`.
 *
 * ## What this class does NOT contain
 *
 * Any judgement. There is no `if` in here that decides what is wrong — every
 * one of those lives in [ConnectionSweepFormat] and [com.lidarscan.core.net.Mid360Diagnosis],
 * in `:core`, where it is asserted against synthetic device lists on a bare
 * JVM. This class is a transcription layer, deliberately dull, because the
 * platform types it touches cannot be constructed in a unit test and anything
 * clever put here would be untestable by construction.
 *
 * ## Why [EthernetMonitor] rather than a second ConnectivityManager query
 *
 * [EthernetMonitor] already owns the `requestNetwork`-then-fall-back-to-
 * `registerNetworkCallback` dance (a normal app cannot hold
 * `CHANGE_NETWORK_STATE`, so the first call throws on most builds), the
 * `NOT_VPN`-without-`INTERNET` request shape, and the IPv4 filtering. A second
 * implementation of all that inside a diagnostic would be a second thing that
 * can be wrong, and a diagnostic that disagrees with the screen it is
 * diagnosing is worse than none. So the ConnectivityManager view is copied out
 * of the monitor's [EthernetState], and the RAW kernel view
 * (`NetworkInterface.getNetworkInterfaces()`) is collected separately — the
 * two disagreeing is itself a diagnosis and the sweep prints both.
 *
 * ## Rate limiting
 *
 * Every PERIODIC caller goes through [logSweep], which is gated by
 * [ConnectionDebugRateLimiter] at one block per second per category, with the
 * suppressed count reported on the verdict line in the same
 * `(+N more since the last line)` shape `ArSessionGate` has used since round
 * 22. [sweepNow] — the Settings row — is deliberately NOT gated: a person
 * pressing a button is entitled to an answer.
 *
 * ## Developer mode
 *
 * [enabled] mirrors `AppSettings.developerMode`, the same seven-tap unlock
 * `CaptureLog.developerCaptureDebug` reads. With it off, [logSweep] returns
 * before doing any work at all — no binder round-trip, no interface
 * enumeration, nothing. The Settings row that calls [sweepNow] only exists
 * inside the developer section, so it cannot be reached with the flag off.
 *
 * ## Wording law
 *
 * Nothing this class produces is operator-facing, so
 * [com.lidarscan.core.WordingLaw] does not apply to it. Said again here
 * because it is the kind of thing a later reader "fixes": these blocks are
 * diagnostic output for whoever is debugging a connection, and they are
 * supposed to be long and full of numbers.
 */
class ConnectionDebugSweeper(
    context: Context,
    private val ethernetMonitor: EthernetMonitor,
    /** Where a sweep is written. `CaptureLog::log` in production — which also mirrors into an open capture's debug log. */
    private val sink: (tag: String, message: String) -> Unit,
    private val limiter: ConnectionDebugRateLimiter = ConnectionDebugRateLimiter(),
) {

    /**
     * What the CALLER knows that the platform does not: which address the
     * lidar is expected to unicast to, and how long ago a heartbeat was last
     * parsed.
     *
     * Passed per call rather than held, because the two callers know different
     * things and neither can answer for the other. The Mid-360 wizard has a
     * live heartbeat clock and a typed (or beacon-supplied) host address; the
     * Settings row has the last-detected address from `SettingsRepository` and
     * no heartbeat at all. A shared supplier would have to be wrong for one of
     * them, and being wrong about the expected host is precisely how a
     * `wrong-subnet` verdict becomes a lie.
     */
    data class SweepContext(
        val expectedHostIp: String = Mid360Settings.DEFAULT_HOST_IP,
        val heartbeatAgeMillis: Long? = null,
    )

    private val appContext = context.applicationContext

    /** Mirrors `AppSettings.developerMode`. See the class doc. */
    @Volatile
    var enabled: Boolean = false
        set(value) {
            val was = field
            field = value
            ConnectionDebugTrace.enabled = value
            if (value && !was) {
                // Switching developer mode ON is a fresh story and is entitled
                // to speak at once — `ArSessionGate.claim` does the same on a
                // new claim, for the same reason.
                limiter.reopenAll()
            }
            if (!value) ConnectionDebugTrace.clear()
        }

    /**
     * Collects one sweep. Never throws: a diagnostic that crashes while
     * diagnosing is the worst possible outcome, so every platform call below
     * is individually wrapped and degrades to an empty list.
     *
     * Runs on [Dispatchers.IO] — `getDeviceList()` is a binder round-trip and
     * `getNetworkInterfaces()` walks `/sys`, and this is called once a second
     * from a screen that must not stutter while it explains why the screen is
     * not working.
     */
    suspend fun collect(
        trigger: String,
        context: SweepContext = SweepContext(),
    ): ConnectionSweep = withContext(Dispatchers.IO) {
        ConnectionSweep(
            trigger = trigger,
            usb = usbDevices(),
            interfaces = networkInterfaces(),
            ethernet = ethernetRecord(),
            discovery = ConnectionDebugTrace.snapshotAndReset(),
            expectedHostIp = context.expectedHostIp,
            heartbeatAgeMillis = context.heartbeatAgeMillis,
        )
    }

    /**
     * One sweep, on demand, unlimited — the Settings row's action.
     *
     * Returns the rendered block so the screen can show it (and copy it)
     * *and* writes it to the log, because the two audiences are different
     * people: the operator reads the screen now, and whoever reads the field
     * log later needs it to have been written down.
     */
    suspend fun sweepNow(
        trigger: String = TRIGGER_SETTINGS_ROW,
        context: SweepContext = SweepContext(),
    ): String {
        val sweep = collect(trigger, context)
        val block = ConnectionSweepFormat.format(sweep)
        runCatching { sink(ConnectionSweepFormat.TAG, block) }
        return block
    }

    /**
     * One PERIODIC sweep, rate-limited per [category] — the wizard poll and
     * the sensor auto-detect run.
     *
     * Returns true when a block was written. Returning early on a suppressed
     * tick is not just cheaper, it is the point: a suppressed sweep does not
     * even enumerate USB, so a wizard left open on a bench costs one binder
     * round-trip a second rather than sixty.
     */
    suspend fun logSweep(
        trigger: String,
        category: String = trigger,
        context: SweepContext = SweepContext(),
    ): Boolean {
        if (!enabled) return false
        val admission = limiter.admit(category) ?: return false
        val sweep = collect(trigger, context)
        val block = ConnectionSweepFormat.format(sweep, extraVerdictSuffix = admission.suffix)
        runCatching { sink(ConnectionSweepFormat.TAG, block) }
        return true
    }

    /**
     * A single `[net-debug]` line that is not a whole sweep — the discovery
     * listener opening, a probe ladder starting. Rate-limited on the same
     * per-category windows, using the exact `ArSessionGate` call shape.
     */
    fun logLine(category: String, line: String) {
        if (!enabled) return
        val text = limiter.line(category, line) ?: return
        runCatching { sink(ConnectionSweepFormat.TAG, text) }
    }

    // ── transcription ──────────────────────────────────────────────────────

    /**
     * Every USB device, with its interface descriptors.
     *
     * The interface list is the load-bearing part and the reason this exists
     * beside [UsbDeviceNames] rather than replacing it: `UsbDeviceNames`
     * answers the wizard's question ("is there anything on USB at all"), and
     * this answers the owner's ("is there an Ethernet FUNCTION on USB that
     * never became an interface"), which is only answerable from
     * `getInterface(i).interfaceClass`.
     *
     * Read-only and permission-free throughout: enumerating devices and
     * reading their descriptors needs no `UsbManager` permission grant — only
     * *opening* one does — so this can run on a 1 Hz poll without ever putting
     * a dialog in front of the operator.
     */
    private fun usbDevices(): List<UsbDeviceRecord> {
        val manager = runCatching {
            appContext.getSystemService(Context.USB_SERVICE) as? UsbManager
        }.getOrNull() ?: return emptyList()
        // getDeviceList() throws on a few OEM builds with the USB HAL in a bad
        // state — which is itself one of the failures being diagnosed.
        val devices = runCatching { manager.deviceList.values.toList() }.getOrElse { emptyList() }
        return devices.mapNotNull { device -> runCatching { describe(device) }.getOrNull() }
            .sortedBy { it.deviceName }
    }

    private fun describe(device: UsbDevice): UsbDeviceRecord = UsbDeviceRecord(
        deviceName = device.deviceName,
        vendorId = device.vendorId,
        productId = device.productId,
        // productName/manufacturerName read string descriptors and are null on
        // plenty of real devices — which is exactly why the VID:PID above is
        // not optional.
        productName = runCatching { device.productName }.getOrNull()?.takeIf { it.isNotBlank() },
        manufacturerName = runCatching { device.manufacturerName }.getOrNull()?.takeIf { it.isNotBlank() },
        deviceClass = device.deviceClass,
        deviceSubclass = device.deviceSubclass,
        deviceProtocol = device.deviceProtocol,
        interfaces = (0 until device.interfaceCount).mapNotNull { index ->
            runCatching {
                val iface = device.getInterface(index)
                UsbInterfaceRecord(
                    index = index,
                    interfaceClass = iface.interfaceClass,
                    interfaceSubclass = iface.interfaceSubclass,
                    interfaceProtocol = iface.interfaceProtocol,
                    endpointCount = iface.endpointCount,
                )
            }.getOrNull()
        },
    )

    /**
     * The raw kernel view. Collected in ADDITION to ConnectivityManager's,
     * never instead of it — see the class doc for why the disagreement between
     * the two is itself worth printing.
     */
    private fun networkInterfaces(): List<NetInterfaceRecord> = runCatching {
        NetworkInterface.getNetworkInterfaces()?.toList().orEmpty().map { nic ->
            NetInterfaceRecord(
                name = nic.name,
                // Every one of these can throw SocketException on a
                // half-torn-down interface, which is a state a failing USB
                // adapter spends real time in.
                up = runCatching { nic.isUp }.getOrDefault(false),
                loopback = runCatching { nic.isLoopback }.getOrDefault(false),
                mtu = runCatching { nic.mtu }.getOrDefault(-1),
                virtual = runCatching { nic.isVirtual }.getOrDefault(false),
                addresses = runCatching {
                    nic.interfaceAddresses
                        // IPv4 only, for the same reason EthernetMonitor filters
                        // it: nothing downstream of here can use an IPv6 address
                        // and it would triple the length of every line.
                        .filter { it.address is Inet4Address }
                        .mapNotNull { addr ->
                            addr.address.hostAddress?.let { "$it/${addr.networkPrefixLength}" }
                        }
                }.getOrDefault(emptyList()),
            )
        }.sortedBy { it.name }
    }.getOrDefault(emptyList())

    /** ConnectivityManager's view, copied out of [EthernetMonitor]. No second implementation. */
    private fun ethernetRecord(): ConnectivityEthernetRecord {
        val state = runCatching { ethernetMonitor.state.value }.getOrNull() ?: return ConnectivityEthernetRecord()
        return ConnectivityEthernetRecord(
            present = state.adapterPresent,
            interfaceName = state.interfaceName,
            addresses = state.addresses.map { "${it.ip}/${it.prefixLength}" },
            hasInternet = state.hasInternet,
            usingRequest = state.usingRequest,
            lastEvent = state.lastEvent,
        )
    }

    companion object {
        /** The Mid-360 wizard's ~1 s diagnostic poll. */
        const val TRIGGER_WIZARD_POLL = ConnectionDebugRateLimiter.CATEGORY_WIZARD_POLL

        /** One sensor auto-detect race, from the Capture tab's entry probe. */
        const val TRIGGER_AUTO_DETECT = ConnectionDebugRateLimiter.CATEGORY_AUTO_DETECT

        /** The developer-mode Settings row. Never rate-limited. */
        const val TRIGGER_SETTINGS_ROW = "settings-row"
    }
}
