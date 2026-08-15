package com.lidarscan.app.net

import android.net.Network
import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import java.io.FileDescriptor
import java.net.InetAddress

/**
 * A UDP socket created in Kotlin, bound to a specific address/port, bound to
 * the Ethernet [Network] via [Network.bindSocket], and then handed to the
 * engine as a raw descriptor.
 *
 * This is the concrete implementation of the seam
 * `engine/include/scanengine/transport/udp_source.h` describes in its own
 * header comment:
 *
 * > The Android seam: the app binds the socket to the USB-Ethernet Network
 * > object (ConnectivityManager TRANSPORT_ETHERNET + Network.bindSocket) and
 * > hands the bound descriptor down, because the engine cannot reach
 * > ConnectivityManager. That is `prebound_fd`.
 *
 * ## Why `android.system.Os` and not `DatagramSocket`
 *
 * `Network.bindSocket` has three overloads — `DatagramSocket`, `Socket`, and
 * `FileDescriptor` — so a `DatagramSocket` would bind fine. The problem is
 * getting an **int** out of it afterwards: `DatagramSocket` exposes no
 * descriptor accessor, and the usual route is reflection into
 * `DatagramSocketImpl`, which is on the hidden-API greylist and has moved
 * between releases. `Os.socket()` returns a `FileDescriptor` directly, and
 * `ParcelFileDescriptor.dup(FileDescriptor).detachFd()` is a documented,
 * public way to turn one into an owned int. No reflection anywhere.
 *
 * ## Descriptor ownership, stated exactly
 *
 * Three descriptors are involved and confusing them leaks or double-closes:
 *
 *  1. **[fd]** — the original, created by `Os.socket`, bound, and bound to
 *     the `Network`. This object owns it and closes it in [close].
 *  2. **The dup returned by [detachDupForNative]** — a *separate* descriptor
 *     onto the same socket, produced by `ParcelFileDescriptor.dup(fd)` and
 *     detached so no `ParcelFileDescriptor` finalizer will close it. It is
 *     handed to native and **native owns it**: `UdpSource` never closes a
 *     pre-bound fd ("the app owns it", udp_source.h), so
 *     `Mid360Probe::stop()` closes it explicitly.
 *  3. Anything else — there is nothing else. If a fourth descriptor appears,
 *     something has gone wrong.
 *
 * A dup rather than handing over [fd] itself because the two lifetimes are
 * genuinely independent: the wizard can be dismissed (closing this object)
 * while a probe is mid-teardown on the native side, and a descriptor closed
 * out from under a blocking `recvfrom` is how you get a receive thread
 * spinning on EBADF.
 *
 * ## Binding to the Network, and what it does *not* cover
 *
 * `Network.bindSocket` is per-socket: it marks this one descriptor so its
 * traffic uses the Ethernet interface regardless of the process's default
 * network. That is exactly right for sockets we create — and it is why this
 * class exists rather than `bindProcessToNetwork`, which would push NTRIP
 * corrections and everything else onto a link with no internet.
 *
 * It cannot cover sockets the **Livox SDK2** creates inside native code
 * (`util::CreateSocket` in the vendored SDK), which is the production
 * bring-up path. See android/NOTES.md's B3 section for that gap and the
 * three ways out of it.
 */
class NetworkBoundUdpSocket private constructor(
    val fd: FileDescriptor,
    val boundAddress: String,
    val boundPort: Int,
    val networkBound: Boolean,
) : AutoCloseable {

    private var detached = false
    private var closed = false

    /**
     * Duplicates this socket's descriptor and detaches it, giving the caller
     * an int the **native side** must close. See the ownership note above.
     * Returns -1 if the dup fails.
     */
    fun detachDupForNative(): Int = try {
        val pfd = ParcelFileDescriptor.dup(fd)
        detached = true
        pfd.detachFd()
    } catch (e: Exception) {
        -1
    }

    /** True once [detachDupForNative] has handed a descriptor to native code. */
    val handedToNative: Boolean get() = detached

    override fun close() {
        if (closed) return
        closed = true
        runCatching { Os.close(fd) }
    }

    companion object {
        /**
         * Creates, binds and network-binds one UDP socket.
         *
         * @param network the Ethernet network from [EthernetMonitor]. Null is
         *   allowed and produces an unbound-to-network socket — useful for
         *   telling "the socket cannot even be created" apart from "the
         *   network binding is what failed", which are different problems
         *   with different fixes.
         * @param bindAddress the local address to bind, normally the host IP
         *   the wizard configured. Binding the specific address rather than
         *   `INADDR_ANY` is deliberate: it fails loudly and immediately if
         *   the address is not actually on this device, which is the single
         *   most common Mid-360 misconfiguration and otherwise shows up 8
         *   seconds later as "no packet".
         * @param recvBufferBytes SO_RCVBUF request. Defaults to `UdpConfig`'s
         *   4 MB (~1.4 s of slack at the Mid-360's ~23 Mbit/s), because a
         *   pre-bound socket bypasses the engine's own sizing — `UdpSource`
         *   only sets SO_RCVBUF on a socket it created itself. Getting this
         *   wrong is invisible until a scheduling hiccup drops packets.
         */
        fun open(
            network: Network?,
            bindAddress: String,
            port: Int,
            recvBufferBytes: Int = 4 * 1024 * 1024,
        ): Result<NetworkBoundUdpSocket> {
            var fd: FileDescriptor? = null
            return try {
                val created = Os.socket(OsConstants.AF_INET, OsConstants.SOCK_DGRAM, OsConstants.IPPROTO_UDP)
                fd = created
                Os.setsockoptInt(created, OsConstants.SOL_SOCKET, OsConstants.SO_REUSEADDR, 1)
                Os.setsockoptInt(created, OsConstants.SOL_SOCKET, OsConstants.SO_RCVBUF, recvBufferBytes)
                Os.bind(created, InetAddress.getByName(bindAddress), port)

                var bound = false
                if (network != null) {
                    // Throws IOException when the network has gone away
                    // between the callback and here — a real race with a
                    // cable being pulled, not a theoretical one.
                    network.bindSocket(created)
                    bound = true
                }
                Result.success(NetworkBoundUdpSocket(created, bindAddress, port, bound))
            } catch (e: ErrnoException) {
                fd?.let { runCatching { Os.close(it) } }
                Result.failure(IllegalStateException(explainErrno(e, bindAddress, port), e))
            } catch (e: Exception) {
                fd?.let { runCatching { Os.close(it) } }
                Result.failure(e)
            }
        }

        /**
         * Turns an errno into the sentence that names the actual fix. Worth
         * the code: `EACCES` from `socket(2)` in particular is an Android-only
         * failure that looks like nothing else — the kernel refuses the call
         * because the app is not in the `inet` group, which it joins only by
         * holding `android.permission.INTERNET`. Reported as a bare "EACCES"
         * it looks like a firewall problem and is not.
         */
        private fun explainErrno(e: ErrnoException, address: String, port: Int): String = when (e.errno) {
            OsConstants.EACCES ->
                "Permission denied creating a UDP socket. On Android this means the app is missing " +
                    "android.permission.INTERNET — sockets are gated on the `inet` group, not on a firewall."
            OsConstants.EADDRNOTAVAIL ->
                "$address is not an address this device holds. Configure the Ethernet interface with " +
                    "that static IP first (or use the address the interface already has)."
            OsConstants.EADDRINUSE ->
                "Port $port is already bound. Another Mid-360 session — or another app — is using it; " +
                    "stop the running self-test before starting another."
            else -> "socket/bind on $address:$port failed: ${e.message}"
        }
    }
}
