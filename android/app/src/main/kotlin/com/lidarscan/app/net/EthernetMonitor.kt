package com.lidarscan.app.net

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import com.lidarscan.core.net.LocalAddress
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.Inet4Address

/**
 * Watches for a USB-C Ethernet adapter (Tech Spec §3.1's Android row:
 * "USB-C Ethernet; `ConnectivityManager` `TRANSPORT_ETHERNET` +
 * `Network.bindSocket`").
 *
 * ## Why `requestNetwork` **and** `registerNetworkCallback`
 *
 * The brief (and the spec) name `requestNetwork(TRANSPORT_ETHERNET)`, which
 * is the right API when you want the system to bring a network up and keep it
 * up for you. But `ConnectivityManager.requestNetwork` is guarded by
 * `android.permission.CHANGE_NETWORK_STATE`, whose protection level is
 * `signature|preinstalled|appop|pre23` — a normal app does **not** get it by
 * declaring it, and on many builds the call throws `SecurityException` (or
 * fails a `Settings.System.canWrite()` check) at runtime.
 * `registerNetworkCallback` with the same `NetworkRequest` needs only
 * `ACCESS_NETWORK_STATE`, is granted at install, and delivers exactly the
 * same callbacks — it just does not *ask for* the network, which for a
 * physically-attached USB-Ethernet adapter is not something that needs
 * asking anyway.
 *
 * So this class tries `requestNetwork` first (so a privileged/OEM build gets
 * the stronger guarantee, and so the system knows Ethernet is wanted) and
 * falls back to `registerNetworkCallback` on `SecurityException`. Which path
 * is live is exposed in [state] as [EthernetState.usingRequest], because it
 * changes what the user should be told when nothing appears: with a plain
 * callback registration, "no Ethernet network" genuinely means the OS has not
 * brought one up, and the fix is in Settings, not in this app.
 *
 * ## What it does NOT do
 *
 * It does not call `bindProcessToNetwork`. That is process-wide and would
 * push *every* socket the app opens — NTRIP corrections (A10/B9), Play
 * services, ARCore's own telemetry — onto an Ethernet link with no route to
 * the internet. Per-socket binding via [Network.bindSocket] is the correct
 * tool and is what [NetworkBoundUdpSocket] uses. The one place that is not
 * sufficient is the SDK2 backend, which creates its own sockets inside native
 * code we do not own; see android/NOTES.md's B3 section for exactly what that
 * costs and what the options are.
 */
class EthernetMonitor(context: Context) {

    private val appContext = context.applicationContext
    private val cm = appContext.getSystemService(ConnectivityManager::class.java)

    private val _state = MutableStateFlow(EthernetState())
    val state: StateFlow<EthernetState> = _state.asStateFlow()

    private var callback: ConnectivityManager.NetworkCallback? = null

    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
            update(network)
        }

        override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) {
            update(network, linkProperties)
        }

        override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) {
            update(network)
        }

        override fun onLost(network: Network) {
            if (_state.value.network == network) {
                _state.value = _state.value.copy(
                    network = null,
                    interfaceName = null,
                    addresses = emptyList(),
                    lastEvent = "Ethernet network lost",
                )
            }
        }
    }

    /** Idempotent. Safe to call from `onStart`; pair with [stop]. */
    fun start() {
        if (cm == null) {
            _state.value = _state.value.copy(
                supported = false,
                lastEvent = "No ConnectivityManager on this device",
            )
            return
        }
        if (callback != null) return

        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
            // NOT_VPN keeps a VPN that happens to advertise an Ethernet-ish
            // transport out of the match. Deliberately no NET_CAPABILITY_INTERNET:
            // a direct phone-to-lidar link has no internet and never will, and
            // requiring it is the single easiest way to make this callback
            // never fire on exactly the setup it exists for.
            .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)
            .build()

        var usingRequest = false
        try {
            cm.requestNetwork(request, networkCallback)
            usingRequest = true
        } catch (e: SecurityException) {
            // Expected on a non-privileged build — see the class doc.
            cm.registerNetworkCallback(request, networkCallback)
        }
        callback = networkCallback

        // Seed from whatever already exists: a callback only reports changes,
        // and the adapter is very often plugged in before this screen opens.
        val existing = cm.allNetworks.firstOrNull { n ->
            cm.getNetworkCapabilities(n)?.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) == true
        }
        _state.value = _state.value.copy(
            supported = true,
            usingRequest = usingRequest,
            lastEvent = if (usingRequest) "Requested an Ethernet network" else "Listening for Ethernet networks",
        )
        if (existing != null) update(existing)
    }

    fun stop() {
        val cb = callback ?: return
        callback = null
        runCatching { cm?.unregisterNetworkCallback(cb) }
    }

    private fun update(network: Network, linkProperties: LinkProperties? = null) {
        val cmLocal = cm ?: return
        val lp = linkProperties ?: cmLocal.getLinkProperties(network)
        val caps = cmLocal.getNetworkCapabilities(network)
        val addresses = lp?.linkAddresses.orEmpty()
            // IPv4 only: `UdpConfig` is dotted-quad throughout and SDK2's
            // config JSON has no IPv6 form, so an IPv6 address here would be
            // offered as a host IP the engine cannot use.
            .filter { it.address is Inet4Address }
            .map { LocalAddress(it.address.hostAddress ?: "", it.prefixLength) }
            .filter { it.ip.isNotEmpty() }

        _state.value = _state.value.copy(
            supported = true,
            network = network,
            interfaceName = lp?.interfaceName,
            addresses = addresses,
            hasInternet = caps?.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) == true,
            lastEvent = "Ethernet network available" +
                (lp?.interfaceName?.let { " on $it" } ?: "") +
                if (addresses.isEmpty()) " — no IPv4 address yet (DHCP pending, or static IP not configured)" else "",
        )
    }
}

/**
 * @param addresses IPv4 addresses currently on the Ethernet interface. Empty
 *   with a non-null [network] is the interesting case: the adapter is
 *   attached and the OS has a network for it, but no address has been
 *   assigned — which is precisely the state a direct lidar link sits in until
 *   the user configures a static IP, because there is no DHCP server on the
 *   other end of the cable.
 */
data class EthernetState(
    val supported: Boolean = true,
    val usingRequest: Boolean = false,
    val network: Network? = null,
    val interfaceName: String? = null,
    val addresses: List<LocalAddress> = emptyList(),
    val hasInternet: Boolean = false,
    val lastEvent: String = "Not started",
) {
    val adapterPresent: Boolean get() = network != null
    val hasAddress: Boolean get() = addresses.isNotEmpty()

    /**
     * The address to pre-fill the host-IP field with, when the interface
     * already has one. Preferring the interface's own address over the
     * `192.168.1.5` default is the difference between a wizard that works
     * first time and one that silently streams into the void.
     */
    val suggestedHostIp: String? get() = addresses.firstOrNull()?.ip

    val apiLevelNote: String
        get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            "Android ${Build.VERSION.RELEASE}"
        } else {
            "Android ${Build.VERSION.RELEASE} — Ethernet settings are less consistent before Android 11"
        }
}
