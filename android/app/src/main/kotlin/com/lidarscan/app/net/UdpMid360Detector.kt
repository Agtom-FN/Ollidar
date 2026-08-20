package com.lidarscan.app.net

import android.net.Network
import com.lidarscan.core.net.Mid360Detector
import com.lidarscan.core.net.Mid360DetectionResult
import com.lidarscan.core.net.Mid360HeartbeatParser
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.SocketException
import java.net.SocketTimeoutException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext

/**
 * AUTO-DETECT: the real [Mid360Detector] — a plain `DatagramSocket` bound
 * `0.0.0.0:56201`, listening for the Mid-360's own heartbeat broadcast (see
 * [Mid360HeartbeatParser]'s class doc for the wire format).
 *
 * Deliberately **not** [NetworkBoundUdpSocket] (the `Os.socket`/raw-fd class
 * the pre-capture self-test's pre-bound-socket path uses): that class exists
 * to hand a descriptor to *native* code, which auto-detect never does — this
 * only ever calls [DatagramSocket.receive] from Kotlin, so the plain JDK
 * socket API is the right tool, per the owner brief ("a plain DatagramSocket
 * bound 0.0.0.0:56201").
 *
 * [network] is a supplier rather than a fixed value for the same reason
 * [com.lidarscan.core.net.Mid360AutoDetectController.start] takes one: the
 * USB-C Ethernet adapter can attach (and `EthernetMonitor` pick up the
 * `Network`) after this detector is constructed, and "no Network yet" must
 * not permanently disable auto-detect — it degrades to an unbound-to-network
 * socket, which still works whenever this phone has only one plausible route
 * to the lidar (the common case: no other active network shares the
 * Mid-360's private subnet).
 */
class UdpMid360Detector(private val network: () -> Network?) : Mid360Detector {

    override suspend fun detect(timeoutMs: Long, onElapsedMs: (Long) -> Unit): Mid360DetectionResult =
        withContext(Dispatchers.IO) {
            var socket: DatagramSocket? = null
            try {
                socket = DatagramSocket(null).apply {
                    reuseAddress = true
                    bind(InetSocketAddress("0.0.0.0", Mid360HeartbeatParser.HEARTBEAT_PORT))
                    broadcast = true
                    soTimeout = POLL_TIMEOUT_MS
                }
                runCatching { network()?.bindSocket(socket) }
                // ROUND 25 item 118 (owner amendment): "no heartbeat heard"
                // means two completely different things depending on whether
                // anything was listening, and the owner's log could not say
                // which. A no-op unless developer mode is on.
                ConnectionDebugTrace.noteListening(true, Mid360HeartbeatParser.HEARTBEAT_PORT)

                val buffer = ByteArray(MAX_DATAGRAM_BYTES)
                val packet = DatagramPacket(buffer, buffer.size)
                val startMs = System.currentTimeMillis()

                while (currentCoroutineContext().isActive) {
                    val elapsed = System.currentTimeMillis() - startMs
                    if (elapsed >= timeoutMs) return@withContext Mid360DetectionResult.TimedOut
                    onElapsedMs(elapsed)

                    try {
                        socket.receive(packet)
                    } catch (e: SocketTimeoutException) {
                        continue
                    }

                    val payload = packet.data.copyOfRange(packet.offset, packet.offset + packet.length)
                    // Item 118 amendment: recorded BEFORE the parse and
                    // whatever the parse says, because "something else is
                    // broadcasting on 56201" is a real failure mode — it is
                    // why the loop below keeps listening past a bad parse —
                    // and is otherwise completely invisible. The payload is
                    // summarised and forgotten; it is never stored or logged.
                    ConnectionDebugTrace.noteDatagram(
                        sourceIp = packet.address?.hostAddress ?: "?",
                        sourcePort = packet.port,
                        payload = payload,
                    )
                    val heartbeat = Mid360HeartbeatParser.parse(payload)
                    if (heartbeat != null) {
                        return@withContext Mid360DetectionResult.Found(heartbeat)
                    }
                    // Not a Mid-360 heartbeat (some other broadcast on this
                    // port, or a corrupt datagram) — keep listening rather
                    // than failing on the first non-matching packet.
                }
                Mid360DetectionResult.TimedOut
            } catch (e: SocketException) {
                Mid360DetectionResult.Error(explainSocketException(e))
            } catch (e: SecurityException) {
                Mid360DetectionResult.Error(
                    "Permission denied opening a UDP socket on port ${Mid360HeartbeatParser.HEARTBEAT_PORT}.",
                )
            } finally {
                socket?.close()
                ConnectionDebugTrace.noteListening(false, Mid360HeartbeatParser.HEARTBEAT_PORT)
            }
        }

    private fun explainSocketException(e: SocketException): String {
        val message = e.message.orEmpty()
        return when {
            message.contains("EADDRINUSE", ignoreCase = true) || message.contains("already in use", ignoreCase = true) ->
                "Port ${Mid360HeartbeatParser.HEARTBEAT_PORT} is already bound — another auto-detect listener " +
                    "(or another app) is using it. Stop it and try again, or enter the addresses manually."
            else -> "Could not listen for a Mid-360 heartbeat: ${message.ifBlank { e.javaClass.simpleName }}"
        }
    }

    private companion object {
        /** How long a single `receive()` blocks before re-checking the overall deadline/cancellation. */
        const val POLL_TIMEOUT_MS = 500
        /** Comfortably above the largest observed heartbeat frame (430 B) with headroom for other broadcast noise. */
        const val MAX_DATAGRAM_BYTES = 4096
    }
}
