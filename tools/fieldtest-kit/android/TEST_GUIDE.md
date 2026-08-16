# LidarScan Android — Field Test Guide (Pixel 8 Pro reference)

This is your walkthrough for testing the LidarScan Android app on the bench
and in the field. It assumes the Pixel 8 Pro as the reference phone (see
`docs/SUPPORTED_PHONES.md` in the repo for other phones), and it assumes you
have the three sensors on hand: **COIN-D6** (USB-serial, CH340), **Livox
Mid-360** (USB-C Ethernet), and a **Unicore UM982** RTK receiver.

Read the "UM982 reality check" in Part 3(c) before you unbox the UM982 —
the short version is that the app cannot talk to it over USB the way you'd
expect, and there's a workaround.

This is a **debug build** of an app that has not run on real hardware before
(see the "what's actually new here" box below) — you are the first real
verification pass for a lot of this. Treat every failure as interesting data,
not just a bug to shrug at. If something doesn't match this guide's
"expected," write down exactly what you saw (screen, numbers, any error
text) — that's more useful than "didn't work."

> **What's actually new here.** Everything in this app has been built,
> compiled, and unit-tested, and a good chunk of it (Filament rendering, the
> replay path, processing jobs, export writers, the RTK screen's UI) has run
> once on an emulator. But almost nothing involving real USB/Bluetooth/
> Ethernet hardware has ever run — the engineering notes are explicit that
> D6 USB permission dialogs, Mid-360 UDP traffic, and RTK Bluetooth SPP have
> **never been exercised against real devices**. You are not double-checking
> finished work; you are doing the first real check.

---

## 0. Before you start

- [ ] Pixel 8 Pro, charged, Android 14 or newer, Developer Options enabled
      with **USB debugging** on (only needed for the adb sideload path, and
      handy for logs if something crashes — see §0.3).
- [ ] The APK: `LidarScan-debug.apk` in this same folder.
- [ ] COIN-D6 + USB-C-to-USB-A (or USB-C OTG) adapter with a CH340 chip in
      the chain somewhere (the D6's own cable ends in USB-A).
- [ ] Livox Mid-360 + its power supply (**9–27 V, ~6.5 W — USB-C cannot power
      it**, see §3(b) fault list) + a USB-C-to-Ethernet adapter (AX88179 or
      RTL8153 chipset — see §3(b) note on why the chipset matters) + an
      Ethernet cable.
- [ ] UM982 eval/carrier board — see §3(c) before assuming this will just
      plug in.
- [ ] A notes app or this checklist printed out — you'll be checking off ~40
      items across four sensors and it's easy to lose your place.

### 0.1 Sideload via Files app (simplest, no computer needed)

1. Copy `LidarScan-debug.apk` onto the phone — email it to yourself, AirDrop
   equivalent (Quick Share) from a laptop, or a USB cable + drag-and-drop
   into the phone's Downloads folder.
2. On the phone, open **Files** → Downloads → tap `LidarScan-debug.apk`.
3. First time only: Android will block the install and offer **Settings** →
   toggle **"Allow from this source"** for whichever app you opened the file
   from (Files, Gmail, etc.) → back button → tap the APK again → **Install**.
4. Open the app once from the installer's **Open** button, or find
   **LidarScan** (debug) in the app drawer.

### 0.2 Sideload via adb (if you have a laptop handy)

```
adb install -r -t LidarScan-debug.apk
adb shell am start -n com.lidarscan.app.debug/com.lidarscan.app.MainActivity
```

The `.debug` in the package name is not a typo — the debug build's
application ID has a `.debug` suffix so it can install alongside a future
release build. `am start` against the plain `com.lidarscan.app` id fails
with "Invalid packageName."

### 0.3 If it crashes

Grab a logcat before you clear it — this is the single most useful thing you
can hand back:

```
adb logcat -d -b crash > crash.txt
```

or, without a laptop, note exactly which screen/button and what happened —
"black screen," "app closed to home screen," "froze for N seconds then
recovered," etc. are all meaningfully different failures.

---

## 1. Sensor A — COIN-D6 (USB-serial, CH340)

**Connect wizard**: Projects list → **+** (new project) → pick **COIN-D6** as
the sensor → the app should route you into the D6 connect wizard on save, or
open it from an existing D6 project's detail screen → **Capture** card →
**Set up**.

- [ ] Plug the CH340 adapter into the phone's USB-C port (OTG), then plug the
      D6 into the adapter, then power the D6.
- [ ] **Permission dialog appears** — a standard Android "Allow LidarScan to
      access USB device?" prompt. Tap **OK**/**Allow**. If it doesn't appear:
      unplug/replug the D6 end (not the phone end) — CH340 clones sometimes
      need the far end to re-enumerate.
- [ ] The wizard's device list shows the CH340 adapter as a connectable
      driver. Tap **Connect**.
- [ ] Connection reaches **Connected** and a health panel appears showing:
      state, points/sec, rotation Hz, checksum pass rate, packets ok/bad,
      bytes in, last error.
- [ ] **Self-test / expected numbers**: **~4,000 pts/s, 10 Hz rotation**,
      checksum pass rate **>99.5%**. A number in the right ballpark but not
      exact is fine — a number that's zero, wildly off, or a checksum rate
      well under 99% is a real fail (loose connector is the first thing to
      suspect — the D6's 4-pin 1.5 mm connector is small and doesn't lock).
- [ ] Data keeps flowing steadily for at least 30 seconds without a stall.
- [ ] Disconnect cleanly from the wizard (or back out of the screen) —
      confirm the app doesn't hang or crash on teardown.

**If the permission dialog never appears at all**: check the adapter is
actually a CH340 — the app's USB filter is scoped to CH340 (VID `0x1A86`,
PIDs `0x7523`/`0x5523`); a different serial chipset (FTDI, CP210x) on the
*D6* adapter specifically won't be picked up by this filter today (this is
D6-specific — the RTK/GNSS path is a separate story, see Part 3(c)).

---

## 2. Sensor B — Livox Mid-360 (USB-C Ethernet)

This is the sensor with the most moving parts, and the one where the "known
prebound-fd/ABI status" below matters — read it before you troubleshoot a
silent connection as a hardware fault.

### (a) USB-C Ethernet adapter

- [ ] Use an adapter with an **ASIX AX88179 or Realtek RTL8153** chipset —
      both have in-box Android driver support (no app/driver install
      needed); other chipsets are a gamble. If you don't know your adapter's
      chipset, check its listing/box — "Gigabit," "USB 3.0," and "works with
      Mac/Windows/Linux" all say nothing about the actual chipset.
- [ ] Plug the adapter into the phone. Plug an Ethernet cable from the
      adapter to the Mid-360. Power the Mid-360 from its **own 9–27 V
      supply** — do **not** expect the USB-C port to power it; it can't, and
      "device does nothing" with the USB-C port as the only power source is
      the single most common false "it's broken" report.

### (b) Static IP on the Pixel — the exact path

Ethernet has no user-facing static-IP API on Android (only a
`signature`-level system API, which no ordinary app — including this one —
can hold), so this is a Settings task, not something the app does for you.
On a Pixel (this holds for Android 14, 15, and 16 — Pixel's own settings,
not a per-OEM skin):

1. **Plug the adapter in first.** The Ethernet entry does not exist in
   Settings until an adapter is physically attached and detected.
2. **Settings → Network & internet → Ethernet.**
3. Turn off **DHCP** / switch to **Static**, and fill in:
   - **IP address**: the *phone's* address, e.g. `192.168.1.5`
   - **Gateway**: any address on the same /24, e.g. `192.168.1.1` (there's
     no real gateway on a direct cable — Android just wants something here)
   - **Netmask**: `255.255.255.0`
   - **DNS**: `8.8.8.8` (nothing on this link resolves names, but leaving it
     blank sometimes blocks Save — see next point)
4. **If Save stays greyed out**: this is a known rough edge in stock
   Android's Ethernet UI, not specific to this app. Fill in *every* field
   (IP, gateway, netmask, and DNS — an empty DNS field is the most common
   cause), or try toggling the Ethernet switch off, re-entering the static
   fields, then switching it back on. If it's still stuck, the Mid-360
   connect wizard's own **Interface status** panel (see below) reads the
   *actual* address off the interface directly, which is the ground truth
   regardless of what Settings displays — use that to confirm the real state
   rather than trusting the Settings screen's own display.
5. The Mid-360 wizard defaults to lidar `192.168.1.1XX` / host `192.168.1.5`
   — match your Settings entry to the host address, or read the wizard's
   **"use this as the host IP"** button once the interface has a real
   address and let it fill in the field for you.

This exact menu path (`Network & internet → Ethernet`) is also what the
app's own in-wizard guidance shows for a Pixel — if what you see on your
phone differs, note it exactly; that's useful data for
`docs/SUPPORTED_PHONES.md`'s Ethernet-quirks column.

### (c) In-app wizard + self-test

Reach it from a Mid-360 project's **Capture** card → **Set up**, or from the
D6 wizard's own door to it.

- [ ] **Interface status** section shows the adapter as present with a real
      IPv4 address once static IP is configured. "Up but no address" is
      itself a valid (if unhelpful) state — it means the OS sees the
      adapter but you haven't finished step (b) above.
- [ ] Enter/confirm lidar IP and host IP, tap **Run self-test**.
- [ ] **Pass criteria**: first packet within **~8 seconds** (the wizard's own
      timeout is 8 s, deliberately shorter than the engine's 10 s internal
      grace period), then sustained **~200,000 pts/s**, **IMU @ 200 Hz**. The
      health readout also shows loss % — should sit near 0% once streaming.
- [ ] On failure, the wizard **diagnoses**, not just reports: it distinguishes
      "datagrams arriving but nothing decodes" (wiring/addressing is fine,
      port/format is wrong) from "the driver reported a fault" (usually a bad
      host IP or already-bound port) from "no datagrams at all" (cabling,
      power, or wrong subnet — the diagnosis text spells out the Mid-360's
      power requirement specifically, since that's the one people miss).
- [ ] On a pass, the device is left streaming — watch the loss % for another
      20–30 seconds; it should stay at ~0%.

**Known prebound-fd/ABI-v5 status — read this before treating a
self-test-pass-but-capture-fails as a mystery.** The wizard's self-test and
the *actual capture session* use two different code paths, and this is a
real, documented gap, not a guess:

- The **self-test probe** explicitly binds its socket to the phone's
  Ethernet network object (Android's `Network.bindSocket`) before talking to
  the Mid-360, so it works even if the phone has other active connections
  (Wi-Fi, cellular) with competing routes.
- **Actual capture** (after you tap "Connect" post-pass) goes through the
  engine's normal SDK2 backend, which creates its own sockets *without* that
  explicit network binding — it relies on the OS's routing table happening
  to send Mid-360 traffic out the Ethernet interface. The engine ABI gained
  the fields needed to fix this (two pre-bound descriptors, full backend
  config) in a recent version bump, but the app has **not yet been rewired**
  to use them for capture — that's a listed follow-up, not something this
  build does.
- **Practical consequence for you**: if self-test passes cleanly but the
  live Capture screen shows no points arriving, put the phone in **Airplane
  mode with Wi-Fi/Ethernet re-enabled** (i.e., kill cellular and Wi-Fi, keep
  only the Ethernet link up) before capturing — that removes the routing
  ambiguity entirely and is the single most useful thing to try. Please
  report explicitly whether this happens on the Pixel 8 Pro; it's flagged as
  untested since no Mid-360 hardware existed while the app was built.
- Also note whether Live-SLAM point-cloud rendering (the moving/registered
  cloud, vs. the raw sensor-frame preview) looks doubled or smeared — that
  was a real bug found and fixed during development (stream filtering), but
  it has never been seen on an actual rendered frame, only reasoned through.

---

## 3. Sensor C — UM982 RTK rover

### Reality check — read this first

The app's RTK screen only knows how to talk to a rover over **Bluetooth SPP**
(the same kind of pairing as a Bluetooth keyboard, using a system PIN
dialog). **It does not have a USB-serial GNSS path.** A UM982 eval/breakout
board is usually a bare USB-serial device (CH340 or CP210x, no Bluetooth) —
so whether this sensor is testable on this app today depends entirely on
which UM982 carrier board you have:

**(i) If your UM982 carrier board has Bluetooth (SPP) built in** — many
commercial RTK-rover carrier boards do, even when the bare eval board
doesn't — use it:

- [ ] Pair the rover in the phone's Bluetooth settings first (system PIN
      dialog — this app does not do discovery/pairing itself, by design:
      see the in-repo engineering notes for why).
- [ ] Open the project's **RTK rover** card → connect to the paired device.
- [ ] Confirm NMEA sentences start flowing — the fix-quality strip should
      show a fix state (No Fix / Single / Float / Fixed) and update as the
      receiver acquires satellites.
- [ ] If you have NTRIP correction access, configure the mountpoint and
      confirm the state moves toward **Float** then **Fixed** and corrections
      age stays low (single digits of seconds).
- [ ] Confirm the §3.4 capture gate behaves: with no fix, a Survey-profile
      project should show a **blocking** warning at capture time; other
      profiles should **warn** but allow capture.

**(ii) If your UM982 board is USB-serial only (the common case for a bare
eval board)** — **this is a real, confirmed gap, not a missed setting.**
There is no code path in this app that reads a USB-serial GNSS device and
feeds it NMEA. Do not spend time hunting for a hidden USB-GNSS option — it
isn't there. Options, in order:

1. **Test the UM982 against the desktop/Windows field-test kit instead**
   (a different part of this same test-kit effort) — it has a general
   serial path that isn't limited to Bluetooth.
2. **If you can get or make a Bluetooth-SPP carrier for the UM982**, use
   path (i) above.
3. This gap has been written up in the engineering notes
   (`android/NOTES.md`, the ABI-5 rebind list) with a scoped fix: a small
   `GnssUsbSerial` reader that reuses the D6 driver's existing USB-serial
   plumbing and feeds the same `push_nmea` entry point the Bluetooth path
   already uses. It is sized as "small — reuses existing code," not a
   redesign, for whoever picks it up next.

**Verdict to report back**: state plainly which of (i)/(ii) applied to your
UM982 hardware, and if (i), whether the Bluetooth pairing/NMEA/NTRIP flow
above actually worked — none of it has run against real RTK hardware before.

---

## 4. Capture, live preview, processing, export, plan — full pass list

Do the **replay** pass first (no hardware needed, confirms the app itself is
healthy on your phone) before the live-sensor passes, so a real-hardware
failure isn't confused with an app-install problem.

### (a) Replay (synthetic, no hardware)

- [ ] Settings screen → **"Replay synthetic capture"** (under the
      "Engine (developer)" section).
- [ ] A "Synthetic Replay Demo" project is created/opened and the app
      navigates straight into a live capture-style screen.
- [ ] Tap **Start replay** — the 3D view should render a growing point cloud
      within a couple of seconds, and the "Points captured" stat should
      climb steadily for the length of the bundled capture (tens of
      thousands of points total).
- [ ] Orbit the camera (one-finger drag) and pinch-zoom — should feel
      responsive, no crashes.
- [ ] Let it finish or stop it — confirm the app returns to Project Detail
      cleanly and the project now shows a non-empty capture.

### (b) Live capture — D6

- [ ] From a D6 project, connect per Part 1 above, then **Capture** screen →
      confirm the same health/points-per-second numbers show live, in the
      capture UI (not just the connect wizard).
- [ ] Toggle **Live-SLAM** vs **Record-only** (only editable before starting)
      — start a short capture in each mode, confirm both run and stop
      cleanly, and check the point-cloud view actually renders points, not a
      blank/black view.
- [ ] **Pause/Resume**: available for D6. Confirm pause genuinely stops point
      growth and resume genuinely continues it (not a restart from zero).
- [ ] **Stop** → confirm a session-summary sheet appears with final stats,
      and the project detail screen now shows "Points captured" as a real
      number, not "No capture yet."

### (c) Live capture — Mid-360

- [ ] Connect per Part 2 above (mind the routing note if capture doesn't
      receive data despite a passed self-test).
- [ ] Same Live-SLAM/Record-only pass as D6. In Live-SLAM mode, confirm the
      cloud looks like one coherent, non-doubled surface — not a
      superimposed/rotating double-exposure (this is the specific bug class
      flagged as never-seen-on-real-hardware in Part 2).
- [ ] **Pause is intentionally not offered for Mid-360** — confirm the Pause
      button is absent/disabled rather than present-but-broken; this is a
      deliberate app decision (resuming would destroy the first half of the
      recording), not a bug to report.
- [ ] Stop → confirm the summary sheet and updated project card, same as D6.

### (d) Processing → export → plan → review

Run this on whichever capture (replay or real) is most convenient — the
pipeline doesn't care which sensor produced the `.lscan`.

- [ ] Project Detail → **Processing** card → run post-process. Confirm a job
      appears in the queue, progresses, and completes ("Done") without the
      app needing to stay in the foreground the whole time (background it
      briefly and confirm the job either keeps running or resumes sanely —
      there's a known limitation here: a very long process can be killed if
      the OS reclaims the app while backgrounded, since there's no
      foreground service yet; a short test capture shouldn't hit this).
- [ ] **Export**: pick a format (LAS 1.4 for Survey-profile projects,
      binary PLY otherwise are the defaults) → confirm a file is produced
      and its size looks plausible for the point count.
- [ ] **Colorize**: only testable if the capture has camera keyframes (ARCore
      must have been tracking during capture) — confirm the gate correctly
      blocks/allows based on whether keyframes exist, and if it runs,
      confirm colorized points actually show color, not uniform grey.
- [ ] **Plan view**: Project Detail → **Review** or a dedicated **Plan**
      entry → confirm a floor-plan extraction runs and shows walls/openings
      as a 2D drawing; check DXF/PDF export from there produces
      non-empty, valid-looking files.
- [ ] **Review / display params**: open the bottom sheet, switch color modes
      (Height/Intensity/RGB), move the point-size slider, confirm the
      renderer visibly updates each time.
- [ ] **Measure tool**: tap two points in the cloud, confirm a distance
      readout appears and updates as you re-pick points.
- [ ] **Merge**: only succeeds for georeferenced (RTK-backed) captures — for
      anything else, confirm it **refuses with a clear explanation**
      ("No georeference recorded…") rather than silently producing a
      meaningless merged cloud. This refusal path is expected behavior, not
      a bug.

---

## 5. Wrap-up

When you're done, report back:

- Pass/fail per checkbox above, with notes on anything that didn't match
  "expected" (numbers, screenshots if easy, or just a clear description).
- The UM982 verdict from Part 3: which sub-case (i) or (ii) applied, and
  whether it worked.
- Whether the Mid-360's routing workaround (Airplane mode + Ethernet only)
  was needed, and whether it helped.
- The exact Ethernet Settings path you actually saw on your Pixel, if it
  differed at all from Part 2(b) above.
- Anything that crashed, with a logcat if you can grab one (`adb logcat -d -b
  crash`).
