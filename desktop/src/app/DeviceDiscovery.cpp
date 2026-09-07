#include "app/DeviceDiscovery.h"

#include <QDeadlineTimer>

#include "app/FieldLog.h"

#include <algorithm>
#include <string>
#include <vector>

#include "scanengine/discovery/discovery.h"

namespace lidarscan {
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// DiscoverOptions::raw_sink. A C function pointer, called on the discovery
// thread once per received datagram, and required by that header to be quick:
// this one appends 14 header bytes plus the payload to an already-open fd and
// returns. No parsing, no allocation beyond the record header, no engine call.
void mid70RawSink(const std::uint8_t* data, std::size_t len, const char* source_ip,
                  std::uint16_t port, void* /*user*/) {
  FieldLog::mid70Datagram(data, len, source_ip, port);
}

void fillMid360(Mid360Discovery& out, const scanengine::discovery::Mid360Beacon& b);

// One Mid-360 listen, sliced into DiscoveryGate::kChunkMs windows so a cancel
// can land between slices and the UDP port is provably free the moment the
// last slice returns. `gate` may be null (the synchronous
// runDiscoveryBlocking() entry point has nobody to cancel it), in which case
// this is just the same listen expressed as N short calls.
//
// stop_after_devices = 1 per slice: this adapter only ever reports the FIRST
// beacon (see below), so a slice that already heard one has no reason to sit
// out the rest of its clock. That also means the common "the lidar is right
// there" case returns in well under a second instead of the full timeout.
Mid360Discovery discoverMid360(int timeout_ms, DiscoveryGate* gate, QString* error) {
  Mid360Discovery out;
  QString last_error;
  int remaining = timeout_ms > 0 ? timeout_ms : DiscoveryGate::kChunkMs;

  while (remaining > 0) {
    const int slice = std::min(remaining, DiscoveryGate::kChunkMs);
    remaining -= slice;

    if (gate && !gate->beginUdpSlice()) break;  // canceled: never bind again
    scanengine::discovery::DiscoverOptions opt;
    opt.timeout_ms = slice;
    opt.stop_after_devices = 1;
    const auto beacons = scanengine::discovery::DiscoverMid360(opt);
    if (gate) gate->endUdpSlice();

    if (!beacons.ok()) {
      // kBusy on one slice (Livox Viewer holding the port, say) is worth
      // reporting, but only if no later slice succeeds.
      last_error = QString::fromUtf8(scanengine::error_str(beacons.error()));
      continue;
    }
    if (beacons.value().empty()) continue;  // legitimate "nothing heard this slice"

    // Spec item (d): every beacon this pass heard, VERBATIM, not just the one
    // the UI ends up using. A second Mid-360 answering is exactly the kind of
    // thing that explains a "wrong IP" bug an hour later.
    for (const auto& b : beacons.value()) {
      FieldLog::info("discovery",
                     QString("event=mid360_beacon sn=%1 fw=%2 fw_text=%3 lidar_ip=%4 "
                             "netmask=%5 gateway=%6 persisted_host_ip=%7 push_port=%8 "
                             "beacons_seen=%9 describe=\"%10\"")
                         .arg(QString::fromStdString(b.sn),
                              QString::fromStdString(b.fw_version),
                              QString::fromStdString(b.fw_version_text),
                              QString::fromStdString(b.lidar_ip),
                              QString::fromStdString(b.netmask),
                              QString::fromStdString(b.gateway),
                              QString::fromStdString(b.persisted_host_ip))
                         .arg(b.push_port_seen)
                         .arg(b.beacons_seen)
                         .arg(QString::fromStdString(b.describe())));
    }

    // First beacon seen wins — a real site has one Mid-360 on the bench; if a
    // second answers, "not seen" would be the wrong message for it, so this is
    // deliberately the common case, not a multi-device picker.
    fillMid360(out, beacons.value().front());
    return out;
  }

  if (error) *error = last_error;
  return out;
}

void fillMid360(Mid360Discovery& out, const scanengine::discovery::Mid360Beacon& b) {
  out.found = true;
  out.sn = qs(b.sn);
  // fw_version_text ("35010108") is the raw FmVer field and what
  // captures/FIELD_SESSION_2026-08-17.md quotes verbatim; fw_version
  // ("35.1.1.8") is the dotted form discovery.h derives from it. Prefer the
  // raw text when the beacon carried it — it is the field session's exact
  // case — and fall back to the dotted form for a beacon that didn't.
  out.fw_version = qs(b.fw_version_text.empty() ? b.fw_version : b.fw_version_text);
  out.lidar_ip = qs(b.lidar_ip);
  out.netmask = qs(b.netmask);
  out.gateway = qs(b.gateway);
  out.persisted_host_ip = qs(b.persisted_host_ip);

  const auto check = scanengine::discovery::CheckHostReachability(b);
  out.host_ip_is_local = check.host_ip_is_local;
  out.on_lidar_subnet = check.on_lidar_subnet;
  for (const auto& c : check.local_candidates) out.local_candidates << qs(c);
  out.suggested_host_ip = qs(check.suggested_host_ip);
  out.suggested_interface = qs(check.suggested_interface);
  out.host_check_note = qs(check.note);
}

// The Mid-70's SDK v1 broadcast (UDP 55000), sliced against the SAME gate as
// the Mid-360 listen. The port numbers differ; the reason for the gate does not
// — see the DiscoveryGate comment in the header for why 55000 is a
// device-arming port too. `stop_after_devices = 1` for the same reason as the
// Mid-360 listen: this adapter reports the FIRST beacon, so a slice that already
// heard one has no reason to sit out its clock.
Mid70Discovery discoverMid70(int timeout_ms, DiscoveryGate* gate, QString* error) {
  Mid70Discovery out;
  QString last_error;
  int remaining = timeout_ms > 0 ? timeout_ms : DiscoveryGate::kChunkMs;

  while (remaining > 0) {
    const int slice = std::min(remaining, DiscoveryGate::kChunkMs);
    remaining -= slice;

    if (gate && !gate->beginUdpSlice()) break;  // canceled: never bind again
    scanengine::discovery::DiscoverOptions opt;
    opt.timeout_ms = slice;
    opt.stop_after_devices = 1;
    // FIELD-TEST HOOK (A17, discovery.h DiscoverOptions::raw_sink). Every
    // datagram this listen receives — before parsing, and whether or not it
    // parses — is appended to a capture_mid70.py-compatible LX70_CAP file. That
    // is how the mid70_broadcast fixture §20.3 lists as missing gets home from
    // the first real rig, with no second tool on the Mac and no operator step.
    // Called on the discovery thread; FieldLog is safe from any thread.
    opt.raw_sink = &mid70RawSink;
    opt.raw_sink_user = nullptr;
    // `ports` left empty: DiscoverMid70 defaults it to {kMid70BroadcastPort}.
    // require_crc and allow_heuristic are documented as IGNORED by this call
    // (a 50-byte fixed-layout frame has nothing to guess at, and both of its
    // CRCs are always enforced), so they are not set here either.
    const auto beacons = scanengine::discovery::DiscoverMid70(opt);
    if (gate) gate->endUdpSlice();

    if (!beacons.ok()) {
      // kBusy here almost always means a Livox SDK — ours, Livox Viewer's, or a
      // livox_ros_driver node — already holds 55000. Worth reporting, but only
      // if no later slice succeeds.
      last_error = QString::fromUtf8(scanengine::error_str(beacons.error()));
      continue;
    }
    if (beacons.value().empty()) continue;  // legitimate "nothing heard this slice"

    for (const auto& bb : beacons.value()) {
      FieldLog::info("discovery",
                     QString("event=mid70_beacon broadcast_code=%1 dev_type=%2 "
                             "dev_type_name=%3 lidar_ip=%4 source_ip=%5 push_port=%6 "
                             "beacons_seen=%7 describe=\"%8\"")
                         .arg(QString::fromStdString(bb.broadcast_code))
                         .arg(int(bb.dev_type))
                         .arg(QString::fromStdString(bb.dev_type_name),
                              QString::fromStdString(bb.lidar_ip),
                              QString::fromStdString(bb.source_ip))
                         .arg(bb.push_port_seen)
                         .arg(bb.beacons_seen)
                         .arg(QString::fromStdString(bb.describe())));
    }

    const auto& b = beacons.value().front();
    out.found = true;
    out.broadcast_code = qs(b.broadcast_code);
    out.dev_type = int(b.dev_type);
    out.dev_type_name = qs(b.dev_type_name);
    out.source_ip = qs(b.source_ip);
    // THE LIDAR'S ADDRESS IS THE UDP SOURCE, NOT A FIELD IN THE FRAME.
    // sizeof(BroadcastDeviceInfo) is 35 in memory and carries an ip[16] text
    // field, but the real wire frame is 34 bytes: the SDK's own receiver
    // (device_discovery.cpp) copies only the 19-byte prefix and fills ip[] from
    // the datagram's source address. So on real hardware Mid70Beacon::lidar_ip
    // is EMPTY and source_ip is the answer — which is also what the SDK
    // itself connects to. lidar_ip is kept as the fallback for the 35-byte form
    // (a fixture, or firmware that does send it) rather than ignored.
    out.lidar_ip = qs(b.source_ip.empty() ? b.lidar_ip : b.source_ip);

    // HOST REACHABILITY FOR A DEVICE WITH NO HOST FIELD OF ITS OWN.
    // discovery.h has exactly one CheckHostReachability() and it takes a
    // Mid360Beacon — there is no by-IP overload. Its implementation
    // (engine/src/discovery/host_check.cpp) reads exactly three fields of that
    // struct: persisted_host_ip, lidar_ip and netmask. An SDK v1 broadcast
    // carries the lidar's own IP and nothing else, so a Mid360Beacon holding
    // that IP alone is not a forgery — it is a complete and truthful statement
    // of everything the Mid-70 told us. The empty persisted_host_ip then
    // selects exactly the right branch, "this lidar has no host configured;
    // here is an address of yours on its subnet", which is the literal truth
    // for SDK v1: the host address is not persisted on the device at all, it is
    // named in the handshake every time. Nothing here is inferred beyond that.
    scanengine::discovery::Mid360Beacon as_if;
    as_if.lidar_ip = out.lidar_ip.toStdString();  // i.e. source_ip on real hardware
    const auto check = scanengine::discovery::CheckHostReachability(as_if);
    out.host_ip_is_local = check.host_ip_is_local;
    out.on_lidar_subnet = check.on_lidar_subnet;
    for (const auto& c : check.local_candidates) out.local_candidates << qs(c);
    out.suggested_host_ip = qs(check.suggested_host_ip);
    out.suggested_interface = qs(check.suggested_interface);
    out.host_check_note = qs(check.note);
    return out;
  }

  if (error) *error = last_error;
  return out;
}

// ORDERING IS A CONTRACT HERE, NOT A STYLE CHOICE (discovery.h, ProbeSerial*):
// the D6 first, because its probe is the only one that may ever WRITE and a D6
// identified passively never reaches the probes below it; the JuxiTech IMU
// next, because claiming it before the UM982 is what keeps a full sweep bounded
// (the UM982 probe sweeps five baud rates for over a second EACH, and would
// otherwise spend six seconds failing to read an IMU module); the UM982 last.
// ProbeSerialStl27l is not run on the desktop at all — it never has been, the
// STL-27L being a phone sensor here — so the "after the STL-27L" half of the
// IMU probe's contract is vacuous on this path rather than violated.
//
// Every probe gets the SAME port list rather than the leftovers of the one
// before, which is what discovery.h's own caller (engine_cli --discover) does.
// The safety of that rests on each sniffer's refusal to identify a device it is
// not — JuxiImuSniffer::LooksLikeText() latches on an NMEA or Unicore line and
// declines — not on bookkeeping here.
void discoverSerial(int probe_ms, D6Discovery* d6_out, JuxiImuDiscovery* juxi_out,
                    Um982Discovery* um982_out) {
  const std::vector<std::string> ports = scanengine::discovery::EnumerateSerialPorts();
  {
    QStringList names;
    for (const auto& p : ports) names << qs(p);
    FieldLog::info("discovery", QString("event=serial_ports probe_ms=%1 n=%2 ports=%3")
                                    .arg(probe_ms)
                                    .arg(int(ports.size()))
                                    .arg(names.isEmpty() ? QStringLiteral("-")
                                                         : names.join(',')));
  }

  if (const auto d6 = scanengine::discovery::ProbeSerialD6(ports, probe_ms)) {
    d6_out->found = true;
    d6_out->port = qs(d6->port);
    d6_out->packets_ok = int(d6->packets_ok);
    d6_out->packets_bad_checksum = int(d6->packets_bad_checksum);
    FieldLog::info("discovery", QString("event=probe_d6 hit=1 port=%1 baud=%2 packets_ok=%3 "
                                        "packets_bad_checksum=%4 used_start_command=%5")
                                    .arg(d6_out->port)
                                    .arg(d6->baud)
                                    .arg(d6->packets_ok)
                                    .arg(d6->packets_bad_checksum)
                                    .arg(d6->used_start_command ? 1 : 0));
  } else {
    FieldLog::info("discovery", "event=probe_d6 hit=0");
  }
  if (const auto juxi = scanengine::discovery::ProbeSerialJuxiImu(ports, probe_ms)) {
    juxi_out->found = true;
    juxi_out->port = qs(juxi->port_path);
    juxi_out->frame_rate_hz = juxi->frame_rate_hz;
    juxi_out->frames_seen = int(juxi->frames_seen);
    juxi_out->raw_frames = int(juxi->raw_frames);
    // frame_rate_hz is the number that says whether the module is still in its
    // 25 Hz power-on default or has been put into 100 Hz mode, so it is logged
    // to two decimals rather than rounded into a category.
    FieldLog::info("discovery",
                   QString("event=probe_juxi_imu hit=1 port=%1 baud=%2 frame_rate_hz=%3 "
                           "frames_seen=%4 raw_frames=%5")
                       .arg(juxi_out->port)
                       .arg(juxi->baud)
                       .arg(juxi->frame_rate_hz, 0, 'f', 2)
                       .arg(juxi->frames_seen)
                       .arg(juxi->raw_frames));
  } else {
    FieldLog::info("discovery", "event=probe_juxi_imu hit=0");
  }
  if (const auto um982 = scanengine::discovery::ProbeSerialUm982(ports, probe_ms)) {
    um982_out->found = true;
    um982_out->port = qs(um982->port);
    um982_out->baud = int(um982->baud);
    um982_out->has_heading = um982->has_heading;
    um982_out->sentences_ok = int(um982->sentences_ok);
    FieldLog::info("discovery",
                   QString("event=probe_um982 hit=1 port=%1 baud=%2 has_heading=%3 "
                           "sentences_ok=%4")
                       .arg(um982_out->port)
                       .arg(um982->baud)
                       .arg(um982->has_heading ? 1 : 0)
                       .arg(um982->sentences_ok));
  } else {
    FieldLog::info("discovery", "event=probe_um982 hit=0");
  }
}

}  // namespace

// --- DiscoveryGate ----------------------------------------------------------

bool DiscoveryGate::beginUdpSlice() {
  QMutexLocker lock(&mutex_);
  if (cancel_) return false;
  udp_bound_ = true;
  return true;
}

void DiscoveryGate::endUdpSlice() {
  QMutexLocker lock(&mutex_);
  udp_bound_ = false;
  released_.wakeAll();
}

void DiscoveryGate::markFinished() {
  QMutexLocker lock(&mutex_);
  finished_ = true;
  udp_bound_ = false;
  released_.wakeAll();
}

bool DiscoveryGate::wasCanceled() const {
  QMutexLocker lock(&mutex_);
  return cancel_;
}

bool DiscoveryGate::cancelAndWaitForSockets(int wait_ms) {
  QMutexLocker lock(&mutex_);
  cancel_ = true;
  // Taking the lock is itself half the handshake: if the worker is between
  // slices it cannot enter beginUdpSlice() until we let go, and when it does
  // it sees cancel_ and stops. If it is INSIDE a slice, udp_bound_ is true
  // and we wait here for endUdpSlice()/markFinished() to wake us.
  const QDeadlineTimer deadline(wait_ms);
  while (udp_bound_) {
    if (!released_.wait(&mutex_, deadline)) break;  // timed out
  }
  return !udp_bound_;
}

DiscoveryResult runDiscoveryBlocking(int mid360_timeout_ms, int mid70_timeout_ms,
                                     int serial_probe_ms) {
  DiscoveryResult out;
  out.mid360 = discoverMid360(mid360_timeout_ms, /*gate=*/nullptr, &out.mid360_error);
  out.mid70 = discoverMid70(mid70_timeout_ms, /*gate=*/nullptr, &out.mid70_error);
  discoverSerial(serial_probe_ms, &out.d6, &out.juxi_imu, &out.um982);
  return out;
}

DiscoveryWorker::DiscoveryWorker(int mid360_timeout_ms, int mid70_timeout_ms, int serial_probe_ms,
                                 QObject* parent)
    : QObject(parent),
      mid360_timeout_ms_(mid360_timeout_ms),
      mid70_timeout_ms_(mid70_timeout_ms),
      serial_probe_ms_(serial_probe_ms),
      gate_(std::make_shared<DiscoveryGate>()) {
  qRegisterMetaType<DiscoveryResult>("lidarscan::DiscoveryResult");
}

void DiscoveryWorker::run() {
  DiscoveryResult out;
  Q_EMIT phase(QObject::tr("Listening for Mid-360 heartbeat…"));
  out.mid360 = discoverMid360(mid360_timeout_ms_, gate_.get(), &out.mid360_error);
  out.canceled = gate_->wasCanceled();

  // A17: the second UDP listen, on 55000, under the same gate and behind the
  // same cancel check. Sequential rather than concurrent — the gate's whole
  // guarantee is "one slice in flight, and a cancel waits for THAT slice to
  // return", and two overlapping listens would make "provably closed" mean two
  // things at once.
  if (!out.canceled) {
    Q_EMIT phase(QObject::tr("Listening for a Mid-70 broadcast…"));
    out.mid70 = discoverMid70(mid70_timeout_ms_, gate_.get(), &out.mid70_error);
    out.canceled = gate_->wasCanceled();
  }

  // The serial probes hold /dev/cu.* handles, never a UDP port, so they are not
  // part of the port conflict — but a canceled pass is being cut short because
  // something more urgent (a device start) is waiting, and spending another
  // ~3 s sweeping serial ports for a result that will be discarded helps
  // nobody.
  if (!out.canceled) {
    Q_EMIT phase(QObject::tr("Probing serial ports…"));
    discoverSerial(serial_probe_ms_, &out.d6, &out.juxi_imu, &out.um982);
    out.canceled = gate_->wasCanceled();
  }
  gate_->markFinished();
  FieldLog::info("discovery",
                 QString("event=pass_done canceled=%1 mid360_found=%2 mid360_error=%3 "
                         "mid70_found=%4 mid70_error=%5 d6_found=%6 juxi_found=%7 "
                         "um982_found=%8 mid70_datagrams_dumped=%9")
                     .arg(out.canceled ? 1 : 0)
                     .arg(out.mid360.found ? 1 : 0)
                     .arg(out.mid360_error.isEmpty() ? QStringLiteral("-") : out.mid360_error)
                     .arg(out.mid70.found ? 1 : 0)
                     .arg(out.mid70_error.isEmpty() ? QStringLiteral("-") : out.mid70_error)
                     .arg(out.d6.found ? 1 : 0)
                     .arg(out.juxi_imu.found ? 1 : 0)
                     .arg(out.um982.found ? 1 : 0)
                     .arg(FieldLog::mid70Datagrams()));
  Q_EMIT finished(out);
}

}  // namespace lidarscan
