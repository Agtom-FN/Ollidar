#include "app/DeviceDiscovery.h"

#include <string>
#include <vector>

#include "scanengine/discovery/discovery.h"

namespace lidarscan {
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

Mid360Discovery discoverMid360(int timeout_ms, QString* error) {
  Mid360Discovery out;
  const auto beacons = scanengine::discovery::DiscoverMid360(timeout_ms);
  if (!beacons.ok()) {
    if (error) *error = QString::fromUtf8(scanengine::error_str(beacons.error()));
    return out;
  }
  if (beacons.value().empty()) return out;  // legitimate "nothing heard", not an error

  // First beacon seen wins — a real site has one Mid-360 on the bench; if a
  // second answers, "not seen" would be the wrong message for it, so this is
  // deliberately the common case, not a multi-device picker.
  const auto& b = beacons.value().front();
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
  return out;
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

DiscoveryResult runDiscoveryBlocking(int mid360_timeout_ms, int serial_probe_ms) {
  DiscoveryResult out;
  out.mid360 = discoverMid360(mid360_timeout_ms, &out.mid360_error);
  discoverSerial(serial_probe_ms, &out.d6, &out.um982);
  return out;
}

DiscoveryWorker::DiscoveryWorker(int mid360_timeout_ms, int serial_probe_ms, QObject* parent)
    : QObject(parent),
      mid360_timeout_ms_(mid360_timeout_ms),
      serial_probe_ms_(serial_probe_ms) {
  qRegisterMetaType<DiscoveryResult>("lidarscan::DiscoveryResult");
}

void DiscoveryWorker::run() {
  DiscoveryResult out;
  Q_EMIT phase(QObject::tr("Listening for Mid-360 heartbeat…"));
  out.mid360 = discoverMid360(mid360_timeout_ms_, &out.mid360_error);
  Q_EMIT phase(QObject::tr("Probing serial ports…"));
  discoverSerial(serial_probe_ms_, &out.d6, &out.um982);
  Q_EMIT finished(out);
}

}  // namespace lidarscan
