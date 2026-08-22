<body>
<img src="docs/img/app-icon.png" width="96" height="100%" align="left" alt="Ollidar icon — a llama in a top hat emitting a lidar fan">

# Ollidar


<br>
<br>
Strap a small spinning lidar to your phone, walk around a room, and get a
corrected 3D point cloud out the other end.

<br clear="left">

Current version: **0.9.18** (Android).

The app and this repository are both called **Ollidar**. The Android package
(`com.lidarscan.app`), the `.lscan` files and the `Downloads/LidarScan/`
export folder keep the old **lidarscan** name on purpose: on Android the
package name *is* the app's identity, and changing it would install a second
app rather than rename the one already on the phone.

---

## What it does

- **Tap Scan, hold still, walk, tap Stop.** The app measures the sensor's
  exact angle on your phone at the start of every scan, then sweeps its
  spinning lidar slice along the path your phone's camera tracks. No manual
  calibration step.
- **Live coverage guidance while you walk.** Thin or missed walls show up as
  amber arcs in the live view so you know where to walk back over before you
  stop, not after.
- **A big warning when tracking is lost.** An amber card fills the screen —
  *"Tracking lost. Stop. Hold still."* — because walking through a tracking
  gap usually makes it unrepairable afterwards.
- **The mount is re-measured every time.** The lidar comes off the bracket
  between sessions, so the app never trusts an old or hard-coded angle — it
  measures fresh at the start of each scan.
- **Processing runs by itself.** The moment you press Stop, the scan is
  graded and processed automatically, with honest numbers (a self-check
  distance in centimetres, not a made-up star rating) rather than a grade
  you'd have to take on faith.
- **Export or share straight from the phone.** PLY, LAS 1.4, PCD, DXF or PDF,
  one scan at a time or several at once, straight to your Downloads folder or
  the Android share sheet.

---

## Supported hardware

At a glance — what plugs into which app, and how ready it is. ✅ Working ·
🔶 Setup needed · 🧪 Untested · ❌ Not supported.

### Phone app (Android) — the handheld scanner

| Hardware | Connection | Status |
| --- | --- | --- |
| COIN-D6 | USB-C (serial) | ✅ Working — field-proven |
| Livox Mid-360 | Ethernet (USB-C adapter) | 🔶 Setup needed — needs an Android-supported Ethernet adapter; guided wizard + diagnostics built in |
| LDROBOT STL-27L | USB-C (serial) | 🧪 Untested — supported in code, first hardware bench pending |
| Unicore UM982 RTK | Bluetooth | 🔶 Optional — for outdoor georeferencing; bench-verified link |

### Desktop app (macOS) — the processing workbench

| Hardware | Connection | Status |
| --- | --- | --- |
| Livox Mid-360 | Ethernet | ✅ Working — bench-proven capture → process → 3D cloud |
| COIN-D6 | USB (serial) | 🔶 Working on macOS 26 — serial path unverified on older macOS |
| LDROBOT STL-27L | — | ❌ Not supported yet |

### Which Ethernet adapter for the Mid-360

The Mid-360 talks over Ethernet, so on the phone it needs a USB-C Ethernet
adapter Android's kernel can actually drive. What to buy — **recommended,
untested on this rig**:

| Want | Buy | Why |
| --- | --- | --- |
| Lidar only | A plain, single-purpose USB-C gigabit Ethernet adapter on a **Realtek RTL8153** (e.g. TP-Link UE300C) or **ASIX AX88179** chipset | These are the two chipset families Android ships drivers for, and they run **unpowered** straight off the phone |
| Lidar **and** charging at once | A **powered USB-C hub with Ethernet** (Anker 341 / 343 class) | The charger feeds the hub, the hub feeds the phone and the Ethernet chip |
| — | Avoid multi-port laptop hubs as a first try | They frequently need a charger plugged into their **USB-C PD port** before the Ethernet chip powers up at all. This is exactly what happened to the owner's **Acer HY41-T9**: nothing enumerated until the hub had its own power |

**Be clear about what this is.** None of these have been tested on this
project's phone. The Mid-360 has never enumerated an Ethernet interface on
Android here at all, so the list above is reasoned from what Android supports
and from the one hub that failed — it is a recommendation, not a proven
compatibility list. If you try one, the result is worth reporting.

---

## Requirements

### Phone app (Android)

| | Minimum | Notes |
| --- | --- | --- |
| Android version | Android 10 (API 29) | Built for Android 16 (API 36). The app installs without ARCore, but a sensor-position scan needs it — full 3D scanning needs Google's AR tracking (ARCore), which needs roughly Android 14+ on a supported device |
| Required hardware | USB host (OTG), OpenGL ES 3.0 GPU | The serial lidars plug in over USB-C; required by the manifest |
| Optional hardware | — | ARCore-capable camera (needed for real scanning), gyroscope (motion witness during tracking loss — strongly recommended), Ethernet adapter support (Mid-360 only), Bluetooth (RTK rover only), GPS (outdoor georeferencing only) — the app runs without these, feature-gated |
| RAM | Any (auto-tiered) | The app auto-tiers its rendering to the device — roughly modest phones get a 96 MB point budget, standard 256 MB, flagships 512 MB; runs on modest hardware with reduced live detail. Field-validated device: Google Pixel 8 Pro |
| Storage | ~150 MB | APK ~100 MB; a typical scan is 5–50 MB |

### Desktop app (macOS)

| | Minimum | Notes |
| --- | --- | --- |
| OS | macOS 14, Apple silicon (arm64) | Validated on macOS 26; the COIN-D6 USB-serial path is unverified below macOS 26 (Qt serial library constraint) |
| GPU | Metal (any Apple silicon) | Validated on Apple M4 |
| Storage | ~96 MB | Expanded app size |

---

## Tested hardware and results

What's actually been run, on what, with what numbers — not marketing claims.

### Lidars

| Hardware | Where tested | Key numbers |
| --- | --- | --- |
| **COIN-D6** lidar | Bench + field | Bench: 100.0000% packet checksum pass over a 180 s soak, ~4,000 samples/s at ~10 Hz spin, per-point noise σ = 5.2 mm. Field: best scans reach ~1.0–1.4 cm same-surface consistency at walking pace |
| **Livox Mid-360** | Bench (desktop/macOS) | 60 s soak at 199,999 points/s, built-in IMU at 200.00 Hz, zero lost packets — full capture → process → 3D cloud pipeline verified. Phone connection currently blocked on Android Ethernet adapter compatibility |
| **LDROBOT STL-27L** | Not yet tested | Driver is code-complete with protocol-exact simulated tests; awaiting first hardware bench |

### Phones

| Hardware | Where tested | Key numbers |
| --- | --- | --- |
| **Google Pixel 8 Pro** (phone) | Field, primary test device | ARCore tracking ~30 Hz; phone IMU sustained at 399/400 Hz; factory camera↔IMU calibration read and in use; best recorded self-check 0.69 cm (scan-085) |
| **OPPO CPH2499** (phone, Android 15) | Field, one outside user, 0.9.11 | **ARCore session fails — "the tracking camera stopped" (FatalException), repeatedly; no scan has completed on this device yet.** Under investigation. Believed to be vendor power management taking the camera back from ARCore; 0.9.12 detects the state, says so, and offers Retry / Send logs plus the battery-optimisation guidance. The same user reported the UI rendering "packed tight", which 0.9.12's responsive work addresses. |

### Computers

| Hardware | Where tested | Key numbers |
| --- | --- | --- |
| **Desktop app** (Apple M4, macOS) | Bench | 146k-point cloud renders at ~46 fps (Metal); floor-plan extraction and merge workbench verified against synthetic ground truth, merge ICP residual ~10 mm |

### Other

| Hardware | Where tested | Key numbers |
| --- | --- | --- |
| **Unicore UM982** RTK receiver | Bench (indoor, no antenna fix) | 230400 baud, 7 NMEA sentence types at 1 Hz including dual-antenna heading, 210/210 checksums OK |

Notes:
- The whole app has been field-tested only on the Pixel 8 Pro, through 0.9.11 — no other phone has been tried yet.
- One vendor CH340 USB-serial adapter caused COIN-D6 stream stalls and eventually failed; a replacement adapter fixed it 100%. Adapter quality matters.
- The Mid-360 pipeline is fully proven from a desktop over Ethernet; one laptop-class USB-C hub (Acer HY41-T9) failed to enumerate on Android. Diagnostics for this shipped in 0.9.10; adapter guidance is above, in the manual, and in the wizard itself as of 0.9.11.

---

## Quick start

1. Install the APK and open the app.
2. Mount the lidar flat on the back of the phone (zero mark up, cap pointing
   forward) and plug it in over USB-C — or, for a Mid-360, wire it up over
   Ethernet (see Mid-360 diagnostics below).
3. Tap the **Scan** tab (the radar icon). The app finds the sensor by itself.
4. Press the big **SCAN** button and **hold still** — a short panel counts
   through locking tracking and measuring the mount, then says
   **"GO — start walking."**
5. **Walk slowly**, turning on the spot rather than swinging the phone
   around corners.
6. Press **STOP**. The scan is saved, graded and processed automatically —
   you land in Projects with the finished scan ready to open.

Finished scans live in the **Projects** tab. Tap one to view it, export it,
or share it.

Full detail: [`docs/QUICK_START.md`](docs/QUICK_START.md) for a first scan in
ten steps, [`docs/USER_MANUAL.md`](docs/USER_MANUAL.md) for everything else —
every tab, the viewer, Mid-360 and STL-27L setup, and troubleshooting.

---

## Key features

### Scanning

- Automatic mount re-zero at the start of every scan, defended against bad
  readings — a materially worse measurement is refused and sampling keeps
  going.
- A four-stage start panel (new tracking session → lock tracking → measure
  the mount → GO) so you always know what the app is waiting on and why.
- A full-screen tracking-loss popup that tells you to stop and hold still,
  and clears itself automatically once tracking returns.
- Leaving the Scan tab (switching to Projects, Jobs or Settings) stops and
  saves whatever scan is running — nothing is left half-recorded.
- A six-step in-app tutorial, offered once on first launch and replayable
  any time from Settings.

### Viewing

- Orbit with one finger, pan with two, pinch to zoom, double-tap to reframe
  the whole scan — a real touch viewer, not a turntable locked to one point.
- A measure tool: tap two points, get a distance in metres or feet.
- Multiple colour modes and a walked-path overlay so you can see where you
  went. Height colouring uses a Turbo ramp (dark blue → red) by default;
  grey and the other ramps are still there in the display panel.

### Projects

- A gallery or list view of every scan, sortable by name or date.
- Long-press to select several scans at once, then export or share them
  together — one job per scan, gathered into a single share sheet.

### Quality and honesty

- Every processed scan gets a real self-check number in centimetres, not a
  cosmetic star rating.
- A gap-rescue pass tries to re-register a tracking-loss break automatically,
  and an auto-level pass straightens a tilted floor — both only apply a fix
  if it demonstrably improves the scan, and both leave the file untouched if
  they can't.
- The maximum point detail is capped to what your phone's memory can safely
  hold — there is deliberately no override, because a setting whose only job
  is to crash the app isn't a setting worth having.
- A built-in crash recorder captures every process death to the log, so a
  send-logs report actually contains what happened.
- Problems are logged and explained on their own terms — the app doesn't
  blame poor lighting or the environment without evidence for a failure it
  hasn't diagnosed.

### Profile and feedback

- A Profile screen with your device facts (app version, device, scan count,
  storage used) and a one-tap **Send logs** that bundles the capture log and
  ships it to your mail app, chat, or a configured server.
- A feedback box for typing a note that goes along with the same bundle.

### Mid-360 diagnostics

- A guided Ethernet setup wizard: it walks the physical chain (adapter →
  link → address → lidar heard) one rung at a time and tells you exactly
  which one is failing and what to do about it.
- A hidden developer **[net-debug]** mode (seven taps on the version footer)
  for deeper diagnostics when the guided wizard isn't enough.

---

## Recent updates

- **0.9.5** — automatic mount re-zero at scan start, an auto-level pass for
  tilted floors, and a per-device mount profile (no more hard-coded mount
  geometry).
- **0.9.6** — hotfix: a deadlock that stopped every COIN-D6 scan from
  starting, plus a clearer scan-start progress panel.
- **0.9.7** — six stability root causes fixed (including a crash recorder,
  a navigation bug that silently killed processing, and a renderer crash on
  the detail slider), a new Simple Mode, and the Agtom orange theme and app
  icon.
- **0.9.8** — fixed a bug where the Scan button went dead after finishing a
  scan, brought back export/share plus group export/share, and added the
  full-screen tracking-loss warning banner.
- **0.9.9** — icon-only tab bar, the Profile and Send logs screen, the
  in-app tutorial, and a centered tracking-loss popup.
- **0.9.10** — real orbit/pan/zoom gestures in the viewer, STL-27L support,
  and the Mid-360 Ethernet diagnostics wizard, alongside this manual.
- **0.9.11** — the app is now called **Ollidar** and has a new llama icon
  (the package and the export folder keep the `lidarscan` name);
  Scan and Review both go fullscreen, with the tab bar hidden while a scan
  runs and a tap on empty space hiding the viewer's controls; both
  orientations are supported, with the start orientation worked out from
  gravity and then locked for the scan; height colouring defaults to a Turbo
  ramp; and the Mid-360 wizard, the manual and this page now name which
  Ethernet adapter to buy.
- **0.9.12** — the Scan tab is a laid-out page again while it is idle:
  connection settings sit in the main flow instead of floating over the
  preview, nothing overlays anything except warnings, and the tab bar
  reserves its own space (and gives it back when a scan starts, which is also
  when the screen goes minimal and hides everything you cannot act on). The
  layout adapts to small phones and large text; the Advanced sheet gains a
  **Connection** tab; the duplicate tracking status and device name are gone,
  and so is the map chip. A failed **Process** now says so, with the reason,
  and Review has a Process button where it always said one was. Height
  colouring migrates existing scans to Turbo. And when ARCore's tracking
  camera dies — which is what happens on the OPPO above — the app says so and
  offers a retry instead of waiting forever.

---

## Honest limits

- **STL-27L has never touched real hardware.** Code-complete and tested
  against synthetic fixtures, but bench validation is still pending — treat
  the first real scan as a test.
- **Mid-360 has never come up over Ethernet on this phone.** A plain
  RTL8153 or AX88179 adapter should work unpowered, and a powered USB-C hub
  should cover lidar plus charging — but that is a recommendation, untested
  on this rig. Multi-port laptop hubs commonly need a charger on their PD
  port before the Ethernet chip powers up at all.
- **A tracking-loss gap usually can't be repaired.** Standing still until
  tracking returns is the only reliable fix — the gap-rescue pass refuses
  far more often than it succeeds, on purpose.
- **There is no detail-level override.** Point detail is capped to what your
  phone's memory can hold, because an earlier version that allowed
  overriding it could crash mid-scan.

---

## Repository layout

| Path | What's there |
| --- | --- |
| `android/` | the Android app (Kotlin + Jetpack Compose) |
| `engine/` | the shared C++ scanning/processing engine |
| `desktop/` | the desktop viewer app |
| `cloud/` | the optional cloud processing worker and job service |
| `docs/` | the user manual, quick start, and design docs |

---

## Contributing

Bug reports, suggestions, feature requests, and pull requests are all
welcome — this project is better with more eyes on it.

- Found a bug or have an idea? Open a [GitHub Issue](../../issues) — see
  [CONTRIBUTING.md](CONTRIBUTING.md) for what to include.
- Want to fix something yourself? Pull requests are welcome; CONTRIBUTING.md
  covers the workflow and the test suites to keep green.
- Just want to try the app? Grab the latest APK or desktop build from the
  [Releases page](../../releases) (beta).

If you're filing a bug report, attaching the app's Send-logs bundle
(Profile → Send logs) helps a lot — it's the fastest way to see what
actually happened.
</body>
