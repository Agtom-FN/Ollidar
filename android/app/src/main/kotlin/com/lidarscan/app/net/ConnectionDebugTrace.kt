package com.lidarscan.app.net

import com.lidarscan.core.net.DatagramRecord
import com.lidarscan.core.net.DiscoveryRecord
import com.lidarscan.core.net.Mid360HeartbeatParser
import com.lidarscan.core.net.SerialProbeRecord
import java.util.ArrayDeque

/**
 * ROUND 25 item 118, **owner amendment** — the discovery half of the
 * connection sweep: what the app's own listeners and probes have been doing
 * since the last sweep.
 *
 * ## What it records
 *
 * The USB and interface halves of a sweep are *queries* — ask the OS, get an
 * answer. Discovery is not: whether a UDP listener is bound, what datagrams
 * arrived, and what the serial ladder concluded are **events**, and by the
 * time a sweep runs they have already happened. So they have to be captured
 * as they occur, which is what this is.
 *
 * That is also the point the owner's log could not make. `no-ethernet — No
 * Ethernet adapter` twice, two minutes apart, cannot say whether anything was
 * even listening; and "no heartbeat heard" means two completely different
 * things depending on whether a socket was bound at the time.
 *
 * ## Why a process-wide object, honestly
 *
 * The three call sites are constructed in three unrelated places —
 * [UdpMid360Detector] in `AppContainer`, the serial ladder's rungs per
 * auto-detect race in `CaptureScreen`, and the sweeper itself in
 * `AppContainer` — and threading one more constructor parameter through all of
 * them would put a diagnostic dependency into the signature of every detection
 * class in the app. For a **developer-mode-only, disabled-by-default,
 * bounded** recorder that is not a trade worth making, so this is deliberately
 * a singleton and says so rather than pretending otherwise.
 *
 * The consequence is accepted openly: this is per-process, not per-screen, and
 * a sweep drains it. See [snapshotAndReset].
 *
 * ## Bounded, and off unless developer mode is on
 *
 * [enabled] is mirrored from `AppSettings.developerMode` exactly the way
 * `CaptureLog.developerCaptureDebug` is, so every `note*` below is a volatile
 * read and a return for every user who has not tapped the version line seven
 * times. When it is on, the buffers are hard-capped at [MAX_DATAGRAMS] /
 * [MAX_PROBES] and the overflow is COUNTED rather than silently dropped —
 * a diagnostic that quietly loses evidence is worse than no diagnostic.
 *
 * Payloads are never stored. [DatagramRecord.summarise] is the one place a
 * datagram becomes text and it is bounded there; nothing in this file ever
 * holds a `ByteArray`.
 */
object ConnectionDebugTrace {

    /** The most recent datagrams kept between sweeps. A second of a chatty LAN, no more. */
    const val MAX_DATAGRAMS = 16

    /** The serial ladder is two rungs; eight is several races' worth of history. */
    const val MAX_PROBES = 8

    /**
     * Mirrors `AppSettings.developerMode`. Written from the Settings collector
     * that already mirrors the same flag onto `CaptureLog`; read on the
     * discovery socket's thread and on the auto-detect coroutines.
     */
    @Volatile
    var enabled: Boolean = false

    private val lock = Any()
    private val datagrams = ArrayDeque<DatagramRecord>()
    private val probes = ArrayDeque<SerialProbeRecord>()
    private var droppedDatagrams = 0
    private var listening = false
    private var port = Mid360HeartbeatParser.HEARTBEAT_PORT

    /**
     * The UDP discovery listener opened or closed.
     *
     * Sticky — unlike the event buffers, this is *state*, and it survives a
     * sweep: "was anything listening when this sweep ran" has to be answerable
     * on every sweep, not only on the one that happens to follow a bind.
     */
    fun noteListening(listening: Boolean, port: Int = Mid360HeartbeatParser.HEARTBEAT_PORT) {
        if (!enabled) return
        synchronized(lock) {
            this.listening = listening
            this.port = port
        }
    }

    /**
     * A datagram arrived on the discovery port.
     *
     * [payload] is summarised immediately and then forgotten — see
     * [DatagramRecord.summarise]. Non-heartbeat traffic is recorded too, on
     * purpose: something else broadcasting on 56201 is a real failure mode
     * (it is why `UdpMid360Detector` keeps listening past a bad parse) and is
     * otherwise completely invisible.
     */
    fun noteDatagram(sourceIp: String, sourcePort: Int, payload: ByteArray) {
        if (!enabled) return
        val record = DatagramRecord(
            sourceIp = sourceIp,
            sourcePort = sourcePort,
            byteCount = payload.size,
            summary = runCatching { DatagramRecord.summarise(payload) }.getOrDefault("summary failed"),
        )
        synchronized(lock) {
            if (datagrams.size >= MAX_DATAGRAMS) {
                datagrams.removeFirst()
                droppedDatagrams++
            }
            datagrams.addLast(record)
        }
    }

    /**
     * One rung of the serial D6 → STL-27L ladder finished.
     *
     * Recorded per rung and in order, because the order is half the diagnosis:
     * an `unusable` on rung 1 stops the ladder, so the ABSENCE of a rung-2
     * line means "never ran" and not "declined" — and nobody can tell those
     * apart from a log that only reports the final answer.
     */
    fun noteSerialProbe(sensor: String, devicePath: String, outcome: String, detail: String = "") {
        if (!enabled) return
        val record = SerialProbeRecord(
            sensor = sensor,
            devicePath = devicePath,
            outcome = outcome,
            detail = detail,
        )
        synchronized(lock) {
            if (probes.size >= MAX_PROBES) probes.removeFirst()
            probes.addLast(record)
        }
    }

    /**
     * Everything since the last sweep, and then empty.
     *
     * **Draining** rather than peeking: the periodic sweep runs once a second
     * while the wizard is open, and a buffer that is not drained would repeat
     * the same sixteen datagrams into the capture log sixty times a minute —
     * the exact bloat the rate limiter exists to prevent, reintroduced one
     * level down. Each sweep therefore reports the traffic in ITS interval,
     * which is also the more useful reading.
     *
     * [listening] and [port] are state and are not drained.
     */
    fun snapshotAndReset(): DiscoveryRecord = synchronized(lock) {
        val record = DiscoveryRecord(
            listening = listening,
            port = port,
            datagrams = datagrams.toList(),
            probes = probes.toList(),
            datagramsDropped = droppedDatagrams,
        )
        datagrams.clear()
        probes.clear()
        droppedDatagrams = 0
        record
    }

    /** Drops everything, including the sticky listen state. For developer mode being switched off. */
    fun clear() = synchronized(lock) {
        datagrams.clear()
        probes.clear()
        droppedDatagrams = 0
        listening = false
    }
}
