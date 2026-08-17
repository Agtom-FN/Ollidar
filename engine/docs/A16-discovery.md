# A16 — device auto-discovery + the single-instance guard

**Scope:** `engine/src/discovery/**`,
`engine/include/scanengine/discovery/discovery.h`,
`engine/include/scanengine/core/instance_guard.h`,
`engine/src/core/instance_guard.cpp`, the ABI-6 half of
`engine/capi/scanengine_c.{h,cpp}`, `engine/tests/test_discovery.cpp`,
`engine_cli --discover`.
**Requirement:** the owner's item from the first real-hardware session —
*manual IP/port entry defeats the GUI*. Round-4 item 6 (single instance) rides
along because it is the same class of setup failure.
**Ground truth:** `captures/FIELD_SESSION_2026-08-17.md` and
`captures/mid360_real_30s.livoxdump`. Every wire fact below was read off those
bytes, not off a datasheet.
**Pattern:** `tools/fieldtest-kit` — identify a device by its PROTOCOL, never
by its name. This module is that kit's production sibling.

---

## 1. What ships

| Piece | Header | Impl |
| --- | --- | --- |
| Beacon record + parser (`Mid360Beacon`, `ParseMid360Beacon`, the two CRCs) | `discovery/discovery.h` | `src/discovery/mid360_beacon.cpp` |
| The listener (`DiscoverMid360`, `DiscoverOptions`) | ″ | `src/discovery/mid360_discover.cpp` |
| Host reachability + interface enumeration + IPv4 arithmetic | ″ | `src/discovery/host_check.cpp` |
| Serial enumeration (`EnumerateSerialPorts`) | ″ | `src/discovery/serial_enum.cpp` |
| Probes + the two sniffer state machines | ″ | `src/discovery/serial_probe.cpp` |
| The one place the engine opens a port | `src/discovery/serial_port.h` (internal) | `src/discovery/serial_port.cpp` |
| UDP/ifaddrs compat | `src/discovery/net_compat.h` (internal) | header-only |
| Single-instance guard | `core/instance_guard.h` | `src/core/instance_guard.cpp` |
| C ABI 6 | `capi/scanengine_c.h` | `capi/scanengine_c.cpp` |
| Tests | — | `tests/test_discovery.cpp` (18 cases, 226 assertions) |
| Fixture | — | `tests/integration/data/mid360_beacon.bin` (2 real heartbeats) |

No `CMakeLists.txt` edit was needed: `src/*.cpp` and `tests/test_*.cpp` are
globbed with `CONFIGURE_DEPENDS`.

Two naming notes. The owner's task fixed the entry-point names in PascalCase
(`DiscoverMid360`, `CheckHostReachability`, …) where the rest of the engine
spells free functions snake_case. Rather than mix conventions inside one
header, EVERY public entry point in `namespace discovery` is PascalCase and
every struct field stays snake_case; nothing outside that namespace adopts it.
The `capi` mirrors keep the C surface's `scan_*` spelling regardless.

---

## 2. The Mid-360 heartbeat

A Mid-360 broadcasts a 430-byte SDK2 control frame to
`255.255.255.255:56201` at 1 Hz, powered up and idle, configured or not. It is
the only passive way to find one: **the lidar does not answer ICMP**, so there
is no ping sweep alternative.

Two field facts drive the socket code, both in `net_compat.h::bind_any_udp()`:

1. **Any-bound sockets only.** The field session's note, verbatim: "heartbeat
   broadcasts reach any-bound sockets only". Binding the interface address —
   the obvious thing, and what the point-stream driver does — receives *zero*
   heartbeats on macOS/BSD. Discovery binds `INADDR_ANY`.
2. **Share the port.** `SO_REUSEADDR` + `SO_REUSEPORT`, so discovery can run
   beside Livox Viewer, beside the engine's own SDK2 backend, or beside a
   second discovery call, instead of fighting for 56201. If *no* requested
   port binds at all, that is `kBusy` with a message naming the likely cause.

Both 56201 and 56200 are listened on by default (56201 is what the capture
shows; 56200 is the `push_port` itself, and firmware conventions differ).

### 2.1 Frame layout (read off the capture)

```
off  size  field
  0     1  sof            0xAA
  1     1  version        0
  2     2  length         WHOLE frame, LE (430)
  4     4  seq_num        +1 per heartbeat
  8     2  cmd_id         0x0102 observed — NOT required by the parser
 10     1  cmd_type
 11     1  sender_type    1 = lidar
 12     6  reserved
 18     2  crc16          CRC16-CCITT-FALSE over bytes 0..17
 20     4  crc32          CRC32 (ISO-HDLC / zlib) over bytes 24..length
 24     2  key_num        31 in the capture
 26     2  reserved
 28     …  key_num × { u16 key, u16 len, u8 value[len] }
```

Keys the parser reads (everything else is skipped **by length**, which is what
makes a firmware that adds keys a non-event):

| Key | Meaning | Value in the capture |
| --- | --- | --- |
| `0x0004` | lidar IP + netmask + gateway | 192.168.1.159 / 255.255.255.0 / 192.168.1.1 |
| `0x0005` | state-info host cfg (where the beacon goes) | 255.255.255.255:**56201** |
| `0x0006` | **persisted point-cloud host** | **192.168.1.5**:56301 |
| `0x0007` | **persisted IMU host** | **192.168.1.5**:56401 |
| `0x8000` | serial number | `ARMCP7K0034759` |
| `0x8001` | product info | `DevType:Mid-360 FmType:App FmVer:35010108 BuildTime:2025/06/09` |
| `0x8002` | app version | 35.1.1.8 |
| `0x8005` | MAC | ec:72:f7:89:13:5f |

**Both CRCs were brute-forced against the capture, not assumed.**
CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection) reproduces `0x5BC6`;
the zlib CRC32 reproduces `0x127CB7CB`. An unverified checksum is worse than
none — it rejects good frames — so nothing here is guessed.

### 2.2 Serial-number note

The frame's SN field is `ARMCP7K0034759` (14 characters). The field session
quotes `MCP7K0034759`, the part printed on the unit. The parser reports the
frame's bytes verbatim and the test asserts **both**: the exact value and that
the quoted serial is a substring of it.

---

## 3. Parsing robustness

The parser is defensive in three layers, in this order:

1. **Structured key-value walk** (the normal path). Bounds-checked at every
   step: a key that claims more bytes than the frame holds aborts the walk
   rather than reading past it. The declared frame length is *clamped* to the
   datagram length, never trusted.
2. **CRC as information, not as a gate.** `crc_ok` is reported; a frame that
   fails still parses. Rationale: a beacon is advisory, and refusing to show
   an operator a lidar because one datagram was clipped is worse than showing
   it flagged. `DiscoverOptions::require_crc` flips this for a diagnostic that
   wants certainty.
3. **The anchor + IPv4 scan fallback** (`heuristic = true`), entered when the
   walk cannot make sense of the frame — reordered/extended firmware, a
   clipped datagram, a renumbered key. It:
   * anchors on the printable `DevType:` run and splits the product info;
   * takes the last long `[A-Z0-9]` run before that anchor as the SN;
   * finds the lidar's own address as an **IP + contiguous netmask + gateway
     on that same subnet** triple, scanning only *after* the 24-byte header.

   That last constraint matters: the header's CRC32 word looks exactly like an
   IPv4 address (`cb b7 7c 12` → 203.183.124.18), and the first version of
   this code happily reported it. The triple test (mask must be /8../30, and
   the gateway must be on the mask's subnet) rejects it. The test that caught
   this is `beacon_heuristic_survives_a_broken_key_walk`.

What is deliberately NOT accepted: a frame with the wrong `sof`, a frame
shorter than a header plus a key count, and a frame where neither the walk nor
the scan finds an IP or an SN. `0xAA`-framed noise gets `kCorruptData`, and
the test asserts that a Mid-360 *point* datagram and an NMEA line are both
rejected as `kProtocolError`.

Frames are deduplicated **by serial number** (by lidar IP, then source IP, for
a frame that carried no SN). Two lidars on one subnet is a real rig; an IP is
not a stable identity across a DHCP lease. A fully-parsed record is never
overwritten by a heuristic one.

---

## 4. Host reachability — the actual field failure

The Mid-360 persists the host address it was last told to stream to. It does
not ARP for it, does not answer ping, and does not complain when that host is
absent: it streams into a subnet where nobody listens, and the app shows
"connected, 0 points". On 2026-08-17 the fix was a hand-typed
`route add -host 192.168.1.159 -interface en7` plus an alias for
`192.168.1.5`. `CheckHostReachability()` turns that into one sentence and one
suggestion.

Four outcomes, each with its own note (all asserted in the tests):

| Case | `host_ip_is_local` | `on_lidar_subnet` | Note says |
| --- | --- | --- | --- |
| We hold the persisted address | ✓ | ✓ | "Ready: … holds that address on en7." |
| Right wire, wrong address | ✗ | ✓ | the alias command, *and* the option of reconfiguring the lidar |
| Wrong network | ✗ | ✗ | what we DO have, so the operator recognizes the mistake |
| No persisted host (factory-fresh) | ✗ | ✓/✗ | "LidarScan will point it at this machine (…)" |

Two traps the arithmetic avoids:

* **Loopback does not count.** A machine that happens to hold 192.168.1.5 on
  `lo0` is not reachable by a lidar. A naive "do I have this IP?" check says
  yes and produces a zero-point session.
* **A down interface is not a candidate.**

`EnumerateLocalInterfaces()` uses `getifaddrs` on POSIX and
`GetAdaptersAddresses` on Win32, both compiled unconditionally; anything else
is `kNotSupported`. The pure-logic overload takes the interface list as an
argument, which is how the four cases above are tested without a second NIC.

---

## 5. Serial: enumeration and probes

Enumeration is a **hint**; only the probe identifies. The field session had
four candidate `/dev/cu.*` on one Mac — the D6, the UM982, an unrelated ESP32
water-flow logger, and the built-in Bluetooth port — with names differing by
one digit.

* **macOS:** `/dev/cu.*` (never `tty.*`, which blocks on DCD), minus
  Bluetooth/debug-console/wlan-debug.
* **Linux:** `/dev/ttyUSB*`, `/dev/ttyACM*`, `/dev/ttyAMA*`. `ttyS*` is
  deliberately excluded — on a typical PC it is 32 phantom nodes, each costing
  a full probe dwell.
* **Windows:** `QueryDosDevice` over the device namespace (no SetupAPI, no
  extra import library), numerically sorted, with a per-name fallback loop.

### 5.1 The write policy

This is the part to review before changing anything:

* **Stage 1 is passive for every port and every device.** A D6 that is already
  streaming — the common case — is identified with zero bytes written.
* **Stage 2 exists only for the D6, only when stage 1 was inconclusive** (no
  `AA 55` packets AND no text), writes the 4-byte start command, listens, and
  writes the stop command afterwards win or lose.
* **A port that showed text never reaches stage 2.** `D6Sniffer` latches a
  "this is somebody's text protocol" flag on a 32-byte printable run or on a
  literal `$G` / `#UNI` start. Writing `AA 55 F0 0F` into a GNSS receiver's
  command port is exactly what a discovery scan must not do.
* **`ProbeSerialUm982` never writes.** A UM982 talks unprompted at 1 Hz.
* **A busy port is skipped silently** (`EBUSY`, `EACCES`, `ERROR_SHARING_VIOLATION`).
  On macOS that is usually the app's own capture session or a leftover
  process; a discovery scan complaining about it is noise.

### 5.2 The signatures

* **D6** — 230400 8N1, `AA 55` framing, **vendor** checksum variant. The
  field session closed that question (2430/2430 vendor vs 143 spec-literal),
  and the sniffer accepts only the vendor reading. Threshold: **two** good
  packets; one turns up in random binary often enough to matter.
* **UM982** — NMEA 0183 with a valid checksum, or a Unicore `#…*<8 hex>` log.
  Threshold: **two** checksum-valid sentences at one rate; at a wrong rate the
  odds of two independent valid NMEA checksums are ~1/65536, which is what
  makes the sweep safe to automate. `has_heading` latches on `THS`/`HDT`/`ROT`
  or a `#…HEADING…` log, and the winning rate gets one extra dwell purely to
  see whether a heading sentence is in the 1 Hz rotation.
* **Baud sweep order:** `230400, 115200, 460800, 38400, 9600` — the OBSERVED
  rate first, the documented default second, because the real unit was at
  230400 and a sweep that tries the datasheet first is a second slower on
  every single run.

`SerialPort` (internal) is the one place the engine opens a port, and it is
deliberately not a `ByteSource`: capture still goes through
`UsbSerialSource` and the app's own port handling (Tech Spec §3.1). The
exception is justified in `serial_port.h`'s header comment — you cannot
identify a device by its protocol without reading its bytes.

**Baud rates across platforms.** Linux needs a `B` constant (`B460800` is an
opaque code, not the number); Darwin's `termios.h` stops at `B230400` but
takes arbitrary rates, with `IOSSIOSPEED` as the fallback (declared inline, so
no IOKit header and no framework). A rate the platform cannot express is an
open error and that rate is skipped, not a crash.

---

## 6. The single-instance guard

The field session's note: "a first attempt left a port-holding process; later
runs failed SdkInit until killed". Every capture path binds FIXED UDP ports
(56100–56501) or claims an exclusive serial handle, and a second LidarScan
does not fail cleanly — it fails deep inside SdkInit, minutes into a setup the
operator thinks is working.

`scanengine::InstanceGuard` is an advisory lockfile with two layers:

1. **The OS advisory lock** — `flock(LOCK_EX|LOCK_NB)` / `LockFileEx`. This is
   the authority, and its one irreplaceable property is that the kernel
   releases it when the process dies, **however** it dies. A SIGKILL'd
   LidarScan must not lock the operator out of their own machine.
2. **The recorded pid** — what turns "busy" into a sentence an operator can
   act on: *"another LidarScan is running (pid 4242)"*. It is also the
   fallback where the OS lock is unavailable or lies (network filesystems),
   in which case a live foreign pid decides and a dead one is taken over.

**Same-process re-entry is `kOk`.** Two guards on one path in one process is
not a violation of "one LidarScan per machine" — it is a library initialized
twice, an app with a plugin, or the test binary. The claim is refcounted per
path per process; the OS lock is taken by the first holder and released by the
last. This is not just convenience: on POSIX a second `flock()` from the same
process on a *different* file description fails, so without the registry a
process would report itself as its own rival.

**`Engine::create()` deliberately does NOT claim it.** The cloud worker runs
several engines per host on purpose, `scanengine_tests` builds dozens, and
`engine_cli --post` is expected to run beside a live capture. Only a capture
UI has the "one at a time" requirement, so the APPS call it at startup, before
creating an Engine. The C ABI's `scan_instance_acquire()` wraps a
function-local static guard, released by `scan_instance_release()` or by
process exit.

**Platform behaviour summary**

| Platform | Mechanism | Crash releases? | Notes |
| --- | --- | --- | --- |
| macOS / Linux / Android | `flock` on `<tmp>/lidarscan-<app_id>.lock` | yes (kernel) | `TMPDIR` honoured |
| Windows | `LockFileEx` on byte 0 | yes (kernel) | `%TEMP%` |
| NFS / SMB `$TMPDIR` | flock may fail → pid + liveness | no (stale file is taken over) | warned in the log |

The lockfile is **not deleted** on release: deleting it races a rival that has
already opened it, and a leftover file with a dead pid is exactly the case the
liveness check exists for. `app_id` is sanitized to `[A-Za-z0-9_-]`, so a
hostile id cannot escape the temp directory.

---

## 7. C ABI 6

ABI 5 → 6. **Every addition is a new symbol or a new struct**; no ABI-5 struct
layout, function signature or enum value changed, so an ABI-5 consumer
relinks unmodified.

| Function | Answers |
| --- | --- |
| `scan_discover_mid360(timeout, out[], cap, &count)` | which lidars are broadcasting |
| `scan_host_check(&beacon, &result)` | will it stream to this machine |
| `scan_enumerate_serial(out[], cap, &count)` | which serial ports exist |
| `scan_probe_d6(ports, n, per_port_ms, &out)` | which one is the D6 |
| `scan_probe_um982(ports, n, per_port_ms, &out)` | which one is the UM982, at what rate |
| `scan_instance_acquire(app_id, lock_path, &pid)` / `scan_instance_release()` | am I the only LidarScan |
| `scan_current_process_id()` | (for the message) |

Conventions kept: out-parameters last, `scan_error_t` return, the two-call
capacity protocol with the TRUE total always written and
`SCAN_ERR_CAPACITY_EXCEEDED` on truncation (identical to
`scan_ntrip_fetch_sourcetable`), and a catch-all around every entry point.

Convention 4 (engine-owned strings valid until the next call) does **not**
apply to these structs: they carry fixed char arrays copied by value, so a JNI
caller can hold a beacon indefinitely. Truncation is impossible for real
values and is asserted as such (`static_assert` on the array bounds).

**Drift guards.** These mirrors carry no enums, so the drift that matters is
numeric: `SCAN_MID360_PUSH_PORT` / `_ALT` are `static_assert`ed against
`discovery::kMid360PushPort` / `kMid360PushPortAlt`, because an Android app
that pre-binds the socket itself (`android/NOTES.md` §8 finding 1) must not
end up on a different port from the engine's listener.
`SCAN_ABI_VERSION == kEngineAbiVersion` is asserted as always.

`SCAN_ERR_NOT_FOUND` from the two probes means "that device is not on any of
these ports" — a normal answer a picker displays, not an I/O failure. That is
why an unopenable port does not produce `SCAN_ERR_IO`.

---

## 8. Tests (`tests/test_discovery.cpp`, 18 cases / 226 assertions)

No real port and no real lidar is required or used.

| Case | What it pins |
| --- | --- |
| `beacon_parses_the_real_capture` | every field, against the committed real bytes: SN, .159, mask, gw, host .5, ports 56301/56401, fw 35.1.1.8 + "35010108", MAC, 31 keys, `crc_ok` |
| `beacon_second_record_is_the_same_lidar` | the two captured frames differ but are one lidar |
| `beacon_crcs_are_the_real_algorithms` | payload corruption fails CRC32 only; header corruption fails CRC16 only; a bad-CRC frame still parses and says so |
| `beacon_rejects_what_is_not_a_beacon` | null, short, all-zero, NMEA, `0xAA`-framed noise |
| `beacon_heuristic_survives_a_broken_key_walk` | the fallback recovers .159/.5/SN/DevType from a frame whose key count was corrupted; `allow_heuristic=false` refuses |
| `ipv4_helpers` | parse/format/subnet/prefix incl. non-contiguous masks and the /24 fallback |
| `host_check_against_fake_interfaces` (7 subcases) | the four outcomes + loopback + down-interface + factory-fresh |
| `enumerate_local_interfaces_on_this_machine` | real enumeration works and the real check produces a note |
| `d6_sniffer_identifies_the_wire_signature` (7 subcases) | clean revolution, 7-byte chunking, one-packet threshold, spec-literal checksum REJECTED, NMEA/Unicore text latch, random binary, reset |
| `um982_sniffer_identifies_nmea_and_heading` (6 subcases) | the real 30 s field NMEA capture (heading detected), threshold, bad checksums, Unicore `#` logs, wrong-baud garbage, reset |
| `serial_enumeration_and_empty_probes` | enumeration shape; empty and nonexistent port lists answer promptly |
| `mid360_timeout_and_arguments` | negative timeout refused; a 250 ms discovery really waits and really stops |
| `mid360_receives_and_dedups_a_real_heartbeat` | the full socket path: two real datagrams to a loopback port produce ONE record with `beacons_seen == 2`, the source IP and the port heard on |
| `mid360_stop_after_first_device` | `stop_after_devices` cuts an 8 s timeout short |
| `instance_guard/*` (4 cases) | two guards in one process both OK (second `same_process`); a foreign LIVE pid is `kBusy` with the pid in the message; a stale lockfile is taken over; hostile `app_id` sanitized, release idempotent |

The D6 spec-literal case carries a trap worth remembering: with an **even**
number of identical samples the two checksum readings XOR to the same value,
so that subcase uses seven.

The C ABI additions are exercised from actual C in `tests/capi_smoke.c`
(steps 150–173): both capacity protocols, every null-argument rejection, the
host check on the field session's beacon, both probes against bogus paths, and
the guard's acquire/re-acquire/release.

---

## 9. `engine_cli --discover`

The field-verification face, and the thing to run first at the start of a
hardware session:

```
engine_cli --discover [seconds] [--no-serial] [--no-lidar]
```

It prints each beacon (SN, firmware, addresses, the persisted host, CRC
status) followed by the host-check sentence, then enumerates serial ports and
runs both probes, D6 first (a port identified as the D6 is removed from the
UM982 sweep). Exit 0 if anything was found, 1 if not.

---

## 10. Platform-deferred / next pass

1. **Android.** `getifaddrs` exists on API 24+, and both are compiled for the
   NDK target, but nothing here has been run on a device. Android's UDP
   broadcast reception additionally needs a `WifiManager.MulticastLock` held
   by the app for some vendors' Wi-Fi stacks — the engine cannot take that
   lock, so the JNI layer must, around a `scan_discover_mid360()` call.
2. **Windows serial enumeration is name-only.** `QueryDosDevice` gives the COM
   names but not the friendly names; SetupAPI would add them. Deliberately not
   linked, because identification is by protocol — the friendly name would be
   a display nicety only.
3. **Reconfiguring the lidar** (pushing this host's address into it) is *not*
   in this task: `CheckHostReachability` reports and suggests, and the SDK2
   config push that would act on the suggestion lives in the Mid-360 driver.
   That is the obvious next wiring item, and the note's second half ("or let
   LidarScan reconfigure the lidar to stream to …") is written against it.
4. **The apps must call `InstanceGuard`.** The engine does not claim it
   implicitly (§6); the desktop app's `main()` and the Android
   `Application.onCreate()` are the two call sites, and the `kBusy` message is
   already operator-ready.
5. **Two concurrent `DiscoverMid360()` calls** on a platform without
   `SO_REUSEPORT` will contend; the second gets `kBusy`. Not worth a shared
   listener until an app needs one.

---

## 11. Build/test verification

```
cmake -S engine -B engine/build/a16 -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENGINE_WARNINGS_AS_ERRORS=ON
cmake --build engine/build/a16 -j8            # clean, zero warnings
ctest --test-dir engine/build/a16 -LE 'sim|sim-rtk' --output-on-failure
./engine/build/a16/scanengine_capi_smoke
```

All five ctest entries pass; `scanengine_tests` reports 517 cases /
~2.28 M assertions with the 18 new ones included, and the C ABI smoke test
passes standalone.
