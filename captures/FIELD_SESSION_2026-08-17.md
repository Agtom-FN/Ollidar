# First hardware session — 2026-08-17, via SSH to kc-m4s-mac-mini (Tailscale)

## COIN-D6 (/dev/cu.usbserial-21130)
- Checksum variant: **VENDOR** (2430/2430 vs spec 143) — S1 question CLOSED
- 4,025 pts/s @ 9.99 Hz live; 100.0000% checksum pass — S1 exit criterion CLOSED
- Start packets: one per revolution; scan-freq field unit = 0.1 Hz (100 → 10 Hz)
- **Range noise σ = 5.2 mm median ≤2 m** (223 buckets) → S6 D6-colorization verdict: **GO**
- ⚠ Stream stalled twice mid-capture (~50% duty) — suspect USB 5V sag under motor
  load through the CH340 adapter; retest with powered hub. No stop-ACK observed.
- Capture promoted to engine/tests/integration/data/field_d6_30s.bin

## Livox Mid-360 (SN MCP7K0034759, fw 35010108, IP 192.168.1.159)
- 60 s SDK2 soak: **199,999 pts/s, IMU 200.00 Hz, 0 lost, skew 0.000 s** — S2
  real-hardware leg CLOSED; frame_cnt=0 / free-running udp_cnt CONFIRMED on hw
- 30 s raw capture → .lscan → worker --post → cloud.ply (28,684 pts) — full
  pipeline proven on real data
- Network notes: host route needed (`route add -host 192.168.1.159 -interface en7`),
  and SDK requires host alias matching lidar's persisted host (192.168.1.5 here);
  lidar ignores ICMP ping; heartbeat broadcasts reach any-bound sockets only.

## Still to test on hardware: UM982 (cu.usbmodem2111101 candidate), Android on
Pixel 8 Pro, D6 powered-hub retest, mount rig + calibration.

## Unicore UM982 (/dev/cu.usbserial-21140 @ 230400 — NOT the 115200 default)
- 7 NMEA types @ 1 Hz incl. GPTHS (dual-antenna heading ENABLED in fw); 210/210 checksums
- Fix 0, 0 SVs — antennas not connected / indoors; link+protocol path PROVEN (S5 bench leg
  closed at maximum-achievable-indoors); needs antennas + sky for Single, NTRIP for RTK
- /dev/cu.usbmodem2111101 = unrelated ESP32 (agri-IoT water-flow logger) on same Mac

## Session 2 — NTRIP + app verification
- **SatRef NTRIP WORKING with owner account FNF8838**: ntrip.geodetic.gov.hk:2101,
  ICY 200 OK, 20+ station mountpoints (RTCM3.2 MSM5, all constellations);
  71/71 frames CRC-valid from HKSC_32 (Stonecutters). 82.6 KB of live corrections
  bridged into the UM982 serial port; device ingested cleanly (NMEA uninterrupted,
  100% checksums). RTK Fixed pending ONLY antennas + sky.
- **macOS app VERIFIED on kc-m4 vs real Mid-360**: --mid360-selftest PASSED,
  first packet 0.70 s (faster than sim's 1.6 s). App installed to ~/Applications.
  Gotcha logged: a first attempt left a port-holding process; later runs failed
  SdkInit until killed — single-instance guard worth considering.
- App RTK defaults to prefill later: host ntrip.geodetic.gov.hk:2101, HK station
  mountpoints (nearest-station picker), UM982 @ 230400.

## Session 3 — D6 root cause SOLVED: the CH340 adapter
Timeline: owner powered D6 from external 5 V → stream garbled (110 KB garbage, 0 packets).
Ground bonding improved it to framed-but-corrupt (AB 55 for AA 55, 82% 1-bits — zeros
read as ones = RX low level marginal). Restoring original wiring did NOT fix it; baud
sweep (57.6k–921.6k) ruled out clock shift; reseat + different Mac USB port ruled out
connector and host. **Swapping the USB-serial adapter fixed everything instantly**:
- 30 s: 100.0000% checksum, 5,281 packets, 0 bad
- 180 s soak: 100.0000% checksum, 30,727 packets, 0 bad, 10.00 Hz steady,
  4,000 pts/s, full duty — **the original ~50% duty stalls are gone too.**
Verdict: the vendor-kit CH340 adapter was flaky from day one (stalls) and died fully
during the rewiring. Owner was right that D6 current draw (240 mA typ) was never the
problem. D6 is now UNCONDITIONALLY CLEARED: capture path meets S1 at full duty.
D6 now at /dev/cu.usbserial-21120 (new adapter); old adapter on -21130 should be
unplugged; UM982 (-21140) not enumerated at session end — likely unplugged during swap.
Evidence: captures/d6_soak_180s.bin (2.5 MB, 100% pass on replay).

## Session 3 (cont.) — auto-detect shipped + full-chain verification
- Owner requirement (GUI defeated by manual IP entry) answered same-day: device
  auto-detect landed in engine (A16), desktop, Android. On the field Mac the
  shipping app found BOTH sensors from cold with zero typed config:
  "Found Mid-360 SN ARMCP7K0034759, fw 35010108, at 192.168.1.159" +
  "COIN-D6: found on /dev/cu.usbserial-21120" (evidence:
  desktop/evidence/17-autodetect-real-hardware.png).
- Regression caught before the owner saw it: silent on-open discovery holds UDP
  56201; fixed by both-ways serialization + CLI chaining. Field 'bind failed' was
  ALSO the en7 alias having evaporated during USB re-plugging (EADDRNOTAVAIL is
  indistinguishable in the SDK log — it discards errno).
- Network made PERMANENT: en7 service manual 192.168.1.5/255.255.255.0 via
  networksetup (survives reboot/replug; replaces the fragile route+alias recipe).
- Final chained verification on the shipping app: suppressed silent pass →
  auto-detect FOUND Mid-360 + D6 → 56201 released → mid360-selftest PASSED
  (first packet 0.62 s) → real 3 s recording: 6,733 chunks / 8.5 MB / lidar+IMU
  streams / sealed=true. macOS app fully verified on real hardware.
- TestPack refreshed (new DMG + new APK with auto-detect); app reinstalled to
  ~/Applications on kc-m4.

## Session 3 (cont.) — round-5 capture redesign deployed
- Rounds 5-5.3 (owner-approved, REVIEW_FEEDBACK items 7-18) implemented in both apps
  and committed (a282d66): 2-step popup-free capture, inline auto-detect w/ self-opening
  manual fallback, auto-armed live preview, auto-named projects, settings carry
  preview->recording, D6 phone-only 3D (AR pose-pump bug found+fixed — would have
  recorded 2D), Processing/Merge folded into Projects, phone-GPS georef fallback,
  hw-derived refresh ceiling + downshift, walkthrough-first kit. Engine point-size
  floor 0.1px (suite 2,280,270 assertions green). Android: 299 unit + 4/4 emulator.
- Round-5 DMG deployed to kc-m4 ~/Applications; CLI verification of the new build:
  suppression, chaining, D6-is-phone-only line, IOKit sleep inhibition all correct.
  Live Mid-360 pass BLOCKED at session end: en7 reports "status: inactive" — the
  lidar's Ethernet/power physically disconnected during hardware shuffling (UM982
  also still unplugged). Rerun the chained selftest once cables are back.
- TestPack rezipped with round-5 DMG + APK.

## Session 3 (close) — round-5 build verified on ALL THREE sensors
- UM982 renumbered to /dev/cu.usbserial-21130 after replug. App auto-detect initially
  missed it → root cause: 150 ms per-baud dwell vs a 1 Hz NMEA burst (mostly silence).
  Fix: dwell >= 1100 ms (one period + margin) + silent-port fast-path (no bytes in the
  first full period => skip remaining bauds). Suite still 2,280,270 assertions green.
- FINAL round-5 verification on kc-m4, shipping app, one invocation:
  "auto-detect: Mid-360 FOUND, D6 FOUND, UM982 FOUND (@230400, 7 sentences, heading yes)"
  -> chained selftest PASSED (first packet 0.52 s) -> 3 s recording, 7,226 chunks /
  8.9 MB, 0 drops, sealed. en7 permanent config SURVIVED the replug (networksetup fix
  proven). TestPack rezipped with the final DMG.

## Owner field bug report vs round-5 desktop capture (GUI use on kc-m4)
Verbatim: "wrongly detect me walking and moving while i am stay still and its not
recording and changing from live while there are moving. i notice that it only
record when the first connected."
Read as: (A) motion hint false-positives while stationary (pose-jitter walk-speed);
(B) live view stops following real changes (suspect downshift w/o recovery or motion
state gating renders); (C) recording works only on the FIRST Start after connect —
second Start after Stop/seal records nothing (session not re-armable).
Fix wave dispatched (desktop Opus repro-vs-sim + --record-cycles hook; Android audit
for the same modes). Hardware re-verification pending.

## Owner field report #2 — Pixel 8 Pro + D6 real walk test
- ARCore path tracking on the phone: GOOD (trail follows the real walk).
- D6 scan result BAD: fan slices collapse into a single vertical plane ("captures the
  plane of Z-axis instead of XY") — walk motion not extruding the fan into 3D.
  Suspects (dispatched to Android agent): ARCore Y-up vs engine world-frame axis
  conversion, pose timestamps not pairing (domain mismatch), nominal phone-back
  extrinsic mapping fan into wrong plane, or live view rendering raw fan-frame points.
  Synthetic straight-line-walk pushbroom test required to pin + prevent regression.
- Owner field item #3 (same Pixel session): AR overlay shows NO camera feed. Suspects:
  ArPosePumpView holding the ARCore session/camera when the real overlay opens (needs
  yield/handoff — one camera consumer at a time), camera permission request path lost in
  the round-5 "no dialog on tab entry" fix, or GLSurfaceView Z-order regression on the
  stacked surfaces. Trail worked → ARCore tracked; this is plumbing, not ARCore init.

## Session 3 (final) — v0.2.1: both field-bug waves fixed and hardware-verified
- Versioning adopted (owner rule): repo VERSION file drives both apps; artifacts named
  LidarScan-0.2.1-201.apk / LidarScan-0.2.1-universal.dmg; version in Settings footer
  (Android), title/--version/status bar (desktop).
- Android wave (6 bugs): RigMotion dt==0 permanent-invalid, stale EXCESSIVE_MOTION across
  pause/resume, 2nd Start reused project 1, governor recovery x2, AR camera session-
  ownership race (RendererOwner), LIVE MAP chip mislabeling raw fallback. 306+13 unit,
  4/4 emulator. D6 flat-plane: pose→pushbroom chain PROVEN correct synthetically; awaiting
  owner re-walk w/ fixed live labeling + Process of the original project; physical bracket
  orientation unchecked.
- Desktop wave: WalkSpeedEstimator + IMU MotionGate (live LIO drifts — pose-only speed
  unfixable; IMU gravity-check suppresses honestly), governor bidirectional + no longer
  persists its own output, recording re-arms device in place (empty-seal-while-REC fixed).
- REAL-HARDWARE verification (kc-m4, Mid-360, v0.2.1): --record-cycles 4/4 PASS on one
  connect; --walk-soak 60 s peak 0.000 m/s hint 0x PASS — soak log captured live LIO
  drift (0.25 m/s implied, sigma 1.449 m, quality 2) with IMU stationary => ENGINE
  FOLLOW-UP: stationary-scene LIO tuning (iVox/ESKF) worth a look before field runs.

## Joint GUI session (owner at kc-m4, logged via /tmp/gui.log) — live-view freeze SOLVED
- Owner ran Capture on fresh 0.2.1 w/ stderr logging: capture + auto-detect + recording
  all work; live view freezes at the moment it fills.
- ROOT CAUSE (proven, 1400 log hits): engine page store (64 x 1M pts) fills — live
  preview alone fills it pre-Start at 200k pts/s — then drops every new point forever.
  No eviction policy = display dead-ends; recording path unaffected (owner's data
  complete). Fix in flight: live-mode oldest-first page eviction + status seam.
- SECOND BUG (proven): GUI recorded into ~/Applications/LidarScan.app/Contents/MacOS/
  record-cycles/ — the --record-cycles evidence hook persisted its project root into
  QSettings. Owner data RESCUED to ~/Documents/LidarScan Projects (Scan-001 19-16 +
  Scan-011 20-54). Fix in flight: hooks never persist settings; root self-heals.
- Android relevance: 24-page phone cap hits the same dead-end sooner => likely the
  "barely mapping" symptom on the Pixel; insight relayed to the round-6 Android agent.
- Process rule adopted: no tests on kc-m4 while the owner is using it.

## v0.3.0 SHIPPED (2026-08-18 00:53)
- Android LidarScan-0.3.0-300.apk: data-loss fix (manifest collision + RECOVERY of lost
  captures), AR crash-proofing, D6 live map actually drawn + tiered budgets, presets,
  mount re-zero, capture log. 343+31 unit, 6/6 device.
- Desktop/engine LidarScan-0.3.0-universal.dmg: live-view freeze fixed for good — THREE
  mechanisms (no-eviction dead-end; per-stream page interleaving wasting 99.6% of the
  store — "64M" store really held ~283k pts, filled during preview; LOD budget keeping
  oldest pages). A/B falsifiable proofs in verify_round5.sh. ABI v7 additive. Record-root
  persistence fixed 4-deep. Engine 524 cases / 2,281,393 assertions green.
- DMG staged at /tmp/LidarScan-0.3.0-universal.dmg on kc-m4 (NOT installed — owner's app
  instance running; per the no-collision rule, install deferred to owner signal).
- Desktop 0.3.0 installed on kc-m4 (owner-authorized) + REAL-HW eviction proof:
  --live-map-soak 180 --live-store-pages 2 => full=yes recycling=yes, 281 windows past
  fill, 0 stalled / 0 dropped / newest always drawn, 6 pages (4.2M pts) recycled — PASS.
  Full-size store no longer fillable in a 2-min preview (interleaving fix confirmed live).
