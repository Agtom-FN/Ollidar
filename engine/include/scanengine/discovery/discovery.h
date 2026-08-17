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
