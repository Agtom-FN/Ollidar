# Ollidar — User Manual

App version 0.9.18 (Android). Written for the owner and for field testers.
If you only want a first scan, read [QUICK_START.md](QUICK_START.md) instead.

The app is called **Ollidar** as of 0.9.11. The repository, the Android
package (`com.lidarscan.app`), the `.lscan` scan files and the
`Downloads/LidarScan` export folder keep the `lidarscan` name on purpose —
renaming the package would install a second app rather than rename this one,
and the exports already on the phone are in that folder. Sentences below
about what an older version did keep the old name.

Everything in quotation marks below is text the app actually puts on screen.

## Contents

1. [What Ollidar is, and what it is not](#1-what-ollidar-is-and-what-it-is-not)
2. [The four tabs](#2-the-four-tabs)
3. [The Scan tab](#3-the-scan-tab)
4. [When tracking is lost](#4-when-tracking-is-lost)
5. [Projects](#5-projects)
6. [The viewer (Review)](#6-the-viewer-review)
7. [Profile, Send logs, Feedback](#7-profile-send-logs-feedback)
8. [The tutorial](#8-the-tutorial)
9. [Settings map](#9-settings-map)
10. [Detail, and the device ceiling](#10-detail-and-the-device-ceiling)
11. [Mounting the COIN-D6](#11-mounting-the-coin-d6)
12. [Livox Mid-360 setup](#12-livox-mid-360-setup)
13. [STL-27L (LDROBOT)](#13-stl-27l-ldrobot)
14. [Troubleshooting](#14-troubleshooting)

---

## 1. What Ollidar is, and what it is not

Ollidar turns a phone plus a small lidar into a walk-around 3D scanner. You
plug a lidar into the phone, press one button, walk through a space, and press
it again. The app saves the scan, processes it, and lets you look at it,
measure in it, export it and share it.

### The hardware it works with

| Sensor | Link | What it is | Status |
| --- | --- | --- | --- |
| **COIN-D6** | USB-C serial (CH340 class), 230400 baud | 2D spinning lidar, 360°, ~4,000 points/s at 10 Hz, 0.9° resolution, 0.05–12 m | The sensor the app is built around and field-tested with |
| **Livox Mid-360** | USB-C Ethernet adapter, UDP | 3D lidar, 360° × 59°, ~200,000 points/s, built-in IMU, needs its own 9–27 V power | Supported. Ethernet setup is the hard part — see §12 |
| **STL-27L (LDROBOT)** | USB-C serial, 921600 baud | 2D spinning lidar, 360°, ~21,600 points/s at ~10 Hz, 0.02–25 m | **Supported — bench validation pending.** No hardware has ever been tested. See §13 |

An RTK GNSS rover can be paired for georeferenced outdoor capture. That is an
advanced feature and is not covered in detail here.

### How it builds 3D from a 2D lidar

The COIN-D6 and the STL-27L are flat scanners: they measure a single slice of
the room, 360° around one axis. They have no idea where they are. The phone
supplies that. The rear camera and the phone's IMU track where you walk
(through ARCore), and the app sweeps the flat slice along your path to make a
3D cloud. This is why the camera is on for the whole scan, why the app cares
so much about tracking, and why the D6's exact angle on the phone has to be
measured before every scan.

The Mid-360 is different: it is a real 3D sensor with its own IMU and its own
odometry, so it does not depend on the phone's camera the same way.

### What it does not do

State these plainly, because they cost field sessions:

- **It cannot scan in the dark, or against blank surfaces.** Camera tracking
  needs visible texture. A featureless white wall an arm's length away is
  enough to lose tracking even in good light.
- **A gap you walked through usually cannot be repaired.** If the camera
  stops tracking and you keep walking, the app does not know how far you
  went. Post-processing has a rescue pass that can sometimes re-register the
  two halves using the lidar and the gyro, but it refuses far more often
  than it succeeds, and it refuses on purpose — a wrong join is worse than a
  visible seam. Standing still until tracking returns is the only reliable
  fix, and it is a fix you have to perform at the time.
- **A scan that recorded nothing cannot be recovered.** If the sensor was
  never streaming, or the phone never tracked, the file is empty. The app
  says so — "NOT RECORDED", "NO ROOM — NOTHING WAS PLACED", "2D ONLY — NO
  ROOM" — rather than showing you a grade you would trust.
- **Pausing is not offered for every sensor.** The pause button is dimmed
  for a Mid-360, because a Mid-360 cannot resume without truncating the
  recording.
- **The app cannot set your phone's Ethernet address.** Android has no
  public API for it. For a Mid-360 you type the static IP into Android's own
  settings; the app can only tell you exactly what to type (§12).
- **There is no override for the detail ceiling.** The phone's memory decides
  the maximum. See §10.
- **It is not a survey instrument out of the box.** Without an RTK rover a
  scan is in its own local frame, positioned by the phone's GPS at best.
- **It has not been tested on many phones.** The reference device is a Pixel
  8 Pro. `docs/SUPPORTED_PHONES.md` is honest about what has and has not been
  run on real hardware.

---

## 2. The four tabs

The bar at the bottom floats over the content and is **icon only** as of
0.9.9. There are no labels. Left to right:

| Position | Icon | Tab |
| --- | --- | --- |
| 1 | open folder | **Projects** — every scan you have taken |
| 2 | radar (arcs sweeping from a point) | **Scan** — where scans are made |
| 3 | stacked layers | **Jobs** — the processing queue for one scan |
| 4 | three sliders | **Settings** — this device's preferences |

The selected tab is the **orange icon** inside a soft orange capsule. As of
0.9.10 there is **no dot** under it — round 24 added one and round 25 removed
it, along with the space it reserved, so the icons sit centred in the bar.

Screen readers announce each icon by its name ("Projects", "Scan", "Jobs",
"Settings").

**As of 0.9.11 the bar hides itself while a scan is recording or starting**,
and slides back when you stop or cancel. The Scan screen is a camera screen
while it is working, and four tabs across the bottom of a live view are four
ways to end the scan by accident (§3.6).

**Jobs** is a queue view for a single scan. You rarely need it: processing
runs by itself after every Stop. If no scan is active, the tab opens a picker
("Processing · choose a project").

---

## 3. The Scan tab

### 3.1 The idle page — rebuilt in 0.9.14

The Scan tab, before you press SCAN, is one page in two states:

```
COIN-D6 · Ready                                    [⋮⋮⋮]
LAST SCAN
┌──────────────────────────────────────────────┐
│  (the last scan, drawn)                       │
│  Scan-085-2026-08-21-1803                     │
│  46.5 K pts · 21 Aug        ·  ● FAIR         │
└──────────────────────────────────────────────┘
READY TO SCAN
 ● Sensor      COIN-D6 connected
 ● Mount       Set · 91.0°
 ● Tracking    Ready
                    ( ● SCAN )
```

Three rows, and **each states its own state and carries its own fix**. Green
is ready, amber will work and be worse, red cannot start — and only the *first*
red row is drawn red, because a screen with two red rows has stopped ranking
its own problems. The SCAN button is enabled when nothing is red.

**With no scanner attached** the Sensor row goes red, says *"Not found · Plug
it in, then retry."*, carries its own **Retry**, and **opens the connect flow
inside the card underneath it** — scan name, the auto-detect line, and the
manual panel. Nothing else about the page changes. Until 0.9.14 this state was
a different screen entirely (a dead black viewport, a `00:00 / 0 pts / 0.0 m`
card and a row of pills); that page is gone.

The tour that used to hang off a floating **?** is now a row in the Advanced
⚙ sheet and in Settings › Tutorial. Zero-valued readouts are not drawn at all:
`0 pts` is not information.

Tapping **LAST SCAN** opens it in the viewer. The card is absent only when the
phone holds no scans.

### 3.1a Connecting the sensor

Open the Scan tab with the sensor plugged in. Auto-detect runs on entry and
races both probes (USB serial and Ethernet). You will see one of:

- a device line naming what was found, or
- **"No scanner found. Plug it in, then Retry."** with a **Retry** button, or
- **"Plug the D6 into USB-C."** / *"It appears here as soon as it does."*

**Enter manually** opens a panel with two blocks. `USB SCANNER` carries a
**D6 / STL-27L** selector — say which serial lidar is on the port, then pick
the device and tap **Connect**. `LIVOX MID-360 · ETHERNET` takes the Lidar IP
and This phone.

The selector matters because both sensors are the same class of USB-serial
device at different speeds, so auto-detect has to guess and you do not. If
auto-detect names the wrong one, come here and say which it is — **and from
0.9.16 that choice is what the capture runs as, full stop.** Picking a sensor
here releases whatever the app had already connected to and reopens the port
at the sensor you named. Before 0.9.16 the port was still held by the earlier
connection, the reopen was refused by the operating system, and the wrong
sensor kept running underneath a failure message.

A third line can appear from 0.9.16:

- **"Can't tell which scanner this is. Pick D6 or STL-27L below, then
  Connect."** — the port answered with fragments of both protocols and the app
  refused to guess. The selector is the first control in the panel that opens
  with this line.

There is no self-test step. *"Points on screen mean it works."*

### 3.2 The Advanced ⚙ sheet

The three-fader button at the top right of the Scan page is the one door to
everything that is not one of the three readiness rows. It has two halves,
**Display** and **Connection**:

- **Display** — view, live view on/off, colour mode, colormap, point size,
  gamma, brightness, live refresh and **Detail** (§10). Nothing in it touches
  the recording, and it says so in one line.
- **Connection** — the same connect controls the Sensor row shows when nothing
  is attached, for when something *is*.
- **Tutorial** — the six-step tour (§8).
- **Diagnostics** — read-only: device state, points/sec, rotation, checksum
  pass rate, packet loss, tracking, keyframes, tracking-loss episodes, poses
  pushed to the engine, mount extrinsic, georeference source.

**Mid-360 setup** and **RTK position** appear on the page only when a Mid-360
is the selected sensor (or Lab features is on). A D6 operator never sees them.

~~The chip row~~ — **gone in 0.9.13/0.9.14.** `Capture OPTIMAL`, `Diag`, `New
capture`, the mount pill and the `?` were five visual treatments in two ragged
rows, none of them the primary action. The scan is auto-named, the preset lives
in the sheet, renaming lives in the viewer, and leaving the tab already clears
the previous scan's readouts.

Loud banners appear above the body when something is wrong: **NO SENSOR
DATA**, **NO POSITION TRACKING**, **MOUNT REFERENCE NOT SET**.

### 3.2b When position tracking will not start — new in 0.9.12

The phone's camera is the whole third dimension of a COIN-D6 scan (§3.5). If
Google's AR services cannot use it, a card appears on the Scan page saying
which of three things is wrong, instead of the app waiting for a tracker that
is never going to arrive:

| On screen | What to do |
| --- | --- |
| **Update AR services.** | Google Play Services for AR is missing or too old. The button opens its Play Store page. |
| **This phone cannot track position.** | The phone is not on Google's ARCore list. The scanner still records; the scan will be flat. |
| **Tracking camera stopped.** | A session started and the camera was taken away again. **Retry** rebuilds it. |

The third one is the important one, and it is what the first user outside the
owner hit on an **OPPO CPH2499**: some phones' battery-saving software takes
the camera back from AR services a moment after it is handed over. If Retry
does not hold, the two settings worth changing are in the phone's own
Settings, not in Ollidar: **allow Ollidar to use the camera in the
background**, and **turn battery optimisation off for Ollidar**. If it still
fails, **Send logs** from the card — that is genuinely the next step, and no
phone with this fault is on the test bench.

### 3.3 The transport row

At the bottom of the Scan screen:

- **Live view** switch, with the caption *"display only"*. Turning the live
  drawing off does not touch the recording, before or during a scan.
- the running numbers: points · points per second · duration · file size.
- **Pause** — a circle to the left of the big button. Dimmed where pause does
  not work (Mid-360, replay).
- the **posture bubble** — a circle to the right of STOP, drawn only while a
  scan is live. A cross-hair, a thin ring, and a bubble that drifts off centre
  as you tilt: **keep the bubble inside the ring**. It carries both axes at
  once — sideways lean and forward/back lean — and it is green inside the ring
  and amber outside it. The ring is the same 10° tolerance the dial used.
  Sideways lean is measured off square, **not** as absolute roll: held squarely
  in portrait *or* landscape the bubble is centred. A phone lying flat has no
  readable posture, and the circle is then empty rather than showing a guess.

  **New in 0.9.18.** Up to 0.9.17 this was a one-axis needle, so a rig held
  perfectly square but aimed at the floor read as fine. (The needle itself only
  became live in 0.9.15 — before that it was fed the roll measured at the start
  of the scan and never moved.)
- the **big orange button**. It says **SCAN** while idle, **STOP** while
  recording, **CANCEL** during the start sequence. A dot means idle, a square
  means live.
- **Advanced ⚙** — a circle beside the status pill. **As of 0.9.12 the sheet
  has two tabs.** *Scan* is the display sheet: view mode, tracking & camera,
  colour mode, colormap, point size, gamma, brightness, live refresh, and
  **Detail** (§10). *Connection* is the same connection settings that are on
  the page itself — the sensor picker, the USB / Ethernet manual entry, the
  Mid-360 wizard and Diagnostics. It is the same panel in both places, so
  whatever you type in one is what the other shows.

The big button is always tappable. If a press cannot start a scan the app
says why in one short sentence, on screen and in the log:

| Reason on screen | What it means |
| --- | --- |
| "Connect the scanner first." | No sensor is connected |
| "Saving your last scan." | The previous scan is still being sealed |
| "Already starting…" | A start sequence is already running |
| "Heard you. Already starting…" | You pressed again during a start |
| "Already recording." | A scan is live; the button is a STOP button |

### 3.4 The start sequence

Pressing SCAN opens one panel headed **STARTING SCAN**, with the elapsed
seconds and *"usually 4–8 s"*. It steps through four stages, marking them
✓ done, ● live, ○ waiting:

1. **New tracking session** — *"Fresh world frame for this scan — under a
   second."* The app throws away the previous scan's world frame so this scan
   starts from a clean origin.

2. **Locking position tracking** — waits for the phone's tracker to settle.
   Live status: *"Camera warming up — nothing from the tracker yet…"*,
   *"Tracking not locked yet…"*, or *"Steady 1.4 s of the 2 s needed"*. The
   instruction under it is **"Point at furniture, an arm away."** — that is
   the useful thing to do here; pointing at a blank wall makes this stage
   take longer or fail.

3. **Measuring the mount — hold still** — *"Hold the phone still in your
   scanning pose…"*, then *"Steady… 0.4° and improving. Keep holding."*
   Instruction: **"Stand still exactly as you will scan. The walk starts at
   GO."** Usually 1–2 seconds. A **little 3D phone** sits at the top of this
   card, live, inside a dashed target frame: it tilts exactly as your hand
   does, and it is green while your posture is inside the 10° tolerance and
   amber outside it. When it goes amber it says the one thing to do about it —
   *Tilt forward.*, *Tilt back.*, *Level left.* or *Level right.* — and the
   phone gives one short tick. **New in 0.9.18**; before that this was the
   one-axis needle.

   Two things this stage can say instead, and both are the app protecting
   the scan rather than misbehaving:
   - *"Moved a little — measuring again from now. Keep holding…"*
   - **"Tracking is drifting — hold on."** — you were still but the pose was
     sliding underneath you. That is a tracking fault, not a mount reading,
     and accepting it would bake the error into every point.
   - *"Worse reading — holding on."* — the sample was worse than the one
     already stored, so it was refused and sampling continues.

4. **GO — start walking** — the panel header flips to **GO — START WALKING**.
   Now walk. Slowly, smoothly, turning on the spot rather than swinging the
   phone.

Any pre-scan check with something to report appears in the panel in amber.
Checks that pass say nothing.

### 3.5 Stopping

Press **STOP**. The scan is sealed, then:

- a **grade banner** — GOOD SCAN / USABLE / RESCAN, or one of the honest
  failures (NOT RECORDED, NO ROOM — NOTHING WAS PLACED, 2D ONLY — NO ROOM) —
  with one sentence naming the worst thing about the scan, and advice for the
  next walk;
- a **summary** with the numbers and one line saying where it went: *"Saved
  to … — it is in the Projects tab now."*;
- an **auto-process card**, which runs by itself. If it fails, the scan is
  still saved: *"Could not finish. Tap Process to retry."*

Tap **Done** and the app takes you to Projects with the new scan selected.

### 3.6 The page, fullscreen while scanning, and which way up the phone is

**Revised in 0.9.12.** 0.9.11 made the whole Scan screen fullscreen and
floated every control over the live view. That was right while a scan is
running and wrong while it is not — with nothing on the cable there is no
picture to float over, only a black rectangle with a settings form on top of
it. So the screen has two shapes now:

**While you are NOT scanning it is an ordinary page**, from top to bottom:
the status pill (sensor, time, points, metres walked, tracking, device
health) with the Advanced ⚙ beside it; the live preview; the **connection
section in the flow of the page** — scan name, auto-detect, Retry, the USB /
Ethernet panel — which scrolls if it is longer than the room it has; and the
control band with SCAN, pause and the live-view eye. Nothing overlaps
anything. In landscape the same parts become three columns: connection on
the left, preview in the middle, controls on the right.

**While you are scanning it goes minimal and fullscreen.** The live view
fills the screen, and the only things on it are the status pill, the
SCAN/STOP cluster, the stream chip, the **?** and anything that is warning
you. Everything you cannot act on mid-walk — the scan name, the mount
re-zero, New capture, the preset chip — is hidden until you stop.

**The tab bar reserves its own space** while it is visible: nothing is drawn
underneath it. It collapses when a scan starts and the content grows into the
space it gives back.

**The tab bar hides while you are recording** (and while a start sequence is
in flight) and comes back when you stop or cancel. Leaving the Scan tab
still stops and saves the scan exactly as §4 describes — the bar being
hidden makes that harder to do by accident, it does not change what happens
if you do it.

**Both orientations work.** Portrait and landscape are both supported on
Scan, Projects and Review, and the layouts re-anchor rather than stretch —
in landscape the big button moves from the bottom centre to the end centre.

**The start orientation is measured, not guessed, and then locked.** During
the hold-still stage the app works out which way you are holding the phone
**from gravity**, not from Android's rotation setting. That distinction is
the point: hold the phone in landscape with auto-rotate off and the display
still reports portrait, and a scan built on that reading comes out on its
side. It writes what it decided into the capture log:

```
[ar] start orientation: landscape-left
```

At **GO the orientation locks** for the rest of the scan. Turning the phone
mid-walk is scanning motion — it moves the sensor through the room — and the
app treats it that way rather than rebuilding the screen underneath a
running capture.

---

## 4. When tracking is lost

While recording, if the phone's camera stops tracking, the screen dims and a
large amber card appears in the middle:

> **Tracking lost. Stop. Hold still.**
> Lost for 4s.

The seconds count up. There is no dismiss button, no X, no timeout. The card
goes away when tracking comes back — it turns green and says **"OK — keep
walking."** for two seconds — or when you stop the scan.

**What to do: stop walking. Stand still. Do not turn.** The lidar keeps
painting and the gyro keeps measuring, but nothing knows how far you
travelled. Walking through a loss is what makes a gap unrepairable. In one
recorded field scan, 4.1 seconds of blindness contained 73° of real turn
where the tracker had reported 12.7°, and the join had to be refused.

**STOP is still tappable through the card.** The dimmed background does not
swallow touches, on purpose: if a scan has gone bad you must always be able
to abandon it.

### Leaving the Scan tab ends the scan — new in 0.9.10

Tapping any other tab (Projects, Jobs or Settings) while the Scan tab is live
does three things:

1. a scan that is **recording or paused is stopped and saved** by the normal
   save path — the manifest is written, empty scans are pruned, and
   auto-processing carries on in the background;
2. a **start sequence in flight is cancelled** cleanly;
3. **tracking is shut down** — the ARCore session is closed and the camera is
   released, so a backgrounded Scan tab costs no camera and no battery.

You will see a green **"Scan saved."** note at the top of Projects, with the
scan's name, the first time you look at that tab afterwards.

Rotating the phone is not leaving, and as of 0.9.11 it does not rebuild the
screen either: the orientation is locked from GO until you stop (§3.6). Turn
the phone mid-scan and the app reads it as the sensor moving, which is what
it is.

**Re-entering the Scan tab always starts a fresh scan.** It never resumes.
If you want to check Projects mid-walk, the honest advice is: don't. Finish
the scan first.

---

## 5. Projects

The list of every scan on the phone, newest first. The header line counts
them and their points, and names any empty scans that are hidden.

Empty state: **"No scans yet"** / *"Plug in the scanner. Tap Scan."* with a
**Start a scan** button.

### Layout and sort

One compact control row sits above the list, and it only appears when there
is a list.

- **Layout toggle** (left). Two layouts, remembered across restarts:
  - **List** — one full-width row per scan. **As of 0.9.10 the list rows have
    no preview image**: name, chips and grade only, and the rows are tighter
    now that nothing sets their height. This is the default.
  - **Gallery** — two columns of thumbnail-first cards. The gallery **keeps**
    its preview image; it is the whole reason the toggle exists.

  The icon shows the layout a tap would give you, and is described as "Show
  as list" / "Show as gallery".

- **Sort menu** (right), showing its current value: **Newest** (default),
  **A–Z**, **Z–A**. Remembered. Sorting is stable — two scans taken in the
  same millisecond keep their order — and case-insensitive by name.

### Tapping, and the ⋯ menu

- **Tap a card** to open it in the viewer. The hint under the list says
  *"Tap a scan to open it."*
- **The ⋯ menu on each card** carries **Export**, **Share**, **Process
  again**, **Delete** (and **Details** when Lab features is on). Export
  and Share open the scan, where the format row lives.
- **Delete** asks first: **"Delete this scan?"** / *"The scan and its data
  go. Cannot undo."*
- A running job shows as a small progress chip on the card it belongs to.

### Selection mode (group export and share)

- **Long-press a card** to enter selection mode and pick it. Long-press is
  no longer a shortcut to delete.
- A bar appears at the top with the count and four controls: **X** to leave
  the mode, **Export**, **Share**, **Delete** (in red).
- Tapping cards toggles them in and out of the selection.
- Export runs one job per scan, in the order you are looking at them. Share
  gathers the results into a single Android share sheet at the end. A failure
  in the middle does not stop the rest — the bar reports what happened.
- Group delete uses the same confirmation dialog, with a count.

---

## 6. The viewer (Review)

Tapping a scan opens the point cloud. The title bar carries the scan's name,
its point count and whether it is georeferenced or in a local frame, plus two
icons: **Measure** (a ruler) and **Display settings** (sliders).

While the cloud loads: *"Opening your scan…"*. If there is genuinely nothing
in it: *"Nothing recorded in this scan."*

### Fullscreen — new in 0.9.11

The point cloud is edge to edge and the controls float over it: back at the
top left, display and measure at the top right, and a strip along the bottom
with the colour mode, **Export** and **Share**.

**A tap on empty space hides every control; another tap brings them back.**
The gestures below keep working while the controls are hidden, so you can
orbit, pan and zoom a completely bare cloud and then tap once to get the
buttons back.

The one exception is measure mode: while the ruler is on, a tap is how you
place a measurement point, so it places one and the controls stay put. Turn
measure off to get the hide-and-show tap back.

Both orientations work here too.

### Gestures — new in 0.9.10

| Gesture | What it does |
| --- | --- |
| **One finger drag** | Rotate / orbit around the scan |
| **Two finger drag** | Pan — slide the view sideways and up/down |
| **Pinch** | Zoom in and out (moves the camera toward or away) |
| **Double tap** | Reset the framing to the whole scan |

Before 0.9.10, pan did not exist and the viewer behaved like a turntable
nailed to the session origin — unusable on a corridor whose geometry sits
thirty metres away. Double tap frames **what is actually there**, not a fixed
home position, so it is a reliable escape when you have lost the cloud
off-screen.

Zoom is clamped at both ends (roughly 12 cm to 2 km from the target) so a
pinch cannot push the camera through the cloud and out the far side.

### Measuring

Tap the ruler icon. A chip appears: **MEASURE ON · TAP A POINT**. Tap two
points and the distance is shown, with the caveat the app states itself:
*"Picks the nearest point. Off by a few centimetres."* Unit chips (**m** /
**ft**) and a **Clear** button sit under the read-out. Feet are shown as feet
and inches below 100 ft.

**The gestures still work while measuring.** A tap measures; a drag orbits;
two fingers pan; a pinch zooms. Before 0.9.10 turning measure mode on froze
the viewer completely.

### Export and Share

Two buttons side by side under the viewport, with a format row under them:
**PLY**, **LAS14**, **PCD**, **DXF**, **PDF**. The first three are point
clouds; DXF and PDF are floor-plan outputs and are only meaningful for a scan
that has a plan.

- **Export** writes the file to your Downloads folder — *"Saves to your
  Downloads folder."* Then: *"Exporting…"* → *"Saved scan-071.ply to
  Downloads."* On failure it says what happened and what to do.
- **Share** does the same write, then opens the Android share sheet.
- LAS 1.4 is the georeferenced format. If the scan has no georeference, the
  format row warns you before you export.

The display panel (sliders icon) holds colour mode, colormap, point size,
gamma, brightness, clipping, background, "Show my path", and the Detail
budget (§10).

**Height colouring is Turbo** — dark blue at the bottom, through blue,
green, yellow and orange, to red at the top. Grey and the other ramps are
still in the display panel and can be picked at any time. The same ramp is
used in the live view while you scan.

0.9.11 made Turbo the *default*, which turned out to reach nothing: every
scan already on the phone had `Grey` saved against it, so switching Colour
mode to Height still gave a grey cloud. **0.9.12 migrates them**, once each:
a scan whose height colour was grey becomes Turbo the first time 0.9.12 opens
it, a scan whose height colour was anything else is left exactly as it is,
and grey chosen *after* that is respected and never overridden again.

---

## 7. Profile, Send logs, Feedback

The person icon at the top right of Projects opens **Profile**. So does the
**same icon at the top right of Settings** (new in 0.9.14 — it replaces the
Profile *row* that used to sit in the ABOUT card; an icon in the header and a
row in the body were two doors to one page on one screen).

### This phone

A block of facts, in a mono font because you will read them out loud or paste
them into a message:

| Row | Example |
| --- | --- |
| APP | `Ollidar 0.9.12 (912)` |
| DEVICE | `Google Pixel 8 Pro` |
| ANDROID | `14` |
| ENGINE | `ABI 12`, or `not loaded` |
| SCANS | `17` |
| STORAGE | `1.2 GB` |

These same six lines are what gets sent. What you can see is exactly what
leaves the phone.

### Send logs

One button, **Send logs**, with the whole disclosure above it: *"Sends your
logs and device info."* Under that, a line saying which way it will go —
*"Opens the share sheet."* or *"Sends to your server."*

**What is in the bundle** (a zip named `lidarscan-logs-YYYY-MM-DD-HHMM.zip`):

- `device.txt` — the six lines above.
- `capture.log` — the app's own capture log, including its `[crash]` entries.
  The previous rotation of the log is included too, so the archive reads
  forward in time.
- `feedback.txt` — only when you typed something in the Feedback box.

Nothing else is read from the phone. No account, no location, no advertising
identifier.

**Where it lands.** The zip is written to **`Downloads/LidarScan`** *first*,
before any attempt to send it. That is deliberate: the share sheet gives no
result callback and an absent server fails after a timeout you have walked
away from, so the one thing the app can guarantee happens first. You can
always find the file and send it by hand.

**Where it goes — you pick (0.9.14).** Tapping **Send diagnostics** opens a
sheet with three doors, and a fourth when a cloud server is configured:

- **GitHub** — the zip is written to Downloads and your browser opens on a
  prefilled issue at `github.com/Agtom-FN/Ollidar`, with the device table
  already filled in. A link cannot carry a file, so the issue body names the
  zip and asks you to drag it in before you post. The app holds no GitHub
  credential; the issue is posted from your own browser session, under your
  own account, when you press the button in it.
- **Save to phone** — the zip and nothing else, with the path on screen.
- **Share…** — the Android share sheet: mail, chat, Drive, anything.
- **Your server** — only when Settings › Lab features › the cloud fields are
  filled in. It POSTs the zip exactly as it always did.

Progress is shown on the card while it runs, and the job survives you leaving
the screen. The result line is honest about which of those actually happened:
**"Opened GitHub. Post it there."**, **"Saved to …"**, **"Sent."** or
**"Could not save the zip."**

### Feedback

**Send feedback** opens a sheet with one labelled box — *"What went wrong?"* —
and one button, **Open GitHub**. It does not upload anything: it opens a
prefilled issue in your browser with your text, the six device facts from the
This-phone table, and a note saying logs can be attached. Nothing leaves the
phone until you press **Submit new issue** yourself.

Very long reports are trimmed to fit a URL, and the issue says where it was
cut — attach the diagnostics zip for the rest.

---

## 8. The tutorial

Six steps, each one dimming the screen, ringing one control, and explaining
it in a sentence:

| Step | Rings | Says |
| --- | --- | --- |
| 1 of 6 | the SCAN button | "Tap SCAN to start." — *Tap it again to stop. Your scan saves itself.* |
| 2 of 6 | the SCAN button | "Hold still after tapping." — *It measures the mount for a few seconds. Then walk.* |
| 3 of 6 | the chip row | "These chips show your state." — *Sensor, tracking and scan name, at a glance.* |
| 4 of 6 | the Advanced ⚙ | "Advanced holds the settings." — *Detail, display and the reset all live here.* |
| 5 of 6 | the viewport | "If tracking is lost, stop." — *An amber card appears. Stand still until it clears.* |
| 6 of 6 | the Projects tab | "Finished scans land in Projects." — *Tap one to open it, export it or share it.* |

**Skip** leaves at any point. The forward button says **Next**, and **Done**
on the last step.

**Three ways in:**

- the **?** circle at the start of the chip row on the Scan screen;
- the **first-run offer** — a card at the top of the Scan screen the first
  time you open it after installing: **"New here? Take the tour."** with
  **Take the tour** and **No thanks**. It is offered exactly once, ever, and
  taking the tour by any other route also retires it;
- **Settings › About › Tutorial** — *"Walk through the Scan screen again."*
  This hops to the Scan tab and runs the tour there, because a tour of a
  screen has to happen on that screen.

The tutorial overlay does consume taps, unlike the tracking-lost card. It is
a mode you entered on purpose, and a stray tap on the real SCAN button behind
the explanation of the SCAN button would start a scan nobody asked for.

---

## 9. Settings map

Round 24 collapsed twelve headings into a short page. Nothing was deleted;
developer-only items moved behind the seven-tap unlock.

**The header**
- the **person icon**, top right → **Profile** (§7). Same component, same
  corner and same page as the Projects avatar.

**Scanning**
- **Where the D6 sits** — the mount profile. Says whether the rotation has
  been measured (*"Rotation measured: 1.4°."* / *"Rotation not measured
  yet."*) and holds three lever-arm fields in centimetres: Up, Behind, Right
  — the D6's offset from the phone's rear camera. **Reset offsets** restores
  the default. An auto-level result from processing appears here as a
  *suggestion* with its provenance, never applied silently.
- **Vibrate and beep** — *"The phone faces away while you walk. These are the
  hints."*
- **Silence notifications** — Do Not Disturb during a scan, with the Android
  permission grant row.
- **Detail** — Auto / High / Max. See §10.

**Storage**
- **Keep empty scans** — *"Off: a scan with no points is deleted at Stop."*
  With a **Clean up** button and a count.
- the storage location — the path, and *"Where your scans are kept."* There
  is no location picker.

**Display**
- **Units** — Meters / Feet.
- **Theme** — System / Light / Dark. Dark is the default.
- **Welcome animation** — *"Plays once when the app starts."* **On by
  default.** Three seconds of the Ollidar llama, once per app start — not on
  tab switches, not when you rotate the phone. **Touch anywhere to skip it**;
  it goes at once and the app is already loaded underneath. If your phone has
  animations turned off (Accessibility › Remove animations, or the developer
  animation scales set to zero) it never plays at all, and this switch is left
  alone. New in 0.9.17.

**Lab features**
- One switch, **default off**. *"Floor plan, merge, cloud, survey
  tools."* (It was called **Advanced features** until 0.9.14. The rows behind
  it are unfinished, not expert — and "Advanced" collided with the Scan tab's
  Advanced ⚙ sheet, which is a different thing entirely.)
  Nothing behind it is deleted; it is hidden. Turning it on brings back the
  floor plan, the merge screen, the Survey and Research display profiles, the
  cloud processing mode and the per-project Details hub.
- With the switch on, the **cloud server** fields appear here: Server URL and
  Token. *"Used for cloud processing and for sending logs. The token is
  stored unencrypted on this phone."*
- RTK and the Mid-360 wizard are **not** behind this switch — they appear on
  the Scan tab whenever a Mid-360 is the selected sensor.

**About**
- **Tutorial** — replays the tour (§8).
- **Camera is used for tracking** — *"No images are saved. Nothing leaves
  your phone."*

**The version footer**

At the very bottom: `Ollidar v0.9.18 (build 918)`. **Tap it seven times**
to unlock a **Developer** section, and seven more to lock it away again. The
counter resets when you re-lock, so a single stray tap afterwards does not
re-open it.

Developer holds: the per-capture debug log switch, the capture-log card (path,
size, last line, Export log, Clear), the D6 sensor-latency slider, the
simulated-engine switch, "Replay synthetic capture", the **Connection debug**
sweep, and the read-only workflow-profile reference. None of it is needed for
scanning. For getting a log to someone, **Profile › Send logs** is the better
door.

**What Developer mode records about a serial scanner (0.9.17).** With it on,
every serial port the app opens writes one `[net-debug]` line carrying the
device's `vid:pid`, its product string, the baud it was opened at, **the state
of the DTR and RTS control lines** (`dtr=1 rts=1`) and **the first 64 bytes it
actually sent**, as hex. Every probe attempt writes another line with the
counters it decided on — for the D6, how many `AA 55` pairs, how many complete
frames and how many correctly chained; for the STL-27L, how many `54 2C`
headers and how many survived the CRC, **at every speed that was tried**:

```
921600:bytes=16,542c=0,packets=0 230400:bytes=34104,542c=712,packets=709 (adopted 230400)
```

The control-line state and the per-speed counters are new in 0.9.17 and they
exist for one reason — see §13's silent line.

That is deliberately the raw evidence rather than a verdict. `54 2C ...` at
921600 is an STL-27L; `AA 55` with a sane length byte at 230400 is a COIN-D6;
printable ASCII is a GNSS receiver on the same connector; all zeros with a
byte count in the tens is a **silent line** — the sensor may be spinning
perfectly and not be reaching the phone at all (§13). None of those can be told
apart from "No scanner found." If a scanner is not recognised, turn Developer
mode on **before** you plug it in, plug it in once, and send the log.

---

## 10. Detail, and the device ceiling

**Detail** decides how many points the app draws. It never changes what is
recorded — not one byte of any scan file depends on it.

Three settings: **Auto**, **High**, **Max**, in that order, and they ascend.
Auto is the default and is the right answer unless you have a reason.

| Setting | What it asks for | Reads out |
| --- | --- | --- |
| **Auto** | what this phone was measured to be good for | **Fits this device** |
| **High** | the full budget, clamped to the phone | e.g. `16 M` |
| **Max** | everything the phone can hold | e.g. `16 M` |

**Auto shows no number**, on purpose: it does not have one — it adapts. Until
0.9.14 it showed the tier ceiling, which is *Max's* number, so a Standard
phone read "Auto 16 M, High 5 M" and the ladder ran downhill. That was a
mapping bug and it is fixed; the ceiling law below is unchanged.

It appears in three places, and they are the same setting: Settings ›
Scanning › Detail, the Scan tab's Advanced ⚙ sheet, and the viewer's display
panel ("Detail budget"). The one-line explanation is *"More detail needs more
memory."*

### The phone decides the maximum, and there is no override

The app measures the phone at startup (RAM, cores, refresh rate) and sorts it
into one of three tiers. Each tier has a hard ceiling on how much point data
may live on the GPU at once:

| Tier | Ceiling | Points |
| --- | --- | --- |
| Modest | 96 MiB | ~6.3 million |
| Standard | 256 MiB | ~16.8 million |
| Flagship | 512 MiB | ~33.6 million |

Any Detail setting above your phone's ceiling is **not shown at all** — not
greyed out, absent — and a four-word note appears under the row:
**"Limited by this device"**. On a Modest and on a Standard phone, High and
Max clamp to the same number, so two rungs are offered rather than three; a
Flagship gets all three. That is the honest presentation of a ladder whose top
is the hardware.

A scan whose saved display settings came from a bigger phone is clamped when
you open it, silently and safely.

**Why there is no override:** an earlier version let you ask for 50 million
points on a phone that could hold 6.3 million, and the app died. Not "got
slow" — the process was killed mid-scan. A setting whose only function is to
crash the app is not a setting worth having, so it was removed rather than
made harder to find.

---

## 11. Mounting the COIN-D6

### The physical convention

- **Flat on the back of the phone**, rigidly, so it cannot shift while you
  walk.
- **Zero mark up.** The 0° mark on the D6 housing points toward the top of
  the phone.
- **Cap forward.** The spin axis points out of the base, away from the phone.
- The result is a **vertical scan fan**. The app says it in five words:
  *"Mount flat. Keep the fan vertical."* / *"The camera tracks where you
  walk."*
- **Do not obstruct the rear camera.** The camera is the tracker.

If you get the orientation wrong the scan comes out mirrored, which is
obvious once you look at it and invisible while you walk.

### Why the mount is measured at every start

The D6 is clamped on by hand and comes off between sessions, so its real
angle on the phone differs every time — and that angle lands in **every**
resolved point. A one-degree error at ten metres is 17 centimetres.

So the hold-still stage of the start sequence measures it, using the phone's
own attitude referenced to gravity, and applies it to that scan only. That is
the "re-zero at Start" behaviour: the app never trusts a hard-coded bracket
figure, and never trusts an old measurement blindly.

The measurement is defended:

- a candidate that is **materially worse** than the one already stored is
  refused, and sampling keeps going until the timeout;
- a hold during which the pose **drifted** is refused outright — very low
  frame-to-frame jitter with a bad split-half number means the phone was
  still and the tracking was not, which is a tracking fault and not a mount
  reading;
- if nothing good arrives before the timeout, it falls back to the stored
  value and says so.

You can also set it by hand from the ready screen: **Set mount reference**
(or **Re-zero mount** once one exists) with **Clear** beside it. *"Hold
still, then tap."* / *"It measures the D6's angle on your phone."* The status
line under it carries the age and where the value came from, so a trim
restored across an app restart is not presented as one you just took.

The **lever arm** — how far the D6 sits from the rear camera, in centimetres
up / behind / right — is a separate thing and is typed in Settings › Scanning
› Where the D6 sits. It does not change between scans unless you change the
bracket.

---

## 12. Livox Mid-360 setup

The Mid-360 is a network sensor. The phone talks to it over Ethernet, not
USB, and it will not work until the phone's Ethernet interface holds one
specific IP address.

### What you need

- a **USB-C Ethernet adapter** the phone can actually drive — see
  "Which adapter to buy" below, because this is where it goes wrong.
- an Ethernet cable from the adapter to the Mid-360.
- the Mid-360's own **9–27 V power supply** (about 6.5 W). It does not draw
  power from the phone.

### Which adapter to buy

The same recommendation is in the README and on the wizard's own no-adapter
screen, so all three say one thing.

- **For the lidar alone: a plain, single-purpose USB-C gigabit Ethernet
  adapter** built on a **Realtek RTL8153** (for example the TP-Link UE300C)
  or an **ASIX AX88179** chipset. Those are the two families Android ships
  kernel drivers for, and they run **unpowered** straight off the phone.
- **Avoid multi-port laptop hubs as a first attempt.** They frequently need a
  charger plugged into their **USB-C PD port** before the Ethernet chip
  powers up at all — plugged into a phone alone, nothing enumerates. That is
  exactly what happened with the owner's **Acer HY41-T9**: the app said
  "No Ethernet adapter found." because, as far as the phone was concerned,
  there was no adapter.
- **For lidar and charging at the same time: a powered USB-C hub with
  Ethernet** (Anker 341 / 343 class), with the charger in the hub's PD port.

**None of this has been tested on this project's phone.** No Ethernet
interface has ever come up on Android here — not once — so the list above is
reasoned from Android's driver support and from the one hub that failed, not
from a shelf of adapters that were tried. Treat it as *recommended,
untested*. If you buy one and it works, or does not, that is worth putting in
a field report along with the wizard's USB device list.

### The address, and why you have to type it

A Mid-360 does not broadcast its point stream. It **unicasts** to a host IP
it was configured with and remembers — by default **192.168.1.5**. If the
phone's Ethernet interface does not own that exact address, the packets are
addressed to a machine that is not on the cable and nothing arrives, forever,
with the link light on and the cable perfectly seated. Android's USB Ethernet
uses DHCP by default, and there is no DHCP server on a lidar cable.

Android exposes no public API to set this, so the app cannot do it for you.
Set it in **Android Settings › Network & internet › Ethernet › IP settings ›
Static**:

- IP address: **192.168.1.5**
- Netmask: **255.255.255.0**

(If your unit is configured for a different host, use that address. The
wizard shows the one the lidar itself is asking for whenever it has heard a
heartbeat.)

### The setup wizard

Reach it from the **Mid-360 setup** chip on the Scan tab, which appears
whenever a Mid-360 is the selected sensor. Lab features does not need to
be on.

The wizard runs **Auto-detect** — it listens for the Mid-360's own heartbeat
broadcast on UDP 56201 for a few seconds, so there are no addresses to type
if the network is already right. It also carries a manual panel (Lidar IP and
Host IP), the interface read-out, and per-OEM guidance for reaching the
static-IP screen.

### The Ethernet check — new in 0.9.10

The wizard leads with a stepwise diagnosis headed **"Ethernet check"**. It
walks the physical chain in order and stops on the first rung that fails,
because the rung you are stuck on *is* the instruction. Every state has a
**Retry**, and the wizard keeps polling while it is open, so plugging a hub
in updates the screen without you doing anything.

| State | On screen | What to do |
| --- | --- | --- |
| **No adapter, nothing on USB** | "No Ethernet adapter found." / *"Nothing is plugged in. Try a powered USB-C hub."* | Nothing at all is enumerating. Check the cable and the hub's own power. |
| **No adapter, but USB devices are seen** | "No Ethernet adapter found." / *"USB works, this adapter does not. Try a powered hub."* | The phone sees USB devices but no Ethernet. Either the kernel has no driver for this adapter or it is browning out on bus power. Both end at a powered hub — or a different adapter. |
| **Adapter, no link** | "Adapter found, no link." / *"Check the cable, then the lidar's own power supply."* | The adapter came up, the network did not. |
| **Link, no address** | "Link up, no address." / *"Set a static IP: 192.168.1.5, mask 255.255.255.0."* | There is no DHCP on this cable. Set it by hand. |
| **Wrong network** | "Wrong network for the lidar." / *"Set a static IP: 192.168.1.5, mask 255.255.255.0."* | The interface holds an address on a different subnet. |
| **Right network, wrong address** | "Right network, wrong address." / *"The lidar streams to 192.168.1.5. Set that."* | Being a *neighbour* of the right address is not enough — the lidar unicasts to one host. |
| **Address set, lidar silent** | "Address set, lidar silent." / *"Check the lidar's power, then retry."* | Addressing is correct and nothing has been heard. Runs the heartbeat discovery with live progress and reports what it heard. |
| **OK** | "Mid-360 heard. Ready." / *"This phone holds 192.168.1.5. Start the scan."* | Go. |

In the first two states the wizard lists **every USB device the phone does
enumerate** under "USB devices seen" (or *"None. Nothing on USB."*). That
list is the evidence that separates "nothing is plugged in" from "this
adapter did not come up", and it is worth putting in a field report.

**As of 0.9.11 those two states also carry the adapter recommendation** —
"Which adapter to buy", above — as a secondary block under the diagnosis,
naming the RTL8153 and AX88179 chipsets, the PD-port trap on laptop hubs,
and the fact that none of it has been tested here. It is longer than the two
lines above it on purpose: those two are the instruction, this is the
shopping list, and the screen keeps them apart.

Where the fix is an Android setting, an **Open Ethernet settings** button
appears — when Android on that phone resolves the deep link.

### Honest limits

- **No Mid-360 has ever come up over Ethernet on this project's phone.** The
  whole pipeline is proven from the desktop, and the phone side is proven as
  far as the diagnosis; the adapter itself is the open question, and the
  advice above is untested.
- Passing every check does **not** guarantee data. A known remaining gap is
  routing rather than addressing: with Wi-Fi up, the kernel may still send
  the lidar's sockets over the wrong interface. If a Mid-360 scan records
  zero bytes with a green Ethernet check, turn Wi-Fi off and try again.
- The app's no-data watchdog is the backstop: a session that records nothing
  says so within a couple of seconds rather than sealing a scan of nothing.

---

## 13. STL-27L (LDROBOT)

> **Status: supported — bench validation pending, second retest owed.**
> The driver is code-complete against the published LD-series protocol and is
> covered by byte-exact synthetic fixtures, including its CRC. **No STL-27L
> hardware has ever been connected to this Mac.** One has been connected to
> the app, twice, on the owner's phone: the 0.9.15 attempt failed three ways
> (all fixed in 0.9.16) and the 0.9.16 attempt failed a fourth (a silent line
> — see below, fixed in 0.9.17). Treat the next real unit as a test, not as a
> working sensor, turn Developer mode on **before** you plug it in, and send
> the logs afterwards.
>
> The engine driver, its fixtures and `engine_cli --sensor stl27l` are in
> place, and the app can select an STL-27L. The C ABI did not change for it
> (it is still 12), so an engine and an app from the same build always agree.

### What changed in 0.9.16, and why

The first real STL-27L was connected to the app on 2026-08-22. It did not
work, in three separate ways, and all three are fixed here. **The owner's
retest is this section's bench validation** — nothing below has been seen on
hardware; it has been proved against byte-exact synthetic streams through the
same code the phone runs.

**1. Auto-detect called it a COIN-D6.** The D6 probe used to accept the two
bytes `AA 55` appearing anywhere in its 1.5-second listening window. That
window is about 34,500 bytes, so those two bytes turn up in roughly **41% of
pure noise** — and an STL-27L's 921600 stream read at the D6's 230400 is
exactly that kind of noise, dense in alternating bits. The probe now requires
four complete, well-formed D6 frames with at least one correctly chained pair.
A real COIN-D6 clears that in about 40 milliseconds; noise does not clear it.

**2. Choosing STL-27L by hand did nothing.** The auto-detect probe leaves the
port open when it identifies a sensor, and the manual connect then tried to
reopen the same port at a different speed without releasing it first. Android
refuses that, so the manual choice failed and the wrongly-detected D6 kept
running. A manual pick now releases the previous connection before reopening.

**3. Nothing in the log said what had happened.** See §9's Developer-mode
note: `vid:pid`, the first 64 bytes off the wire, and the counters each probe
decided on.

There is also a new honest answer where there used to be a wrong one: when the
port produces fragments of **both** protocols and neither reaches its bar, the
app says *"Can't tell which scanner this is"* and asks, instead of claiming
whichever probe ran first.

### What changed in 0.9.17: the silent line

The owner retested on 0.9.16 the same evening. **All three of the above
worked** — the manual pick bound, the log said
`[session] sensor: LDROBOT STL-27L (manual)`, the project recorded
`sensor=STL27L`, the D6 probe honestly declined, and the first-bytes evidence
line appeared. And the scan still recorded nothing, for a fourth reason
nobody had looked for.

**The line was silent.** Not garbled — *absent*. 42 bytes in two seconds. 552
bytes in twenty-five. The first byte was `00`. Not one `54 2C` header ever
arrived, at any point, in any window. **And the sensor was spinning.**

A sensor that is powered, enabled and turning while its host receives nothing
is not a baud problem and not a protocol problem. Something is holding the
data path down. Two things were, and 0.9.17 fixes both.

**1. DTR and RTS were low.** Every serial port this app has ever opened ended
with `setDTR(false)` and never mentioned RTS, which the driver leaves low
after opening. On a bare COIN-D6 that is invisible — a hundred field scans
prove it. The STL-27L arrives on a **CH340 dev-kit adapter board**
(`1a86:7523`, product string `USB Serial`), and boards of that class commonly
wire DTR and/or RTS to the sensor's enable line or to the level shifter's
output-enable. Both low means the board is listening to a spinning sensor and
telling the phone nothing.

From 0.9.17 an STL-27L port is opened with **DTR and RTS both asserted**, on
the auto-detect path and on the manual pick alike. **The COIN-D6 is
deliberately unchanged** and keeps both lines low: that is the state every
recorded scan was taken with, and on some adapters DTR is a *reset* line, so
changing the sensor that works to fix the one that does not is not a trade
worth making. Developer mode records which state each port was opened in
(`dtr=1 rts=1`).

**2. The datasheet's speed is now a hypothesis, not a fact.** The STL-27L is
documented at 921600. The LD06 and LD19 in the same family run **230400**, and
several STL-27L dev-kit and clone batches ship at the family default. So when
the probe hears a **silent** line at 921600 — under 256 bytes in its window —
it now re-opens at **230400** and then **460800** and listens again. If one of
them produces CRC-valid packets, *that is the speed the scan runs at*, end to
end (the phone's divisor and the number the engine is told are the same
number), and the log says so:

```
[session] STL-27L at 230400 (non-standard)
```

A **loud** line carrying the wrong protocol does not get this treatment, on
purpose: something is streaming and it is not an STL-27L, and re-clocking a
working COIN-D6 port twice on every connect to ask a question that has already
been answered would be churn on the one path every recorded scan came through.

Under developer mode the `[net-debug]` block now carries every speed that was
tried, with its counters:

```
921600:bytes=16,542c=0,packets=0 230400:bytes=34104,542c=712,packets=709 (adopted 230400)
```

**Still unverified.** No STL-27L exists on this machine. Both fixes are proved
against byte-exact synthetic streams through the same decision code the phone
runs, and against a mock port for the two control-line writes. If the retest
fails again, the line above is what to send.

### What it is

The LDROBOT STL-27L is a 360° spinning DTOF lidar — the same *kind* of sensor
as the COIN-D6, with more range and more points:

| | STL-27L | COIN-D6 |
| --- | --- | --- |
| Link | USB-serial, **921600 baud, 8N1** | USB-serial, 230400 baud, 8N1 |
| Rate | ~21,600 points/s at ~10 Hz | ~4,000 points/s at 10 Hz |
| Range | 0.02–**25 m** | 0.05–12 m |
| Packet | LD-series 47-byte frame, 12 points, CRC8 | COIN-D6 vendor frame, XOR checksum |

Like the D6 it is a flat scanner with no idea where it is, so the phone's
camera supplies the trajectory and the app sweeps the fan into 3D. **It is
used exactly like a COIN-D6:** the same mounting convention, the same
hold-still mount measurement at every start, the same SCAN / STOP, the same
tracking-lost rules, the same viewer and export.

It free-runs the moment it is powered — there is no start or stop command to
send, so there is no handshake to fail. A stalled unit is reported as
degraded and left for you to power-cycle.

### What is unverified, and what to watch for

Everything in the driver is derived from the public LD-series protocol
references, not observed on hardware:

- the 47-byte frame layout and field order,
- the CRC8 parameters,
- the 30,000 ms timestamp wrap,
- **which way the reported angle sweeps.**

That last one is the one that will bite. If the STL-27L's angle increases in
the opposite rotational sense to the COIN-D6's, **the scan will come out
mirrored** — and a mirrored room looks entirely plausible until you find an
asymmetric feature in it. The fix is a one-line configuration change in the
driver (`invert_angle`), not a rebuild; it is deliberately a knob rather than
a second copy of the geometry.

**First bench test:** scan a room with something obviously asymmetric in it
— a door on one side, a staircase, an L-shaped corridor — and check the
result against reality before trusting any measurement. Then read the
driver's counters (bad-CRC packets, malformed packets, resyncs) from the Diag
chip: a healthy link should show a CRC pass rate at or above 99%.

**Do this on the very first scan, not the tenth.** A mirrored room is the one
failure on this list that looks completely correct: walls in the right places,
distances that measure sensibly, and everything reflected. If the door is on
the wrong side of the resulting scan, the sensor sweeps the other way and the
one-line `invert_angle` change is the whole fix — nothing else about the scan
needs to be re-derived, and no scan taken before the change is salvageable by
any other means.

**Before that scan, turn Developer mode on** (§9). If the first plug-in does
not produce a live preview, the `[net-debug]` line with the first 64 bytes is
what tells us whether the phone saw LD-series packets at all — and that is the
difference between a protocol problem, a speed problem and a cable.

---

## 14. Troubleshooting

### The SCAN button will not start anything

It is never silent. The app writes the reason on screen in one short
sentence and puts the same sentence in the log. See the table in §3.3. The
usual answer is "Connect the scanner first."

A standing reason is shown *before* you press, too, so a dimmed-looking
button is never a mystery.

### No scanner found

- Check the USB permission prompt — Android asks once per device.
- Unplug and re-plug, then tap **Retry**. The line reads *"No scanner found.
  Plug it in, then Retry."*
- Try the cable. USB-C cables that only charge are common.
- If the phone shows nothing at all on USB, try a powered USB-C hub.
- For a Mid-360, this is an Ethernet problem, not a USB one — go to §12.
- **Enter manually** lets you pick the serial device directly, or type the
  Mid-360's addresses.

### The app found the wrong scanner

Open **Enter manually**, pick the right one on the **D6 / STL-27L** selector,
choose the device and tap **Connect**. From 0.9.16 that choice binds: the app
releases whatever it had connected, reopens the port at the sensor you named,
and the log records `sensor: <name> (manual)`.

If the line above the panel reads *"Can't tell which scanner this is"*, the
app has already decided not to guess. Same answer: pick one.

If picking the right one still does not produce points, turn Developer mode on
(§9), unplug and re-plug once, and send the log. The `[net-debug]` line with
the device's `vid:pid` and its first 64 bytes is what identifies the device
without it being in the room.

### Tracking keeps dropping

Almost always the scene, not the phone:

- **Get further from the surfaces.** Losses cluster where most returns are
  under about 1.5 metres. Stand back.
- **Point at furniture and edges, not at blank walls.** The start panel says
  exactly this: *"Point at furniture, an arm away."*
- **Turn slowly, and turn on the spot.** Fast rotation is the other reliable
  way to lose the tracker.
- **Do not cover the rear camera** with your hand, a case or the bracket.
- If it says **"Phone tracking degraded."** — *"Recording continues. Stop and
  start again if it lasts."* — that is exactly the advice. The recording is
  not damaged; the trajectory may be.
- Check ARCore ("Google Play Services for AR") is installed and up to date.
  Without it, the D6 can only record flat slices: *"No tracking. Scans would
  be flat."* / *"Grant the camera permission for 3D."*

**When it does drop mid-walk: stop. Stand still. Wait for the green card.**
That is the whole technique, and it works. Walking on is what makes the gap
permanent.

### A scan recorded nothing

The app tells you rather than grading an empty file. **NOT RECORDED**, **NO
ROOM — NOTHING WAS PLACED** or **2D ONLY — NO ROOM** on the grade banner, and
the summary line says *"Nothing was written for this session."*

Common causes, in order:

1. **the sensor never streamed** — the connection said connected but no bytes
   arrived. For a Mid-360 this is nearly always the addressing (§12). Check
   the Diag chip's points/sec while idle: it should be non-zero before you
   press SCAN.

   **If the banner says *"Sensor is silent. Is it spinning?"*** — that is a
   different fault from a wrong speed, and the app is asking you the one
   question that splits it, because you can answer it in a second by looking:

   * **it is not spinning** → it is not getting 5 V. A phone's USB-C port is
     the usual reason; try a powered hub, and check the sensor's own power
     lead rather than the data cable.
   * **it is spinning** → the sensor is fine and the *data* is not arriving.
     Try a different USB cable first (a charge-only cable powers a sensor
     perfectly and carries no data), then the adapter board and its TX wiring.
     From 0.9.17 the app has already asserted DTR and RTS and already retried
     at 230400 and 460800 before showing you this — see §13 — so those three
     are ruled out by the time you read it.

   Either way, turn Developer mode on (§9) and send the log: the
   `[net-debug]` line records the adapter's `vid:pid`, the control-line state
   and the byte counts at every speed tried, which is what makes the next
   attempt start from evidence.
2. **the phone never tracked** — no ARCore, camera permission denied, or the
   camera was covered. Without a trajectory a flat scanner produces flat
   slices and no room.
3. **the scan was too short** to place anything.

Empty scans are deleted at Stop unless **Settings › Storage › Keep empty
scans** is on. If it is off and a scan you expected is missing, that is why —
the Projects header line counts hidden empty scans.

### Auto-processing failed

The scan is saved and untouched. The card says *"Could not finish. Tap
Process to retry."*

**Fixed in 0.9.12.** Until 0.9.12 that instruction pointed at a button that
did not exist: opening the scan showed *"No cloud in memory. Run Process on
this project…"* and there was no Process control anywhere on the Review
screen, and the one retry that did exist — the ⋯ menu's **Process again** —
reported nothing at all when it failed. Both are fixed:

- the Review screen's empty state has a **Process** button in it, with a
  progress bar and the stage the job is on while it runs;
- when a run fails, from anywhere, the reason is on screen — the engine's own
  sentence, not a shrug — and the retry is one tap away. A failed **Process
  again** from a Projects card leaves its reason on the card; tap the line to
  dismiss it.

If it keeps failing, send the logs.

### Sending logs when something goes wrong

**Profile › Send logs.** See §7 for what is in the bundle. Three things worth
knowing:

- the zip is written to **`Downloads/LidarScan`** before any send is
  attempted, so it exists even if the send fails;
- it includes crash entries, in the same file and in order with everything
  the app was doing at the time;
- do it **soon**. The log rotates; the bundle carries the live file and one
  rotation, and a session from several days of scanning ago may have aged out.

A useful report is: what you were doing, what you expected, what happened,
the scan's name, and the bundle.

### Other

- **The scan stopped when I checked Projects.** That is the 0.9.10 behaviour
  described in §4. It saved. Look for the green "Scan saved." note.
- **I lost the cloud in the viewer.** Double tap to re-frame the whole scan.
- **The Detail setting I want is missing.** Your phone's ceiling removed it.
  See §10. There is no override.
- **An option I remember is gone.** It is probably behind **Settings ›
  Lab features**, which is off by default. Nothing was deleted.
