#include "app/DeviceDiscovery.h"

#include <QDeadlineTimer>

#include <algorithm>
#include <string>
#include <vector>

#include "scanengine/discovery/discovery.h"

namespace lidarscan {
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

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

void discoverSerial(int probe_ms, D6Discovery* d6_out, Um982Discovery* um982_out) {
  const std::vector<std::string> ports = scanengine::discovery::EnumerateSerialPorts();

  if (const auto d6 = scanengine::discovery::ProbeSerialD6(ports, probe_ms)) {
    d6_out->found = true;
    d6_out->port = qs(d6->port);
    d6_out->packets_ok = int(d6->packets_ok);
    d6_out->packets_bad_checksum = int(d6->packets_bad_checksum);
  }
  if (const auto um982 = scanengine::discovery::ProbeSerialUm982(ports, probe_ms)) {
    um982_out->found = true;
    um982_out->port = qs(um982->port);
    um982_out->baud = int(um982->baud);
    um982_out->has_heading = um982->has_heading;
    um982_out->sentences_ok = int(um982->sentences_ok);
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

DiscoveryResult runDiscoveryBlocking(int mid360_timeout_ms, int serial_probe_ms) {
  DiscoveryResult out;
  out.mid360 = discoverMid360(mid360_timeout_ms, /*gate=*/nullptr, &out.mid360_error);
  discoverSerial(serial_probe_ms, &out.d6, &out.um982);
  return out;
}

DiscoveryWorker::DiscoveryWorker(int mid360_timeout_ms, int serial_probe_ms, QObject* parent)
    : QObject(parent),
      mid360_timeout_ms_(mid360_timeout_ms),
      serial_probe_ms_(serial_probe_ms),
      gate_(std::make_shared<DiscoveryGate>()) {
  qRegisterMetaType<DiscoveryResult>("lidarscan::DiscoveryResult");
}

void DiscoveryWorker::run() {
  DiscoveryResult out;
  Q_EMIT phase(QObject::tr("Listening for Mid-360 heartbeat…"));
  out.mid360 = discoverMid360(mid360_timeout_ms_, gate_.get(), &out.mid360_error);
  out.canceled = gate_->wasCanceled();
  // The serial probes hold /dev/cu.* handles, never UDP 56201, so they are not
  // part of the port conflict — but a canceled pass is being cut short because
  // something more urgent (a device start) is waiting, and spending another
  // ~3 s sweeping serial ports for a result that will be discarded helps
  // nobody.
  if (!out.canceled) {
    Q_EMIT phase(QObject::tr("Probing serial ports…"));
    discoverSerial(serial_probe_ms_, &out.d6, &out.um982);
    out.canceled = gate_->wasCanceled();
  }
  gate_->markFinished();
  Q_EMIT finished(out);
}

}  // namespace lidarscan
