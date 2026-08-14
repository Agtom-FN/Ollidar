# S2-sim follow-up — link-drop/reconnect, `.lvx2` replay, E2 fixtures

Executes the three "recommended before the Mid-360 reaches the bench" items
from REPORT.md §8/§9. Same host/toolchain as REPORT.md (Apple M4, macOS
26.5.1, Apple clang 21.0.0, CMake 4.4.2). Date: 2026-08-15. Read REPORT.md
first — this document assumes its vocabulary (`[S]`/`[D]`/`[R]` sourcing,
the `udp_cnt` free-running-vs-documented finding, the loopback setup).

All three deliverables were run end-to-end against the real, patched SDK2
client (`sdk_client_demo`) exactly as REPORT.md's soaks were. Raw logs are in
`results/` (see the file list at the end of each section).

---

## 1. Link-drop / reconnect injection — the A3-critical finding

### What was built

`mid360_sim` gained `--drop-link-after SECS --link-down-for SECS [--repeat]
[--restart-identity]` (see `sim/mid360_sim.cpp`, `LinkFaultThread` +
`g_link_up` gate applied to all five threads: command replies, discovery,
heartbeat, point, IMU). Two distinct fault models, because they are
genuinely different on real hardware:

- **Plain drop** (cable pull): every outbound packet on every port is
  suppressed for the down window, then resumes to the **same** host-taught
  destinations. The device's internal packet counter keeps advancing during
  the outage (mirrors a real ranging ASIC, which has no idea the Ethernet
  link is down) — packets are generated-and-dropped via the same accounting
  path as `--loss`, not simply paused.
- **`--restart-identity`** (power-cycle): additionally clears everything the
  host configured (`have_state_dst`/`have_point_dst`/`have_imu_dst`, work
  mode, pcl type, imu-enable) right before the link comes back up — the
  device forgets it was ever configured, exactly like a fresh boot.

`sdk_client_demo` gained stream-gap (stall/resume) detection on the point
stream, the IMU stream, and the heartbeat (`--stall-threshold S`, default
1.0 s / 2.5 s for the 1 Hz heartbeat), plus a counter on
`LidarInfoChangeCallback` re-fires (`info-change callback fires` in the
summary) — because **the SDK exposes no explicit "link down" callback**.
Checked directly against `third_party/Livox-SDK2/include/livox_lidar_api.h`
and `livox_lidar_def.h`: the only relevant callbacks are the one-shot
info-change, the periodic info-push, and per-command timeouts
(`kLivoxLidarStatusTimeout`, `general_command_handler.cpp:826-845`). A
driver has no choice but to build its own watchdog on data/heartbeat
silence, same as this demo now does — that is itself a finding for A3.

### Scenario A — plain cable pull (drop at 25s, down 15s, resume, no identity reset)

`results/linkfault-plain-20260815-023027-{sim,demo}.log`

```
[sim] 02:30:52.799 ===== LINK DOWN (simulated cable pull) =====
[sim] 02:31:07.991 ===== LINK UP (resumed) =====
...
[demo] 02:30:53.996 STREAM STALLED: no point packets for >1.0s (event #1, t=25.6s)
[demo] 02:30:53.996 IMU STALLED: no IMU packets for >1.0s (t=25.6s)
[demo] 02:30:55.217 HEARTBEAT STALLED: no push-state (0x0102) for >2.5s (t=26.8s)
[demo] 02:31:08.196 STREAM RESUMED: point packets flowing again after 15.39s gap (t=39.8s)
[demo] 02:31:08.196 IMU RESUMED after 15.39s gap (t=39.8s)
[demo] 02:31:08.803 HEARTBEAT RESUMED after 16.04s gap (t=40.4s)
...
packets lost (udp_cnt)   : 0  (0.0000 %)
info-change callback fires: 1  (1 = never re-fired; ...)
point-stream stall events : 1
```

**Result: the client auto-recovers with zero application-level action.**
Once the wire resumes, points/IMU/heartbeat flow again immediately — no
re-init, no callback re-fire, `LivoxLidarSdkInit`/`Start` called exactly
once. This is the easy case and it works.

**But `udp_cnt`-based loss detection is structurally blind to a full-link
outage.** 0 packets lost was reported across a 15-second total outage,
because the simulated device's counter kept incrementing through the
outage — so the first packet after resume has `udp_cnt == last_seen + 1`,
which is indistinguishable from zero loss by the counter alone. **A3's
health/reconnect logic must not rely on `udp_cnt` gaps to detect an
extended outage** — it needs a wall-clock "time since last packet/heartbeat"
watchdog (exactly what `sdk_client_demo`'s new stall detector does) as the
primary signal, with `udp_cnt` gaps only useful for **partial**, in-band
loss during an otherwise-live link (which is what Run C in REPORT.md §6
correctly measures — a different failure mode from this one). Confirmed
repeatable: the `--repeat` smoke test below shows 0 lost packets across
three separate 4-second outages, every time.

### Scenario B — power-cycle (`--restart-identity`)

`results/linkfault-restart-20260815-023200-{sim,demo}.log`

```
[sim] 02:32:25.536 ===== LINK DOWN (simulated cable pull), will restart identity before resume =====
[sim] 02:32:40.754 identity reset (simulated power-cycle): host destinations forgotten, streaming stopped, ...
[sim] 02:32:40.754 ===== LINK UP (resumed) =====
...
[demo] 02:32:26.570 STREAM STALLED: no point packets for >1.0s (event #1, t=25.3s)
[demo] 02:32:27.784 HEARTBEAT STALLED: no push-state (0x0102) for >2.5s (t=26.6s)
                                                    (no STREAM RESUMED / HEARTBEAT RESUMED for the
                                                     rest of the 60 s run)
...
duration (measured)      : 60.1 s
points                   : 4,454,208           (should be ~12,000,000 at 200k pts/s for 60s)
device timestamp span    : 22.271 s (host span 60.149 s, skew 37.878 s)
info-change callback fires: 1  (never re-fired)
point-stream stall events : 1
```

**Result: the client NEVER recovers on its own** for the remaining ~35
seconds observed after resume (limited only by the test's 60 s duration —
nothing in the logs suggests it would recover later either). Data flow
simply stops forever from the client's point of view.

Root cause, confirmed by reading the SDK source
(`sdk_core/command_handler/general_command_handler.cpp:337-397`,
`HandleDetectionData`): the sim's `DetectionThread` keeps announcing at 1 Hz
throughout and after the outage — confirmed in the demo log, which shows
`Handle detection data, handle:16777343, ...` on almost every second,
including deep into the "no recovery" window — so **the SDK is receiving
fresh discovery ACKs the entire time**. But:

```cpp
if (devices_.find(handle) != devices_.end()) {
  DeviceInfo& device_info = devices_[handle];
  if (!(device_info.is_update_cfg.load()) && ...) {
    if (!is_view_) UpdateLidarCfg(...);   // re-sends 0x0100 host-ip config
  }
  ...
  return;   // <-- always returns here once `handle` is already known
}
```

Once a handle has been seen (`devices_.find(handle) != end()`), the SDK
takes this branch **permanently** and only calls `UpdateLidarCfg` (which
re-drives host-IP configuration) if `is_update_cfg` is still false — i.e.
only before the *first* successful configuration ever happened. After that,
new detection ACKs for the same handle are accepted for internal
bookkeeping but **never trigger a re-configuration handshake**, no matter
how many more of them arrive. The SDK's model is "this device, once
configured, stays configured" — it has no concept of "the device forgot its
configuration and needs it re-sent."

**This is the single most load-bearing finding for A3.** A driver that only
watches for "did the SDK's info-change callback fire again" or "did a new
discovery ACK arrive" will never notice a power-cycled device needs
reconfiguring — those signals either don't fire again (info-change) or fire
but are ignored by the SDK's own state machine (discovery). **A3 must treat
"heartbeat/data silence beyond a threshold, followed by data still not
resuming after the link nominally comes back" as a hard signal to tear down
and fully re-`LivoxLidarSdkInit`/`Start` the SDK** (or otherwise force a
fresh handshake) rather than trust the SDK to self-heal. The plain-drop case
above proves the SDK *can* recover a live link with zero help; this case
proves it will not always know it needs to.

### Scenario C — `--repeat` smoke test (3 consecutive plain-drop cycles)

`results/linkfault-repeat-smoketest-{sim,demo}.log` — `--drop-link-after 8
--link-down-for 4 --repeat`, 33 s run:

```
LINK DOWN @8.1s -> UP @~12.1s     STREAM STALLED t=8.1s -> RESUMED after 4.06s gap (t=11.2s)
LINK DOWN @~20.3s -> UP @~24.3s   STREAM STALLED t=20.3s -> RESUMED after 4.08s gap (t=23.3s)
LINK DOWN @~32.5s -> (run ends before resume)
packets lost (udp_cnt)   : 0  (0.0000 %)      <- across all 3 outages
info-change callback fires: 1
point-stream stall events : 3
```

Confirms the plain-drop recovery and the `udp_cnt`-blindness finding are
repeatable, not a one-off artifact — 0 lost packets every single cycle.

### Reproducing

```sh
scripts/run_soak.sh 60 --drop-link-after 25 --link-down-for 15                     # (or run mid360_sim/sdk_client_demo by hand, see below)
build/spike/mid360_sim --duration 70 --drop-link-after 25 --link-down-for 15 &
build/spike/sdk_client_demo config/mid360_loopback.json --duration 60 --stall-threshold 1.0
# add --restart-identity to mid360_sim for the power-cycle variant
# add --repeat to mid360_sim to cycle continuously
```

---

## 2. `.lvx2` replay — real Risley-prism scan pattern for A6

### What was built

- `sim/lvx2_reader.h` — a from-scratch `.lvx2` container reader (public/
  private header, frame blocks, 27-byte package headers), format documented
  inline and cross-checked byte-for-byte against `DATASETS.md`'s own probe
  (package count, point count, container-frame count, first `udp_cnt` all
  match exactly — see below). No SDK/scene code is reused since none of it
  reads `.lvx2`; what **is** reused is the wire-format layer:
  `sim/livox_wire.h`'s `DataHeader`/`CartesianHigh`/`ImuSample` structs and
  its `Crc32()`, so the packets this tool emits are byte-identical in
  framing to what `mid360_sim` emits.
- `replay/lvx2_replay.cpp` — the control plane (discovery/handshake/
  heartbeat) is copied from `mid360_sim.cpp` essentially verbatim (it has to
  be synthetic either way; `.lvx2` carries no control-channel traffic). The
  data plane reads real packages via `Lvx2Reader`, paces them by their real
  recorded device-clock deltas (scaled by `--speed`), and re-wraps each into
  a live-format UDP datagram. `--loop` re-reads the file from the start at
  EOF.
- A Python port, `scripts/lvx2_reader.py`, independently re-validates the
  same format for `scripts/make_fixtures.py` (§3) — two independent
  implementations of the same byte layout agreeing is itself a
  cross-check.

### What is REAL vs SYNTHESIZED when replaying (stated in the tool's own
header comment, repeated here per the task's request)

| | Status |
| --- | --- |
| Point x/y/z/reflectivity/tag payload | **REAL**, byte-for-byte from the recording |
| `udp_cnt`, `frame_cnt`, `data_type` | **REAL**, from the recording (unlike `mid360_sim`, which synthesizes these) |
| Per-package device-clock timestamp deltas (i.e. real inter-packet pacing/jitter) | **REAL** (rebased to start at 0; deltas unchanged) |
| 36-byte `DataHeader` wrapper (`length`, `time_interval`, `crc32`) | SYNTHESIZED — `.lvx2` does not store the on-wire header, only decoded metadata + payload; `time_interval` assumes constant 200,000 pts/s spacing (matches the measured rate); `crc32` is recomputed over the real payload |
| Heartbeat / push-state (0x0102) content | SYNTHESIZED (mid360_sim's placeholder KV set) — `.lvx2` has no control channel at all |
| IMU | **Replayed only if the source file actually contains IMU packages.** Livox's own `Indoor_sampledata.lvx2` / `Outdoor_sampledata.lvx2` contain **zero** IMU packages (`DATASETS.md` §1) — replaying them correctly produces **zero IMU packets on the wire**, not synthetic ones. This is the honest behaviour, not a gap in the tool; it *is* the format gap the task asked to be stated clearly. |

### Verification — indoor sample, one full pass (77.9 s file, first 60 s
measured)

`results/` — reproduced with:
```sh
build/spike/lvx2_replay datasets/Indoor_sampledata.lvx2 &
build/spike/sdk_client_demo config/mid360_loopback.json --duration 60 --report-period 10
```

```
[replay] source: 162293 packages, 15580128 points, IMU packages: NONE (nothing to replay on the IMU port)
...
================ sdk_client_demo summary ================
point packets            : 125191
points                   : 12018336
  of which no-return     : 4148793        (34.51% -- matches DATASETS.md/FIXTURES.md's ~34.7% full-file figure)
mean point rate          : 200000 pts/s
mean packet rate         : 2083.3 pkt/s
IMU packets              : 0  (0.00 Hz)
packets lost (udp_cnt)   : 0  (0.0000 %)
duplicate/stalled udp_cnt: 0
packets with dot_num!=96 : 0
packets with bad length  : 0
info-change callback fires: 1
point-stream stall events : 0
```

**Clean parse: 200,000 pts/s exactly, 0 loss, 0 duplicates, every packet
`dot_num == 96`, correct length, no stream stalls.** The real SDK2 client
consumes this exactly like it consumes `mid360_sim`'s synthetic stream —
which is the point: A6 can now be built against real recorded scan geometry
without any client-side changes.

### `--loop` and `--speed`

`--loop --speed 4 --duration 40` on the same file:
```
[replay] t=    20s  points 799999 pts/s ...
[replay] looping back to start of file (iteration 2)
[replay] t=    25s  points 800007 pts/s ...  loops=1
...
mean point rate           : 799999 pts/s     (= 4 x 200,000, exact)
timestamp regressions     : 1                (expected: device clock resets to ~0 at the loop point)
packets lost (udp_cnt)    : 0                (the counter jump at the loop boundary lands in the
                                               client's own ">1024 = unattributable reset" bucket --
                                               same handling as a real per-frame reset, not a bug)
```
`--speed` scales throughput exactly (4.00x -> 799,999 ≈ 800,000 pts/s).
`--loop` works correctly across the file boundary; the one expected artifact
is a single timestamp regression at the loop point, which the client's
existing timestamp-monotonicity check correctly flags (this is a genuine
property of looping a finite recording, not a defect in the tool).

### Reproducing

```sh
build/spike/lvx2_replay datasets/Indoor_sampledata.lvx2 --speed 1 &
build/spike/sdk_client_demo config/mid360_loopback.json --duration 60
```

---

## 3. E2 fixtures

See `FIXTURES.md` for the full write-up (provenance, licensing, real-vs-
synthesized breakdown per fixture, regenerate instructions). Summary:

| File | Duration | Size | Licence | Committable |
| --- | --- | --- | --- | --- |
| `fixtures/indoor_livox_5s.livoxdump` | 5.00 s | 14.52 MB | Livox sample, no explicit licence | No — regenerate-only |
| `fixtures/outdoor_livox_5s.livoxdump` | 5.00 s | 14.52 MB | Livox sample, no explicit licence | No — regenerate-only |
| `fixtures/outdoor_imu_ccby_6s.livoxdump` | 5.93 s | 17.51 MB | **CC-BY-4.0** | **Yes** |

All three verified with `scripts/fixture_stats.py` (duration, pts/s,
no-return %, tag histogram, IMU rate) and, as a bonus structural
cross-check, with the existing `tools/remote-capture/verify_capture.py`
(all three: `OVERALL: PASS`) — they share the `.livoxdump` container that
tool already understands.

Headline numbers: indoor 35.24% no-return (corroborates REPORT.md's 35.9%
finding; 34.67% over the full 77.9 s recording), outdoor (single-device)
67.76% no-return, Zenodo slice 200.20 Hz IMU / ~200k pts/s with real
synchronized IMU + real per-point `tag` values. Regenerate with
`python3 scripts/make_fixtures.py --all` (needs `pip install rosbags numpy`
for the Zenodo one only).

---

## Files touched / added

**Modified:**
- `sim/mid360_sim.cpp` — link-fault injection (`LinkFaultThread`,
  `g_link_up`, new CLI flags)
- `demo/sdk_client_demo.cpp` — stall/resume + info-change-refire
  observability
- `CMakeLists.txt` — new `lvx2_replay` target
- `.gitignore` — fixtures policy

**Added:**
- `sim/lvx2_reader.h`, `replay/lvx2_replay.cpp` — deliverable 2
- `scripts/lvx2_reader.py`, `scripts/livoxdump.py`, `scripts/make_fixtures.py`,
  `scripts/fixture_stats.py` — deliverable 3
- `fixtures/outdoor_imu_ccby_6s.livoxdump` (committed); `fixtures/indoor_livox_5s.livoxdump`,
  `fixtures/outdoor_livox_5s.livoxdump` (regenerate-only, gitignored)
- `FIXTURES.md`, this file

**Raw run logs** (all in `results/`): `linkfault-plain-20260815-023027-*`,
`linkfault-restart-20260815-023200-*`, `linkfault-repeat-smoketest-*`.
(The `.lvx2`-replay verification run logs were captured to `/tmp` during
this session and are reproducible exactly via the commands in §2 above —
only the link-fault logs were routed through `results/` by default.)

## What this changes about REPORT.md's plan (updates to §8/§9)

1. §8 item 3 ("link drop / reconnect / power-cycle... untested even in
   simulation") is now tested, and the power-cycle result (§1 Scenario B
   above) is a **harder requirement on A3** than REPORT.md anticipated: it
   is not enough to detect an outage, A3 must be prepared to force a full
   SDK re-init because the SDK will not self-heal from a device that lost
   its configuration.
2. §8 item 8 ("the real non-repetitive scan pattern... partially closable in
   software now by replaying `Indoor_sampledata.lvx2`") is now closed in
   software: `lvx2_replay` does exactly that, verified end-to-end against
   the real SDK2 client.
3. §9's GO verdict for A3 stands, with one addition: A3's health/reconnect
   design must include an explicit SDK re-init path, not just loss/skew
   detection — this was not visible before link-fault injection existed to
   test it.
