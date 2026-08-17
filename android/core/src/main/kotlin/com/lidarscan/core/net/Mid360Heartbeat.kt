package com.lidarscan.core.net

/**
 * AUTO-DETECT: one decoded Mid-360 heartbeat/push-message beacon.
 *
 * The Mid-360 (once powered and on the wire, whether or not any host has
 * ever configured it) broadcasts a "push message" — SDK2's `cmd_id`
 * `0x0102` (`kCommandIDLidarPushMsg`) — once a second from its own port
 * 56200 (`kMid360LidarPushMsgPort`) to the LAN broadcast address on port
 * **56201** (`kMid360HostPushMsgPort`). It is receive-only and needs no
 * handshake, which is what makes it usable *before* the wizard's mandatory
 * lidar-IP/host-IP fields are filled in — see [Mid360HeartbeatParser].
 *
 * @param serialNumber the device serial (e.g. `MCP7K0034759`).
 * @param deviceType the `DevType:` value, expected `"Mid-360"`.
 * @param firmwareVersion the `FmVer:` value (e.g. `35010108`).
 * @param lidarIp the device's own current IPv4 address — this is what
 *   should be typed into the wizard's "Lidar IP" field.
 * @param lidarNetmask the device's netmask (factory default `255.255.255.0`).
 * @param lidarGateway the device's configured gateway.
 * @param persistedHostIp **the host IP the device is already configured to
 *   stream point/IMU data to** — i.e. whatever host address was last pushed
 *   into it (factory default `192.168.1.5`, Tech Spec §3.1). This is the
 *   "beacon's persisted host" the connect wizard compares against the
 *   phone's own Ethernet address: if they match, the existing
 *   configuration already works; if not, static-IP guidance is shown for
 *   *this exact value*, not a generic default.
 * @param persistedHostPointPort the point-data host port from the same
 *   record (normally `56301`).
 * @param persistedHostImuPort the IMU host port, when present (normally
 *   `56401`).
 */
data class Mid360Heartbeat(
    val serialNumber: String,
    val deviceType: String,
    val firmwareVersion: String,
    val lidarIp: String,
    val lidarNetmask: String,
    val lidarGateway: String,
    val persistedHostIp: String,
    val persistedHostPointPort: Int,
    val persistedHostImuPort: Int?,
)

/**
 * Parses the Mid-360's UDP push-message beacon on port 56201.
 *
 * ## Wire shape (as observed on real hardware)
 *
 * The frame is SDK2's general command envelope
 * (`sdk_core/comm/sdk_protocol.h`'s `SdkPacket` / the public mirror
 * `LivoxLidarCmdPacket` in `include/livox_lidar_def.h`), `#pragma pack(1)`:
 *
 * ```
 * u8  sof            0xAA
 * u8  version
 * u16 length         LE, total frame length (== the UDP payload's own size)
 * u32 seq_num
 * u16 cmd_id         LE, 0x0102 for a push message (kCommandIDLidarPushMsg)
 * u8  cmd_type
 * u8  sender_type
 * u8  rsvd[6]
 * u16 crc16          over bytes [0..17]
 * u32 crc32          over data[]
 * ...data[]...       TLV-encoded key/value pairs (LivoxLidarKeyValueParam):
 *                       u16 key, u16 length, then `length` bytes of value
 * ```
 * That fixed envelope is exactly 24 bytes (offsets 0..23), confirmed both by
 * struct layout and by `length`/`cmd_id` matching a real captured frame
 * byte for byte (`captures/mid360_real_30s.livoxdump`, port 56201).
 *
 * The **first TLV tag does not start at offset 24 (or at 24+4 for a
 * documented `key_num`/reserved push-message prefix) on real hardware** —
 * it starts at **38** on every one of the real captured records checked.
 * Rather than hard-code that (a firmware/SDK revision could shift it by a
 * few bytes again, and getting it wrong silently misreads the whole
 * message), this parser *locates* the TLV list by scanning a bounded
 * window for the one TLV entry that is cheap to recognise unambiguously:
 * `key = 0x0004` (`kKeyLidarIpCfg`) with `length = 12` (IPv4 + netmask +
 * IPv4 gateway, i.e. **the lidar's own IP immediately followed by the
 * `FF FF FF 00` netmask** the format is documented by). Once that anchor is
 * found, the rest of the TLV list is walked normally (`key,length,value`
 * repeating) to pick up every other field this class needs.
 *
 * Known keys used here (`ParamKeyName`, `include/livox_lidar_def.h`):
 *  - `0x0004` `kKeyLidarIpCfg`              → [Mid360Heartbeat.lidarIp]/netmask/gateway
 *  - `0x0006` `kKeyLidarPointDataHostIpCfg` → [Mid360Heartbeat.persistedHostIp] + point port
 *  - `0x0007` `kKeyLidarImuHostIpCfg`       → IMU host port
 *  - `0x8000` `kKeySn`                      → serial number, prefixed with a
 *    two-letter code on real hardware (`"AR" + serial`, e.g.
 *    `"ARMCP7K0034759"` decodes to serial `MCP7K0034759`) — stripped by
 *    [SN_PREFIX_REGEX].
 *  - `0x8001` `kKeyProductInfo`             → the printable
 *    `"DevType:Mid-360 FmType:App FmVer:35010108 BuildTime:..."` string,
 *    which is both the device-type confirmation and where
 *    [Mid360Heartbeat.firmwareVersion] comes from.
 */
object Mid360HeartbeatParser {

    /** `kMid360HostPushMsgPort` — the host-side port a Mid-360 broadcasts its heartbeat to. */
    const val HEARTBEAT_PORT = 56201

    private const val SOF: Byte = 0xAA.toByte()
    private const val CMD_ID_PUSH_MSG = 0x0102

    private const val TAG_LIDAR_IP_CFG = 0x0004
    private const val TAG_HOST_POINT_IP_CFG = 0x0006
    private const val TAG_HOST_IMU_IP_CFG = 0x0007
    private const val TAG_SERIAL_NUMBER = 0x8000
    private const val TAG_PRODUCT_INFO = 0x8001

    private const val ANCHOR_SEARCH_START = 20
    private const val ANCHOR_SEARCH_END = 96

    private val DEV_TYPE_REGEX = Regex("DevType:(\\S+)")
    private val FM_VER_REGEX = Regex("FmVer:(\\S+)")
    private val SN_PREFIX_REGEX = Regex("^[A-Za-z]{2}([A-Za-z0-9]{6,})$")

    /** Returns the decoded heartbeat, or null if [payload] is not a recognisable Mid-360 push message. */
    fun parse(payload: ByteArray): Mid360Heartbeat? {
        if (payload.size < ANCHOR_SEARCH_END) return null
        if (payload[0] != SOF) return null
        if (u16le(payload, 2) != payload.size) return null // `length` self-check
        if (u16le(payload, 8) != CMD_ID_PUSH_MSG) return null

        val tlvStart = findLidarIpTag(payload) ?: return null

        var lidarIp: String? = null
        var lidarNetmask: String? = null
        var lidarGateway: String? = null
        var hostIp: String? = null
        var hostPointPort: Int? = null
        var hostImuPort: Int? = null
        var serial: String? = null
        var deviceType: String? = null
        var firmwareVersion: String? = null

        var offset = tlvStart
        while (offset + 4 <= payload.size) {
            val tag = u16le(payload, offset)
            val len = u16le(payload, offset + 2)
            val valueStart = offset + 4
            if (len < 0 || valueStart + len > payload.size) break

            when (tag) {
                TAG_LIDAR_IP_CFG -> if (len >= 12) {
                    lidarIp = ipv4String(payload, valueStart)
                    lidarNetmask = ipv4String(payload, valueStart + 4)
                    lidarGateway = ipv4String(payload, valueStart + 8)
                }

                TAG_HOST_POINT_IP_CFG -> if (len >= 6) {
                    hostIp = ipv4String(payload, valueStart)
                    hostPointPort = u16le(payload, valueStart + 4)
                }

                TAG_HOST_IMU_IP_CFG -> if (len >= 6) {
                    if (hostIp == null) hostIp = ipv4String(payload, valueStart)
                    hostImuPort = u16le(payload, valueStart + 4)
                }

                TAG_SERIAL_NUMBER -> {
                    val text = asciiTrimmed(payload, valueStart, len)
                    serial = SN_PREFIX_REGEX.find(text)?.groupValues?.get(1) ?: text
                }

                TAG_PRODUCT_INFO -> {
                    val text = asciiTrimmed(payload, valueStart, len)
                    deviceType = DEV_TYPE_REGEX.find(text)?.groupValues?.get(1)
                    firmwareVersion = FM_VER_REGEX.find(text)?.groupValues?.get(1)
                }
            }

            offset = valueStart + len
        }

        if (deviceType != "Mid-360") return null
        val sn = serial?.takeIf { it.isNotBlank() } ?: return null
        val ip = lidarIp ?: return null
        val host = hostIp ?: return null

        return Mid360Heartbeat(
            serialNumber = sn,
            deviceType = deviceType,
            firmwareVersion = firmwareVersion.orEmpty(),
            lidarIp = ip,
            lidarNetmask = lidarNetmask.orEmpty(),
            lidarGateway = lidarGateway.orEmpty(),
            persistedHostIp = host,
            persistedHostPointPort = hostPointPort ?: 0,
            persistedHostImuPort = hostImuPort,
        )
    }

    /** Scans a bounded window for `key=0x0004, length=12` — see the class doc's "Wire shape" note. */
    private fun findLidarIpTag(payload: ByteArray): Int? {
        val end = minOf(ANCHOR_SEARCH_END, payload.size - 4 - 12)
        var offset = ANCHOR_SEARCH_START
        while (offset <= end) {
            if (u16le(payload, offset) == TAG_LIDAR_IP_CFG && u16le(payload, offset + 2) == 12) {
                return offset
            }
            offset++
        }
        return null
    }

    private fun u16le(b: ByteArray, offset: Int): Int =
        (b[offset].toInt() and 0xFF) or ((b[offset + 1].toInt() and 0xFF) shl 8)

    private fun ipv4String(b: ByteArray, offset: Int): String =
        "${b[offset].toInt() and 0xFF}.${b[offset + 1].toInt() and 0xFF}." +
            "${b[offset + 2].toInt() and 0xFF}.${b[offset + 3].toInt() and 0xFF}"

    private fun asciiTrimmed(b: ByteArray, offset: Int, length: Int): String =
        String(b, offset, length, Charsets.US_ASCII).trimEnd(' ').trim()
}

/** Applies a detected heartbeat's addresses onto a settings record — pure so it is trivially unit-testable. */
fun Mid360Settings.withDetectedHeartbeat(heartbeat: Mid360Heartbeat): Mid360Settings =
    copy(lidarIp = heartbeat.lidarIp, hostIp = heartbeat.persistedHostIp)

/** One outcome of [Mid360Detector.detect]. */
sealed interface Mid360DetectionResult {
    data class Found(val heartbeat: Mid360Heartbeat) : Mid360DetectionResult
    data object TimedOut : Mid360DetectionResult
    data class Error(val message: String) : Mid360DetectionResult
}

/**
 * Listens for a Mid-360 heartbeat. Kept as a plain-Kotlin seam (no Android
 * dependency) so [Mid360AutoDetectController] is JVM-testable against a
 * fake — the real implementation
 * (`com.lidarscan.app.net.UdpMid360Detector`, `:app`) is a plain
 * `DatagramSocket` bound to `0.0.0.0:`[Mid360HeartbeatParser.HEARTBEAT_PORT].
 */
interface Mid360Detector {
    /**
     * Listens for up to [timeoutMs], calling [onElapsedMs] periodically so a
     * caller can drive a progress indicator, and returns as soon as a valid
     * heartbeat is parsed (does not necessarily wait out the full timeout).
     */
    suspend fun detect(timeoutMs: Long, onElapsedMs: (Long) -> Unit = {}): Mid360DetectionResult
}
