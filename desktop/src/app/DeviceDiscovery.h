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
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QWaitCondition>

#include <memory>

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

// A17. The SDK **v1** broadcast on UDP 55000, which is a different protocol on
// a different port from the Mid-360's SDK2 heartbeat on 56201 — two parsers,
// two listens, one gate (see the DiscoveryGate comment below).
struct Mid70Discovery {
  bool found = false;
  QString broadcast_code;   // "3GGDJ5N00100101" — the ONLY name SDK v1 accepts
  int dev_type = 0;         // livox_def.h DeviceType; 6 = Mid-70
  QString dev_type_name;    // "Mid-70", "Horizon", "unknown (9)"
  // The lidar's address. On real hardware this is the beacon's UDP SOURCE
  // address, not a field in the frame: the wire form is 34 bytes and carries no
  // ip text, and the Livox SDK fills the lidar's ip[] from the datagram source
  // exactly the same way (discovery.h, Mid70Beacon::lidar_ip). `source_ip` is
  // kept alongside so a diagnostic can still see the two separately.
  QString lidar_ip;
  QString source_ip;

  // Host reachability, derived the same way the Mid-360's is and reported in
  // the same fields — see discoverMid70() in the .cpp for why an SDK v1 beacon
  // can be handed to CheckHostReachability() honestly.
  bool host_ip_is_local = false;
  bool on_lidar_subnet = false;
  QStringList local_candidates;
  QString suggested_host_ip;
  QString suggested_interface;
  QString host_check_note;
};

struct D6Discovery {
  bool found = false;
  QString port;
  int packets_ok = 0;
  int packets_bad_checksum = 0;
};

// A18. The JuxiTech ICM-42670-P module, identified passively at 115200 by its
// `7E 23 …` framing. `frame_rate_hz` counts func-0x04 RAW frames only, which is
// the IMU SAMPLE rate — the number that says whether the module is still in its
// 25 Hz power-on default or has been put into 100 Hz mode. The probe never
// writes, so it cannot change that; ImuSerialDriver::start() does, once.
struct JuxiImuDiscovery {
  bool found = false;
  QString port;
  double frame_rate_hz = 0.0;
  int frames_seen = 0;   // checksum-valid frames of every func
  int raw_frames = 0;    // ...of which func 0x04
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
  Mid70Discovery mid70;
  D6Discovery d6;
  JuxiImuDiscovery juxi_imu;
  Um982Discovery um982;
  // Non-empty only if the Mid-360 discovery call itself failed (not the same
  // as "nothing found" — that is mid360.found == false with this empty).
  QString mid360_error;
  // Same meaning for the Mid-70 listen. Note that kOk + not found is ALSO what
  // "something is already connected to the lidar" looks like: a Mid-70 stops
  // broadcasting the moment a client attaches (discovery.h, DiscoverMid70).
  QString mid70_error;
  // The pass was cut short by DiscoveryGate::cancelAndWaitForSockets() (a
  // device session needed UDP 56201). Everything else in this struct is
  // partial and MUST NOT be applied to the UI — "not seen" here means "not
  // looked for", which is a different sentence entirely.
  bool canceled = false;
};

// ===========================================================================
// DiscoveryGate — the cancel token engine/…/discovery.h does not have
// ===========================================================================
//
// THE REGRESSION THIS EXISTS FOR (real hardware, 2026-08-17; NOTES.md §16.7).
// scanengine::discovery::DiscoverMid360() binds UDP 56201 (any-bound, so the
// broadcast heartbeat reaches it) and blocks for the WHOLE timeout it was
// given. The Livox SDK's push channel must bind 56201 too, and on macOS/BSD a
// second bind of the same addr:port succeeds only when EVERY socket holding
// it set SO_REUSEPORT — which discovery does (net_compat.h) and the vendored
// SDK does not. So a discovery pass in flight makes the next SdkInit fail with
// a raw `bind failed`, which surfaces as `device 1: idle -> fault I/O error`.
// Discovery is not wrong and the driver is not wrong; they must simply never
// hold the port at the same time.
//
// A17 — THE GATE COVERS THE MID-70 LISTEN TOO, and the ports are not the
// reason. DiscoverMid70() binds UDP **55000**, which is a different port from
// the Mid-360's 56201, so the two listens do not collide with each other. What
// they collide with is the SAME thing: the moment a Mid-70 is armed, the
// vendored Livox **SDK v1** inside Mid70Driver binds 55000 itself (its own
// device_discovery.h names it: `static const uint16_t kListenPort = 55000`) to
// hear the broadcast it needs for the handshake — and SDK v1 no more sets
// SO_REUSEPORT than SDK2 does. So arming a Mid-70 while a Mid-70 listen is in
// flight reproduces the §16.7 failure exactly, on a different port number.
// One gate therefore guards BOTH listens, and the contract it enforces is the
// same one in both cases: provably closed, not merely asked to stop, before
// anything arms a device. Mixing the two lidar models makes that stronger, not
// weaker — an operator who arms a Mid-360 must also have released 55000,
// because the next thing they do may well be to arm the Mid-70 instead.
//
// discovery.h ships no cancellation and engine/** is read-only here, so the
// worker does NOT call DiscoverMid360 once for the full timeout. It calls it
// in kChunkMs slices inside a loop that consults this gate before every slice.
// cancelAndWaitForSockets() then blocks its caller until the slice in flight
// has RETURNED — i.e. until the socket is provably closed, not merely until a
// flag was set. A canceled-but-still-bound socket reproduces the fault exactly
// as a non-canceled one does, so "asked it to stop" is not a safe handoff;
// "it stopped" is. Cancel latency is bounded by one slice (~1 s).
//
// THREADING: every method is safe from any thread; the begin/end pair is meant
// for the worker thread and cancelAndWaitForSockets() for the GUI thread. The
// gate outlives the worker (both hold a shared_ptr), so a GUI-side cancel is
// safe even against a worker that is already tearing itself down.
class DiscoveryGate {
 public:
  // One listen slice. 1000 ms is a whole beacon period (the heartbeat is
  // 1 Hz), so a slice is still a meaningful window, and it caps the worst-case
  // wait a device start pays for a discovery pass that must get out of the way.
  static constexpr int kChunkMs = 1000;

  // --- GUI-thread side ----------------------------------------------------
  // Ask the worker to stop and BLOCK until it holds no UDP port (or wait_ms
  // expires). True means the port is provably free and it is safe to arm a
  // device; false means the caller should say so rather than pretend.
  bool cancelAndWaitForSockets(int wait_ms);
  bool wasCanceled() const;

  // --- worker-thread side -------------------------------------------------
  // False => canceled; do not bind, leave the loop. The check and the "I am
  // bound" publication happen under one lock, so a cancel can never slip
  // between them and observe a free port that is about to be taken.
  bool beginUdpSlice();
  void endUdpSlice();
  void markFinished();

 private:
  mutable QMutex mutex_;
  QWaitCondition released_;
  bool cancel_ = false;
  bool udp_bound_ = false;
  bool finished_ = false;
};

// Runs one full discovery pass synchronously, in this order:
//   1. Mid-360 SDK2 heartbeat listen  (UDP 56201)
//   2. Mid-70  SDK v1 broadcast listen (UDP 55000)
//   3. the serial sweep — D6, then the JuxiTech IMU, then the UM982
// The UDP listens are sequential rather than concurrent because the gate's
// contract is "one slice in flight, and cancel waits for it"; two overlapping
// listens would make "provably closed" mean two things at once.
//
// The serial ORDER is discovery.h's own contract, not a preference:
// ProbeSerialJuxiImu must run after ProbeSerialD6 (the D6 probe is the only one
// that may ever write, and a D6 identified passively never reaches the IMU
// probe) and before ProbeSerialUm982 (whose five-baud sweep would otherwise
// spend six seconds failing to read an IMU module). The desktop does not run
// ProbeSerialStl27l at all — it never has; the STL-27L, like the D6, is a phone
// sensor here — so the "after the STL-27L" half of that contract is vacuous on
// this path rather than violated.
//
// Blocking (several seconds) — call only off the GUI thread, i.e. from
// DiscoveryWorker::run().
DiscoveryResult runDiscoveryBlocking(int mid360_timeout_ms, int mid70_timeout_ms,
                                     int serial_probe_ms);

// Thin QObject wrapper: CaptureWindow moves one of these onto a throwaway
// QThread (the standard Qt worker-object pattern) and connects
// started -> run, finished -> its own result handler, finished ->
// thread->quit, thread->finished -> both deleteLater. `phase` fires once
// before each stage so the progress dialog can show
// "Listening for Mid-360 heartbeat…" / "Listening for a Mid-70 broadcast…" /
// "Probing serial ports…" against what is actually happening rather than a
// canned timer.
class DiscoveryWorker : public QObject {
  Q_OBJECT
 public:
  DiscoveryWorker(int mid360_timeout_ms, int mid70_timeout_ms, int serial_probe_ms,
                  QObject* parent = nullptr);

  // The gate this worker consults. Held by shared_ptr precisely so the GUI
  // side can keep cancelling against it after the worker has deleteLater'd
  // itself on its own thread.
  std::shared_ptr<DiscoveryGate> gate() const { return gate_; }

 Q_SIGNALS:
  void phase(const QString& label);
  void finished(lidarscan::DiscoveryResult result);

 public Q_SLOTS:
  void run();

 private:
  int mid360_timeout_ms_;
  int mid70_timeout_ms_;
  int serial_probe_ms_;
  std::shared_ptr<DiscoveryGate> gate_;
};

}  // namespace lidarscan

Q_DECLARE_METATYPE(lidarscan::DiscoveryResult)
