// DeviceDiscovery.h — Qt-side adapter over the engine's discovery API
// (engine/include/scanengine/discovery/discovery.h), moved off the GUI
// thread so a ~6 s Mid-360 heartbeat listen + serial probe sweep does not
// freeze the capture window.
//
// WHY THIS EXISTS (owner, field session, docs/design/REVIEW_FEEDBACK.md
// round 4 item 5): "Manual IP entry defeated the GUI on first contact." The
// field session (captures/FIELD_SESSION_2026-08-17.md) found the Mid-360's
// lidar IP, SN, firmware and the host IP it was already configured to talk
// to (192.168.1.159 / MCP7K0034759 / 35010108 / 192.168.1.5) all by
// listening — no manual entry. This file is the desktop half of turning
// that into a button.
//
// THE ADAPTER SEAM. discovery.h (A16) landed concurrently with this task from
// a different workstream (Do NOT edit engine/**), so every call into
// `scanengine::discovery::*` is isolated to this .cpp — CaptureWindow.cpp
// never sees an engine discovery type, only the plain Qt DiscoveryResult
// below. main.cpp's instance-guard check similarly only touches
// scanengine::InstanceGuard in one place. Written against
// engine/include/scanengine/discovery/discovery.h and
// engine/include/scanengine/core/instance_guard.h as shipped.
//
// Owner: C2 follow-up (auto-detect).
#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

namespace lidarscan {

// One flattened, Qt-friendly snapshot of a discovery pass. Built from
// scanengine::discovery's Result<>/optional<> types entirely on the worker
// thread so nothing but plain Qt values ever crosses back to the GUI thread.
struct Mid360Discovery {
  bool found = false;
  QString sn;
  QString fw_version;
  QString lidar_ip;
  QString netmask;
  QString gateway;
  QString persisted_host_ip;

  // CheckHostReachability(beacon) — only meaningful when found is true.
  bool host_ip_is_local = false;
  bool on_lidar_subnet = false;
  QStringList local_candidates;
  QString suggested_host_ip;
  QString suggested_interface;  // "en7" — where to add the alias, when known
  QString host_check_note;
};

struct D6Discovery {
  bool found = false;
  QString port;
  int packets_ok = 0;
  int packets_bad_checksum = 0;
};

struct Um982Discovery {
  bool found = false;
  QString port;
  int baud = 0;
  bool has_heading = false;
  int sentences_ok = 0;
};

struct DiscoveryResult {
  Mid360Discovery mid360;
  D6Discovery d6;
  Um982Discovery um982;
  // Non-empty only if the Mid-360 discovery call itself failed (not the same
  // as "nothing found" — that is mid360.found == false with this empty).
  QString mid360_error;
};

// Runs one full discovery pass synchronously: Mid-360 heartbeat listen, then
// the D6 and UM982 serial probes against scanengine::discovery::
// EnumerateSerialPorts(). Blocking (several seconds) — call only off the GUI
// thread, i.e. from DiscoveryWorker::run().
DiscoveryResult runDiscoveryBlocking(int mid360_timeout_ms, int serial_probe_ms);

// Thin QObject wrapper: CaptureWindow moves one of these onto a throwaway
// QThread (the standard Qt worker-object pattern) and connects
// started -> run, finished -> its own result handler, finished ->
// thread->quit, thread->finished -> both deleteLater. `phase` fires once
// before each stage so the progress dialog can show
// "Listening for Mid-360 heartbeat…" / "Probing serial ports…" against what
// is actually happening rather than a canned timer.
class DiscoveryWorker : public QObject {
  Q_OBJECT
 public:
  DiscoveryWorker(int mid360_timeout_ms, int serial_probe_ms, QObject* parent = nullptr);

 Q_SIGNALS:
  void phase(const QString& label);
  void finished(lidarscan::DiscoveryResult result);

 public Q_SLOTS:
  void run();

 private:
  int mid360_timeout_ms_;
  int serial_probe_ms_;
};

}  // namespace lidarscan

Q_DECLARE_METATYPE(lidarscan::DiscoveryResult)
