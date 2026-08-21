# Ollidar — Quick Start

App version 0.9.11. This page gets you one COIN-D6 scan on disk. For
everything else, see [USER_MANUAL.md](USER_MANUAL.md).

(The app is called Ollidar. The repository, the Android package and the
`Downloads/LidarScan` export folder still say `lidarscan` — that is
deliberate, not a leftover.)

**Before you start**

- An Android phone with USB-C OTG and ARCore ("Google Play Services for AR").
- A charged COIN-D6 on its bracket, and a USB-C cable.
- Somewhere with furniture and edges. The camera needs texture to track.

---

1. **Mount the D6 on the back of the phone.** Flat against the phone, the
   zero mark up and the cap pointing forward, so the scan fan is vertical.
   The screen says the same thing in five words: *"Mount flat. Keep the fan
   vertical."*

2. **Plug the D6 into the phone's USB-C port** and allow the USB permission
   prompt if Android asks for one.

3. **Open Ollidar and tap the Scan tab** — the radar icon, second from the
   left in the floating bar at the bottom. The selected tab's icon is orange.

4. **Wait for the scanner to be found.** The connection panel finds it by
   itself. If it says *"No scanner found. Plug it in, then Retry."*, unplug,
   plug back in and tap **Retry**.

5. **Stand where the scan should start and press the big orange SCAN
   button.** If nothing happens, the app writes the reason on screen in one
   short sentence — usually *"Connect the scanner first."*

6. **Hold still.** A panel counts through four stages:
   *New tracking session* → *Locking position tracking* → *Measuring the
   mount — hold still* → *GO — start walking*. Keep the phone pointed at
   furniture an arm's length away and do not move until the panel says
   **GO — START WALKING**. This usually takes 4–8 seconds.

7. **Walk slowly and smoothly.** Normal walking pace or slower. Turn on the
   spot rather than swinging the phone around a corner. Watch the points
   appear in the live view, which fills the whole screen while you scan —
   the tab bar hides itself and comes back when you stop.

8. **If an amber card fills the screen — "Tracking lost. Stop. Hold
   still." — stop walking immediately** and stand still until it turns green
   and says *"OK — keep walking."* Walking while it is amber puts a hole in
   the scan that usually cannot be repaired afterwards.

9. **Press STOP** (the same big button; it says STOP while recording). The
   scan is saved, given a grade, and processed automatically. Wait for the
   card to finish, then tap **Done**.

10. **The app lands you on Projects.** Tap the scan to open it in the viewer.
    One finger rotates, two fingers pan, pinch zooms, double tap re-frames
    the whole scan. **Export** saves to your Downloads folder; **Share**
    opens the Android share sheet.

---

**Two things that will surprise you if nobody says them**

- **Leaving the Scan tab ends the scan.** The tab bar is hidden while you
  are recording, but if you do get to Projects, Jobs or Settings — with the
  system Back gesture, say — the scan is stopped and saved and the camera is
  shut down. You get a green *"Scan saved."* note on Projects. Coming back to
  the Scan tab always starts a fresh scan — it never resumes the old one.
- **The phone can be held either way, and the way you start is the way it
  stays.** Portrait and landscape both work. At the start of a scan the app
  works out which way you are holding the phone from gravity and then locks
  it for the rest of the scan: turning the phone mid-walk is scanning motion,
  not a change of layout.
- **The mount is measured at every start.** The D6 comes off the phone
  between sessions, so its exact angle changes each time. That is what the
  hold-still stage is for. Skipping it is not offered because the angle ends
  up in every point.

New to the app? Tap the **?** at the top of the Scan screen for a six-step
tour of the controls. You can replay it any time from
**Settings › About › Tutorial**.
