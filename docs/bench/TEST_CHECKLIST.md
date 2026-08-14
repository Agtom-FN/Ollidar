# LidarScan — Bench Smoke-Test Checklist

Spike **S4** companion. Run this checklist after each fresh assembly or driver
change, per sensor, per host, to catch wiring/driver regressions before they eat
into S1/S2/S7 time. This checklist only needs enumeration + raw byte/packet
flow — no LidarScan app code is required to pass any item below (all steps use
OS tools or the vendor's own Windows host software).

Check off `[ ]` → `[x]` as items pass. Record the host (OS + device model) and
date for each run — this doc is meant to be re-run repeatedly, not filled in once.

---

## A. COIN-D6, per host

### A.1 macOS

- [ ] Plug in CH340 adapter → `ls /dev/tty.usb* /dev/cu.usb*` shows a new device
      node within ~2 s, no driver install needed (in-box on macOS 13+)
- [ ] Open the port at 230400 8N1 (e.g. `screen /dev/cu.usbserial-XXXX 230400` or
      a serial terminal app) and observe the device-info frame
      (`A5 5A 14 00 E3 02 01 ...` containing ASCII `COIN-D6`) arriving
      unsolicited within ~1 s of power-up, before sending anything
- [ ] Send start command `AA 55 F0 0F` → receive OK ack
      (`A5 5A 50 07 00 00 00 00 00 00 00 A8`)
- [ ] Data frames stream continuously after start; checksum field present per
      packet (validation itself is A2/S1 driver work, not required to pass this
      smoke item — just confirm bytes are flowing and packet headers `0x55AA`
      recur at a plausible rate)
- [ ] **Expected rate: ~4,000 pts/s, 10 Hz rotation** — at 230400 baud with the
      vendor's packet framing this should look like a steady, non-bursty byte
      stream (rough sanity check: 4,000 pts/s × ~3 bytes/sample + packet
      overhead is well under the 230400 bps ceiling, so no framing/backpressure
      is expected — a bursty or stalling stream is itself a fail signal)
- [ ] Send stop command `AA 55 F5 0A` → receive OK ack
      (`A5 5A 55 07 00 00 00 00 00 00 00 AD`), data stream ceases

### A.2 Windows

- [ ] Device Manager → Ports (COM & LPT) shows **USB-SERIAL CH340 (COMx)**
      (install `CH340_WINDOWS.zip` first if this string doesn't appear — see
      BENCH_SETUP.md §1.5)
- [ ] Open the vendor's own host software (`4 Windows Host Software/cspc Host
      Software V2.1.36/`), select the COM port from the previous step, click
      **OPEN** then the lidar icon (per `Host Software Operation Guide V1.1`
      steps 1–3) — this is the fastest true end-to-end check since it's the
      vendor's known-good reference client, independent of any LidarScan code
- [ ] Click **Running** → point-cloud plot appears and updates live (Guide
      step 4) — confirms full data-path health (wiring, driver, protocol) using
      vendor tooling only
- [ ] Click a point on the plot → angle/distance/intensity readout appears
      (Guide step 6) — confirms per-sample decode is sane, not just "bytes are
      moving"
- [ ] Repeat the raw serial checks from A.1 (start/stop ACKs, ~4,000 pts/s @
      10 Hz) against a terminal program or a throwaway script, independent of
      the vendor GUI, so the same protocol-level checks exist on both OSes

### A.3 Linux (Ubuntu 22.04+)

- [ ] `lsusb` shows `ID 1a86:7523 ... CH340 serial converter`
- [ ] After installing `99-sc-mini.rules` (BENCH_SETUP.md §1.5), `/dev/sc_mini`
      exists as a symlink and is read/writable by a non-root user
- [ ] Repeat the same raw-serial checks as A.1: device-info frame on power-up,
      start ACK, streaming data at ~4,000 pts/s / 10 Hz, stop ACK

### A.4 Android

- [ ] Plug CH340 adapter via OTG/hub → USB permission dialog appears (standard
      Android USB host attach flow) — grant it
- [ ] `usb-serial-for-android` (once wired into the app / a throwaway test
      harness) enumerates the CH340 as a supported driver
- [ ] Same protocol checks as above: device-info frame, start ACK, streaming
      data, stop ACK, ~4,000 pts/s / 10 Hz

**Cross-host pass bar for A:** all four hosts reach "streaming data after start
ACK, stops cleanly after stop ACK." Checksum-pass-rate >99.5% and full parser
correctness are S1's exit criteria, not this checklist's — this checklist is
wiring/enumeration/protocol-liveness, not parser validation.

---

## B. Livox Mid-360, per host

Run §2.3 of `BENCH_SETUP.md` (ping test) before any of the below — if ping
fails, fix that first; none of the SDK-level checks below can pass without basic
IP reachability.

### B.1 macOS

- [ ] Static IP configured on the active interface (built-in Ethernet or USB-C
      adapter) per BENCH_SETUP.md §2.2
- [ ] `ping 192.168.1.1XX` (lidar address) succeeds with stable, low-ms replies
- [ ] Livox SDK2 sample app (or a throwaway SDK2-linked test binary) connects
      and reports point-cloud + IMU callbacks firing
- [ ] **Expected rate: ~200,000 pts/s** sustained — check the SDK sample's
      reported point rate or count packets over a fixed window (spec: 96-pt UDP
      packets, ~22 Mbps sustained, so roughly ~2,080 packets/s)
- [ ] **IMU @ 200 Hz** — check the SDK sample's IMU callback rate directly, or
      count IMU messages over a fixed window
- [ ] Run for **10 minutes** continuously; confirm no packet-loss growth over
      that window (spec S2 exit criterion) — watch the SDK's own drop/loss
      counters if exposed, or track sequence-number gaps if visible at the UDP
      level

### B.2 Windows

- [ ] Static IP configured per BENCH_SETUP.md §2.2
- [ ] `ping 192.168.1.1XX` succeeds
- [ ] Same SDK2 connect / point-rate / IMU-rate / 10-minute soak checks as B.1

### B.3 Linux (Ubuntu 22.04+)

- [ ] Static IP configured per BENCH_SETUP.md §2.2 (`nmcli` or GUI)
- [ ] `ping 192.168.1.1XX` succeeds
- [ ] Same SDK2 connect / point-rate / IMU-rate / 10-minute soak checks as B.1
      — this host doubles as the eventual cloud-worker build target (tech spec
      §3, "same engine, headless CLI build (Linux)"), so a clean pass here
      matters beyond just the bench

### B.4 Android

- [ ] USB-C Ethernet adapter recognized, static IP set per BENCH_SETUP.md §2.2
      (flag immediately if the Ethernet option doesn't appear in Settings at
      all on a given phone — that's the OEM-variance risk from tech spec
      §Risks, worth recording even though this checklist isn't S2/B3 itself)
- [ ] `ping` from the phone (via a terminal app, e.g. Termux, or ADB shell —
      `adb shell ping 192.168.1.1XX`) succeeds
- [ ] SDK2-on-NDK connect + point/IMU rate checks, same as B.1 — this item
      depends on S2's arm64 NDK build existing; skip with a note if S2 hasn't
      landed the Android binding yet, don't block the desktop checks on it

**Cross-host pass bar for B:** ~200,000 pts/s sustained, IMU @ 200 Hz, 10 minutes
with no packet-loss growth, on at least one desktop OS before calling S2's
bench-side exit criteria met; all three desktops + Android is the full spread
worth running before M1.

---

## C. Combined rig sanity (after A and B individually pass)

- [ ] Both sensors powered and streaming **simultaneously** from the same host
      (checks for USB/Ethernet bus contention, powered-hub current headroom —
      see PROCUREMENT.md (b) note on why the hub must be powered, not
      bus-powered)
- [ ] Rig physically stable on the tripod — no flex between phone and either
      lidar when gently pushed (rigidity assumption behind S6 and A8)
- [ ] Camera view (phone) unobstructed by either lidar's mounting hardware
- [ ] D6 zero-angle reference mark photographed/noted for the current mount
      orientation (BENCH_SETUP.md §3 step 1)

---

## Notes for future runs

- This checklist intentionally stops at "packets flow at expected rates" —
  checksum-rate validation (D6) and packet-loss-rate validation (Mid-360) over
  longer soaks are S1/S2's own exit criteria and belong in those spikes'
  reports, not repeated here every time.
- If any item fails after previously passing, suspect cabling/connector wear
  first (the D6's 4-pin 1.5 mm connector is small and non-locking — see
  BENCH_SETUP.md §3 step 5) before suspecting driver or OS regressions.
