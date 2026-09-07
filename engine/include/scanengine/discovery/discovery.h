// discovery.h — A16: find the hardware instead of asking the operator to type
// its IP, its port and its /dev path.
//
// WHY THIS EXISTS (owner requirement, first real-hardware session)
//   The 2026-08-17 field session (captures/FIELD_SESSION_2026-08-17.md) proved
//   every driver in this engine against real devices and, in doing so, proved
//   the SETUP story is the weak link: the Mid-360 needed a hand-typed IP, a
//   hand-added host route and a host alias matching a number persisted INSIDE
//   the lidar; the D6 and the UM982 needed the right /dev/cu.* guessed from a
//   list of four; and the UM982 turned out to be at 230400 rather than its
//   documented 115200 default. A GUI that makes the operator supply all of
//   that is not a GUI. This module is the engine half of the fix.
//
// THE ONE RULE, inherited from tools/fieldtest-kit: identify a device by its
// PROTOCOL, never by its name. /dev/cu.usbserial-21130 and
// /dev/cu.usbserial-21140 differ by one character and carry different devices;
// /dev/cu.usbmodem2111101 on the same Mac was an unrelated ESP32. A name-based
// guess would have opened the wrong one. A wire-signature probe cannot.
//
// THREADING: every function here is synchronous and blocking for at most the
// timeout the caller passes. None of them touch an Engine, none of them own a
// thread, and none of them are safe to call from a UI thread without a worker.
// They are otherwise safe to call from any thread and from several at once,
// except that two concurrent DiscoverMid360() calls contend for the same UDP
// port (the second gets kBusy unless SO_REUSEPORT is available).
//
// PRIVILEGE: nothing here needs root. Binding 56200/56201 is unprivileged;
// opening a /dev/cu.* needs the usual dialout/serial group membership, and a
// port we cannot open is SKIPPED, never an error (docs/A16-discovery.md §5).
//
// Owner: A16.
#ifndef SCANENGINE_DISCOVERY_DISCOVERY_H
#define SCANENGINE_DISCOVERY_DISCOVERY_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "scanengine/core/error.h"

namespace scanengine {
namespace discovery {

// The owner's task fixed these entry-point names in PascalCase. The rest of
// the engine spells free functions snake_case; rather than have two
// conventions inside one header, EVERY public entry point in this module is
// PascalCase and every struct field stays snake_case like the rest of the
// engine. This is the module boundary of the deviation — nothing outside
// namespace discovery adopts it.

// ===========================================================================
// Mid-360 beacon discovery
// ===========================================================================
//
// A Mid-360 broadcasts a ~430-byte SDK2 control frame to
// 255.255.255.255:56201 at 1 Hz whether or not anyone has ever configured it.
// That frame carries, in one datagram, everything the setup wizard has to
// know: the lidar's own IP/netmask/gateway, the serial number, the firmware
// string, and — the field session's actual failure — the HOST IP the lidar has
// persisted and will stream to.
//
// Two field facts shape the socket code:
//   1. A broadcast is delivered only to sockets bound to INADDR_ANY on
//      macOS/BSD. Binding the interface address (the obvious thing, and what
//      the driver does for the point stream) receives NOTHING. So discovery
//      binds 0.0.0.0 — see docs/A16-discovery.md §2.
//   2. The lidar ignores ICMP. There is no ping-sweep alternative; the
//      heartbeat is the only passive way to find one.

inline constexpr std::uint16_t kMid360PushPort = 56201;      // observed
inline constexpr std::uint16_t kMid360PushPortAlt = 56200;   // the "push_port" itself
inline constexpr std::size_t kMid360BeaconMinBytes = 24 + 4; // header + key count

// One discovered lidar. Strings are dotted-quad IPv4; empty means "the
// heartbeat did not carry it", never "0.0.0.0".
struct Mid360Beacon {
  std::string sn;               // key 0x8000, e.g. "ARMCP7K0034759"
  std::string dev_type;         // "Mid-360", from the DevType: anchor
  std::string fw_version;       // "35.1.1.8" — key 0x8002, dotted
  std::string fw_version_text;  // "35010108" — the FmVer: field, verbatim
  std::string fw_type;          // "App" or "Loader", from FmType:
  std::string build_time;       // "2025/06/09", from BuildTime:
  std::string product_info;     // the whole "DevType:... BuildTime:..." string
  std::string mac;              // key 0x8005, "ec:72:f7:89:13:5f"

  // Key 0x0004 — the lidar's own L3 configuration.
  std::string lidar_ip;
  std::string netmask;
  std::string gateway;

  // Keys 0x0006 / 0x0007 — the PERSISTED host. This is the field failure in
  // one field: the lidar will stream to `persisted_host_ip` and nowhere else
  // until an SDK2 config push changes it, so a host that does not HOLD that
  // address gets a silent zero-packet session.
  std::string persisted_host_ip;
  std::string persisted_imu_host_ip;
  std::uint16_t persisted_point_port = 0;  // 56301 in the field capture
  std::uint16_t persisted_imu_port = 0;    // 56401

  // Where the datagram came from and where we heard it.
  std::string source_ip;                 // the sender's L3 address
  std::uint16_t push_port_seen = 0;      // the local port it arrived on
  std::int64_t t_last_seen_ns = 0;       // SteadyClock, last heartbeat
  std::uint32_t beacons_seen = 0;        // heartbeats merged into this record

  std::uint32_t key_count = 0;  // key-value pairs the frame declared
  bool crc_ok = false;          // header CRC16 AND payload CRC32 verified
  bool heuristic = false;       // parsed by the fallback scan, not the KV walk

  // Stable, human-facing one-liner for a log or a picker row.
  std::string describe() const;
};

struct DiscoverOptions {
  int timeout_ms = 3000;  // >= 2000 recommended: the beacon is 1 Hz

  // Empty means {56201, 56200}. A port already held by Livox Viewer or by a
  // second LidarScan is skipped with a warning, not an error — as long as at
  // least ONE of the requested ports bound, discovery proceeds.
  std::vector<std::uint16_t> ports;

  // Return as soon as this many DISTINCT lidars have been seen. 0 = listen
  // for the whole timeout (the right default for a picker: a second lidar
  // that appears at t+1.5 s must show up in the list).
  std::uint32_t stop_after_devices = 0;

  // Drop frames whose CRC16/CRC32 do not verify. Default false: a beacon is
  // advisory, and refusing to show an operator a lidar because one datagram
  // was clipped is worse than showing it with crc_ok=false. Set true for a
  // diagnostic that wants certainty.
  bool require_crc = false;

  // Allow the anchor+IPv4-scan fallback when the key-value walk fails
  // (firmware that reorders, extends or pads the frame). See §3 of the doc.
  bool allow_heuristic = true;

  // Field-test hook (A17): every datagram a listen receives, BEFORE parsing
  // and whether or not it parses, with the sender's dotted IPv4. The desktop
  // app uses it to write a capture_mid70.py-compatible LX70_CAP file from a
  // discovery pass, which is how the first real-hardware broadcast fixture
  // gets home without a second tool on the rig. Called on the discovery
  // thread; must be quick. Honoured by DiscoverMid70; DiscoverMid360 ignores
  // it (its heartbeat has a fixture already).
  void (*raw_sink)(const std::uint8_t* data, std::size_t len, const char* source_ip,
                   std::uint16_t port, void* user) = nullptr;
  void* raw_sink_user = nullptr;
};

// Listen for heartbeats and return one record per DISTINCT lidar, dedup'd by
// serial number (by source IP when a frame carried no SN), newest last-seen
// wins and beacons_seen counts the merges.
//
// Never fails just because nothing answered: an empty vector with kOk means
// "no lidar is broadcasting", which is a legitimate, displayable answer.
// kBusy means no requested port could be bound at all — the single most
// likely cause is Livox Viewer 2 or a second LidarScan still running.
Result<std::vector<Mid360Beacon>> DiscoverMid360(int timeout_ms);
Result<std::vector<Mid360Beacon>> DiscoverMid360(const DiscoverOptions& opt);

// The pure parser behind it — no sockets, and the function the tests aim at
// the real captured payloads. `allow_heuristic` mirrors DiscoverOptions.
// kProtocolError for a frame that is not an SDK2 control frame at all,
// kCorruptData for one that is but whose fields do not survive the walk.
Result<Mid360Beacon> ParseMid360Beacon(const std::uint8_t* data, std::size_t len,
                                       bool allow_heuristic = true);

// CRC16-CCITT-FALSE over the first 18 header bytes and CRC32 (ISO-HDLC, the
// zlib polynomial and conventions) over the payload — both verified against
// the field capture, both exposed because a diagnostic wants to say WHICH
// half failed.
bool Mid360HeaderCrcOk(const std::uint8_t* data, std::size_t len);
bool Mid360PayloadCrcOk(const std::uint8_t* data, std::size_t len);

// ===========================================================================
// Mid-70 broadcast discovery (Livox SDK **v1**)
// ===========================================================================
//
// A Mid-70 is NOT a Mid-360 with a different name. It speaks the older SDK v1
// protocol, and its "I am here" message is not the Mid-360's 430-byte SDK2
// control frame on 56201 but a 50-byte SDK v1 COMMAND frame broadcast to
// 255.255.255.255:55000 roughly once a second for as long as nothing is
// connected to it. (The SDK's own listener is
// sdk_core/src/device_discovery.h: `static const uint16_t kListenPort =
// 55000`.) Two different protocols on two different ports; two parsers.
//
// The frame, from sdk_core/src/comm/sdk_protocol.{h,cpp} of Livox-SDK v1:
//
//   off  size  field
//     0     1  sof            0xAA
//     1     1  version        1 (kSdkVer0 — the enum starts at kSdkVerNone=0)
//     2     2  length         WHOLE frame, LE. 50 for a broadcast.
//     4     1  packet_type    0 Cmd / 1 Ack / 2 Msg; a broadcast is a Msg
//     5     2  seq_num        LE
//     7     2  preamble_crc   LE, CRC16 over bytes 0..6
//     9     1  cmd_set        0x00 = kCommandSetGeneral
//    10     1  cmd_id         0x00 = kCommandIDGeneralBroadcast
//    11    35  BroadcastDeviceInfo (livox_def.h, #pragma pack(1)):
//                 char broadcast_code[16];  // "3GGDJ5N00100101", NUL-padded
//                 uint8_t dev_type;         // DeviceType; 6 = Mid-70
//                 uint16_t reserved;
//                 char ip[16];              // the lidar's own IPv4, as TEXT
//    46     4  crc32          LE, over bytes 0..45 (everything before it)
//
// The two CRCs are NOT the Mid-360's. See mid70_beacon.cpp for the exact
// parameters, which were read out of the SDK's vendored FastCRC rather than
// guessed: both use a Livox-specific INIT value, so a stock CCITT-FALSE or a
// stock zlib CRC32 rejects every real frame.
//
// Everything else here mirrors the Mid-360 half: bind 0.0.0.0 (a broadcast
// reaches only an any-bound socket on macOS/BSD), listen, merge.

inline constexpr std::uint16_t kMid70BroadcastPort = 55000;
// 11-byte header + 19-byte wire payload (code[16] + dev_type + reserved[2]) +
// 4-byte CRC32. NOT 50: sizeof(BroadcastDeviceInfo) is 35 in memory, but the
// SDK's receiver (device_discovery.cpp:148) copies only 19 bytes off the wire
// and fills ip[] from the UDP source address. ParseMid70Beacon accepts both
// forms; only the 35-byte one carries lidar_ip.
inline constexpr std::size_t kMid70BroadcastBytes = 34;
inline constexpr std::size_t kMid70BroadcastBytesWithIp = 50;

// One discovered Mid-70 (or, see dev_type below, one discovered SDK-v1 Livox
// of any model). Strings are trimmed of the wire's NUL padding; empty means
// "the frame did not carry it".
struct Mid70Beacon {
  // The 15-character code printed on the lidar's label and the ONLY name the
  // SDK v1 API accepts in AddLidarToConnect(). "3GGDJ5N00100101".
  std::string broadcast_code;

  // livox_def.h DeviceType, verbatim. 6 is a Mid-70; the parser does NOT
  // reject the others, because a Horizon on the bench that shows up as
  // "Horizon 0TFDG… at 192.168.1.12 (not a Mid-70)" is a diagnosis and a
  // silently dropped datagram is not.
  std::uint8_t dev_type = 0;
  std::string dev_type_name;  // "Mid-70", "Horizon", "unknown (9)"

  // The ip[16] TEXT field — present only in the 35-byte payload form, which
  // the real wire does NOT carry (see kMid70BroadcastBytes). Usually empty;
  // use source_ip, which is where the SDK itself gets the lidar's address.
  std::string lidar_ip;

  // Where the datagram came from and where we heard it — same fields, same
  // meaning, as Mid360Beacon.
  std::string source_ip;
  std::uint16_t push_port_seen = 0;
  std::int64_t t_last_seen_ns = 0;
  std::uint32_t beacons_seen = 0;

  // Stable one-liner for a log or a picker row.
  std::string describe() const;
};

// Listen on UDP 55000 for SDK v1 broadcasts and return one record per
// DISTINCT lidar, dedup'd by broadcast code (by source IP if a frame somehow
// carried none), beacons_seen counting the merges.
//
// Never fails just because nothing answered: an empty vector with kOk means
// "no Mid-70 is broadcasting". kBusy means the port could not be bound at
// all, whose overwhelmingly likely cause is that the Livox SDK — ours, Livox
// Viewer's, or a livox_ros_driver node — already holds it. Note that a
// Mid-70 STOPS broadcasting once something connects to it, so kOk+empty is
// also what "a viewer is already streaming from it" looks like.
Result<std::vector<Mid70Beacon>> DiscoverMid70(int timeout_ms);
// The DiscoverOptions overload. timeout_ms, ports (default {55000}) and
// stop_after_devices apply exactly as they do to DiscoverMid360. The other
// two do not, and are ignored:
//   * `allow_heuristic` — an SDK v1 broadcast is a fixed-layout 50-byte
//     frame, so there is nothing to guess at and no fallback to fall back to.
//   * `require_crc` — it is always on here. The Mid-360's heartbeat is 430
//     bytes of independently useful key-value data and stays worth showing
//     with a failed checksum; 50 bytes with a mangled CRC has nothing left,
//     so ParseMid70Beacon rejects rather than flags. There is no crc_ok
//     field on Mid70Beacon for the same reason: every record you get back
//     passed both CRCs.
Result<std::vector<Mid70Beacon>> DiscoverMid70(const DiscoverOptions& opt);

// The pure parser behind it — no sockets, and the function the tests aim at
// captured bytes.
//   kInvalidArgument  null buffer
//   kProtocolError    not an SDK v1 broadcast at all: too short, wrong sof,
//                     declared length disagreeing with the datagram, a
//                     cmd_set/cmd_id that is not General/Broadcast, or a
//                     payload that is not sizeof(BroadcastDeviceInfo)
//   kChecksumFailed   it IS one, and one of the two CRCs does not verify
// Unlike the Mid-360's advisory heartbeat, a bad CRC here is FATAL to the
// parse: the frame is 50 bytes with no redundancy, so a corrupt one has
// nothing left worth showing an operator.
Result<Mid70Beacon> ParseMid70Beacon(const std::uint8_t* data, std::size_t len);

// The two checks, exposed separately because a diagnostic wants to say WHICH
// half failed — the preamble CRC covers only bytes 0..6, so "preamble ok,
// frame bad" means the payload was mangled in flight.
bool Mid70PreambleCrcOk(const std::uint8_t* data, std::size_t len);
bool Mid70FrameCrcOk(const std::uint8_t* data, std::size_t len);

// ===========================================================================
// Host reachability — "the lidar expects 192.168.1.5 and you are not it"
// ===========================================================================

struct LocalInterface {
  std::string name;     // "en7", "Ethernet 2"
  std::string ipv4;     // dotted quad
  std::string netmask;  // dotted quad; empty if the OS did not report one
  bool is_loopback = false;
  bool is_up = true;
};

// Every IPv4 address this machine currently holds, loopback included (the
// caller decides whether to care). getifaddrs on POSIX,
// GetAdaptersAddresses on Win32. kNotSupported on a platform with neither.
Result<std::vector<LocalInterface>> EnumerateLocalInterfaces();

struct HostCheck {
  // Does this machine actually hold the address the lidar will stream to?
  // False here is the field failure, exactly.
  bool host_ip_is_local = false;

  // Do we hold ANY address on the lidar's subnet? True + host_ip_is_local
  // false is the good case: the operator can add an alias and be done.
  bool on_lidar_subnet = false;

  // This machine's IPv4s that sit on the lidar's subnet, best first.
  std::vector<std::string> local_candidates;

  // What the app should offer to configure. Either the persisted host IP
  // (when we hold it, or when we can add it) or a local address to push into
  // the lidar instead. Empty only when nothing sensible can be suggested.
  std::string suggested_host_ip;
  std::string suggested_interface;  // where to add the alias / route

  // One operator-readable sentence. Stable enough to assert on in tests and
  // short enough to put in a dialog.
  std::string note;
};

// The real thing: enumerate this machine's interfaces and compare.
HostCheck CheckHostReachability(const Mid360Beacon& beacon);
// The testable thing: same logic against a supplied interface list.
HostCheck CheckHostReachability(const Mid360Beacon& beacon,
                                const std::vector<LocalInterface>& interfaces);

// IPv4 helpers, public because the host-check logic is worth unit-testing and
// the apps re-derive the same subnet arithmetic for their own dialogs.
bool ParseIpv4(const std::string& text, std::uint32_t* out_host_order);
std::string Ipv4ToString(std::uint32_t host_order);
bool SameSubnet(const std::string& a, const std::string& b, const std::string& netmask);
// Netmask → prefix length; 0xffffff00 → 24. -1 for a non-contiguous mask.
int PrefixLen(const std::string& netmask);

// ===========================================================================
// Serial: enumeration and protocol probes
// ===========================================================================

// macOS: /dev/cu.* minus the built-in Bluetooth/debug pseudo-ports.
// Linux: /dev/ttyUSB*, /dev/ttyACM*, /dev/ttyS* that actually exist.
// Windows: QueryDosDevice over COM1..COM255 (SetupAPI is not linked).
// Anything else: empty. Never fails — an unreadable /dev is an empty list.
std::vector<std::string> EnumerateSerialPorts();

// The D6's wire signature: 230400 8N1, AA 55 framing, and the VENDOR checksum
// variant the field session closed S1 on. A probe hit means those bytes were
// seen and checksummed, not that a file called ttyUSB0 exists.
struct D6Probe {
  std::string port;
  std::uint32_t baud = 230400;
  std::uint32_t packets_ok = 0;
  std::uint32_t packets_bad_checksum = 0;
  bool used_start_command = false;  // stage 2 was needed (see below)
};

// The STL-27L's wire signature (ITEM 119): 921600 8N1, 0x54 0x2C framing, a
// valid CRC8 and a plausible header. Nothing is ever written to the port — the
// LD-series free-runs from power-on, so there is no command to send and no
// reason to risk speaking into somebody else's device.
struct Stl27lProbe {
  std::string port;
  std::uint32_t baud = 921600;
  std::uint32_t packets_ok = 0;
  std::uint32_t packets_bad_crc = 0;
  std::uint16_t speed_dps = 0;  // the last packet's reported spin rate
};

// The JuxiTech ICM-42670-P module's wire signature (A18): 115200 8N1,
// `7E 23 <len> <func> <payload…> <sum8>` framing, where <len> counts the
// WHOLE frame and sum8 is the low byte of the sum of every preceding byte.
// The module free-runs at 25 Hz from power-on (100 Hz if somebody has
// configured it) and emits four frame types unprompted — 0x04 raw IMU (18
// payload bytes), 0x16 quaternion (16), 0x26 Euler (12), 0x32 barometer (16)
// — so like the STL-27L and the UM982 it is identifiable with ZERO bytes
// written. Reference: the vendor's Arduino driver, imu_uart_driver.cpp.
struct JuxiImuProbe {
  std::string port_path;
  std::uint32_t baud = 115200;   // the only rate this module speaks
  std::uint32_t frames_seen = 0; // checksum-valid frames of EVERY func
  std::uint32_t raw_frames = 0;  // ...of which func 0x04, the ones LIO wants
  // RAW (0x04) frames per second over the observed window — the IMU sample
  // rate, not the total frame rate, because that is the number that decides
  // whether the module is in its 25 Hz default or its 100 Hz mode. Zero if
  // the window was too short to measure.
  double frame_rate_hz = 0.0;
  // From a func 0x01 version frame IF one happens to go past. The probe
  // never REQUESTS one (that would mean writing), so version_known is false
  // far more often than not, and that is not a failure.
  std::uint8_t version[3] = {0, 0, 0};
  bool version_known = false;
};

// Unicore UM982: NMEA 0183 at an unknown baud — 230400 on the real unit, NOT
// the documented 115200 — plus Unicore's own "#UNI..." lines. `has_heading`
// means a dual-antenna heading sentence (GPTHS/xxHDT/#UNIHEADING) was seen,
// which is what tells the app whether to offer heading-aided georeferencing.
struct Um982Probe {
  std::string port;
  std::uint32_t baud = 0;
  bool has_heading = false;
  std::uint32_t sentences_ok = 0;
  std::uint32_t sentences_bad = 0;
};

// The sweep, in the order the field session says to try it: the OBSERVED rate
// first, the documented default second.
inline constexpr std::uint32_t kUm982BaudSweep[] = {230400, 115200, 460800, 38400, 9600};
inline constexpr std::size_t kUm982BaudSweepCount =
    sizeof(kUm982BaudSweep) / sizeof(kUm982BaudSweep[0]);

// Probe each path in turn, `per_port_ms` of wall clock each, and return the
// FIRST that identifies. std::nullopt means "none of them is one of these",
// which is a normal answer and not an error.
//
// WRITE POLICY (owner requirement, and the reason these are two functions and
// not one):
//   * Stage 1 is PASSIVE for every port and every device. We open, read, and
//     decide. A D6 that is already streaming (the common case — it streams on
//     power-up once started, and the vendor tool leaves it running) is
//     identified here with zero bytes written.
//   * Stage 2 exists only for the D6 and only when stage 1 was INCONCLUSIVE:
//     no AA 55 packets AND no text. It writes the 4-byte D6 start command,
//     listens, and — win or lose — writes the stop command before moving on.
//   * A port whose stage-1 bytes looked like TEXT (an NMEA talker, a shell
//     banner, a modem's AT chatter) never reaches stage 2. Writing AA 55 F0 0F
//     into a GNSS receiver's command port is exactly the kind of thing a
//     discovery scan must not do.
//   * ProbeSerialUm982 NEVER writes. A UM982 talks unprompted at 1 Hz.
std::optional<D6Probe> ProbeSerialD6(const std::vector<std::string>& port_paths,
                                     int per_port_ms);
// ITEM 119. PASSIVE ONLY — there is no stage 2 and no write, ever.
//
// ORDERING CONTRACT. Run this AFTER ProbeSerialD6 and on the ports it did not
// claim, which is what discovery's own caller (engine_cli --discover) does.
// The two probes open at different rates (230400 vs 921600) so neither can
// read the other's device as anything but noise, but the ordering is what
// makes that a guarantee rather than a probability: a D6 identified passively
// in stage 1 never reaches this function at all. Stl27lSniffer::LooksLikeD6()
// closes the remaining direction — see below.
std::optional<Stl27lProbe> ProbeSerialStl27l(const std::vector<std::string>& port_paths,
                                             int per_port_ms);
// A18. PASSIVE ONLY — this function NEVER WRITES A BYTE, at any stage, for
// any reason. The UM982 rule, for the same reason: 115200 is the rate half
// the world's serial devices come up at, and `7E 23 …` typed into somebody
// else's bootloader is exactly what a discovery scan must never do.
//
// ORDERING CONTRACT. Run this AFTER ProbeSerialD6 and ProbeSerialStl27l and
// BEFORE ProbeSerialUm982, on the ports the earlier probes did not claim.
// Both halves of that sentence are load-bearing:
//   * AFTER the two lidars, because they are the noisier protocols and their
//     probes are the ones that may write (the D6's stage 2). A lidar
//     identified first never reaches this function at all.
//   * BEFORE the UM982, because the UM982 probe SWEEPS five baud rates for
//     over a second each and would happily spend six seconds failing to read
//     an IMU module. Claiming the IMU first is what keeps a full sweep
//     bounded. The safety of that ordering rests on the text latch —
//     JuxiImuSniffer::LooksLikeText() — which latches on an NMEA or Unicore
//     line and refuses to identify, so a UM982 sitting at its DOCUMENTED
//     115200 default cannot be stolen by this probe before its own runs.
// Opens at 115200 and only 115200: there is no sweep, because the module has
// no other rate.
std::optional<JuxiImuProbe> ProbeSerialJuxiImu(const std::vector<std::string>& port_paths,
                                               int per_port_ms);
std::optional<Um982Probe> ProbeSerialUm982(const std::vector<std::string>& port_paths,
                                           int per_port_ms);

// --- the probe state machines, exposed for testing -------------------------
//
// Both probes are "open a port, push bytes through a sniffer, ask the sniffer
// what it saw". Tests push INJECTED byte streams through the same sniffers,
// so the identification logic is covered without a real port anywhere — the
// same seam UsbSerialSource gives the drivers.

class D6Sniffer {
 public:
  D6Sniffer();
  ~D6Sniffer();
  D6Sniffer(const D6Sniffer&) = delete;
  D6Sniffer& operator=(const D6Sniffer&) = delete;

  void Feed(const std::uint8_t* data, std::size_t n);

  // Two good packets. One is not enough: a single AA 55 with a plausible
  // 16-bit checksum turns up in random binary about once every few hundred
  // kilobytes, and a GNSS receiver's RTCM stream is not random.
  static constexpr std::uint32_t kPacketsToIdentify = 2;
  bool Identified() const;

  std::uint32_t packets_ok() const;
  std::uint32_t packets_bad_checksum() const;

  // "These bytes are somebody's text protocol." Latches on the first
  // credible ASCII line and gates stage 2 forever after.
  bool LooksLikeText() const;

  void Reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Stl27lSniffer {
 public:
  Stl27lSniffer();
  ~Stl27lSniffer();
  Stl27lSniffer(const Stl27lSniffer&) = delete;
  Stl27lSniffer& operator=(const Stl27lSniffer&) = delete;

  void Feed(const std::uint8_t* data, std::size_t n);

  // FOUR good packets, not the D6 probe's two. The LD frame's sync is only two
  // bytes wide and its CRC is only eight bits, so a single accepted packet is
  // a 1-in-16-million coincidence per byte offset rather than the D6's
  // 1-in-4-billion — over a second of a 921600 link that is not negligible.
  // Four, each also passing the header sanity band (see LooksSane below),
  // is.
  static constexpr std::uint32_t kPacketsToIdentify = 4;
  bool Identified() const;

  std::uint32_t packets_ok() const;
  std::uint32_t packets_bad_crc() const;
  // The last accepted packet's `speed` field, degrees/second.
  std::uint16_t speed_dps() const;

  // "These bytes are somebody's text protocol" — same latch and the same
  // purpose as D6Sniffer::LooksLikeText(): a GNSS receiver or a console must
  // never be mistaken for a lidar.
  bool LooksLikeText() const;

  // "These bytes are a COIN-D6." Latches when two checksum-valid AA-55
  // packets go past. This is the half of the mutual-exclusion guarantee that
  // can be tested off-hardware: feed a D6 revolution in and Identified()
  // stays false FOR A REASON, not by luck. It also covers the one case the
  // baud difference does not — a caller that opened the port at 230400 and
  // pointed this sniffer at it anyway.
  bool LooksLikeD6() const;

  void Reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class JuxiImuSniffer {
 public:
  JuxiImuSniffer();
  ~JuxiImuSniffer();
  JuxiImuSniffer(const JuxiImuSniffer&) = delete;
  JuxiImuSniffer& operator=(const JuxiImuSniffer&) = delete;

  void Feed(const std::uint8_t* data, std::size_t n);

  // THREE good frames. The sync is two bytes and the checksum is eight bits,
  // so one accepted frame is a 1-in-16-million coincidence per byte offset —
  // the STL-27L's arithmetic exactly — and the STL-27L's answer (four) is
  // not available here because this module emits only 25 frames a second in
  // its default mode, so a four-frame bar would need a longer dwell than the
  // one a five-port sweep can afford. Three at 1-in-2^24 each is ample.
  static constexpr std::uint32_t kFramesToIdentify = 3;
  bool Identified() const;

  std::uint32_t frames_ok() const;       // checksum-valid, any func
  std::uint32_t frames_bad_checksum() const;
  std::uint32_t raw_frames() const;      // func 0x04
  std::uint32_t quat_frames() const;     // func 0x16
  std::uint32_t euler_frames() const;    // func 0x26
  std::uint32_t baro_frames() const;     // func 0x32

  // A func 0x01 version frame, if one was ever seen. Never solicited.
  bool version_known() const;
  const std::uint8_t* version() const;   // three bytes: high, mid, low

  // "These bytes are somebody's text protocol" — the same latch, and the
  // same job, as D6Sniffer::LooksLikeText(). Here it is what protects the
  // UM982 from being claimed by a probe that runs before its own.
  bool LooksLikeText() const;

  void Reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Um982Sniffer {
 public:
  Um982Sniffer();
  ~Um982Sniffer();
  Um982Sniffer(const Um982Sniffer&) = delete;
  Um982Sniffer& operator=(const Um982Sniffer&) = delete;

  void Feed(const std::uint8_t* data, std::size_t n);

  // Two checksum-valid sentences at the same baud. At a WRONG baud the
  // framer sees garbage and the odds of two independent valid NMEA checksums
  // are ~1/65536 — which is what makes the sweep safe to automate.
  static constexpr std::uint32_t kSentencesToIdentify = 2;
  bool Identified() const;
  bool has_heading() const;
  std::uint32_t sentences_ok() const;
  std::uint32_t sentences_bad() const;

  void Reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace discovery
}  // namespace scanengine

#endif  // SCANENGINE_DISCOVERY_DISCOVERY_H
