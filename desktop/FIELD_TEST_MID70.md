# Field test — Livox Mid-70 + JuxiTech serial IMU (desktop 1.1.0)

This is the test the app has never had: the first run against real hardware.
Everything in the Mid-70 path was built from the SDK source and the earlier
ROS work, not from a live device. The app logs everything it needs on its
own; your job is to run the steps below in order and send the diagnostics
bundle back. **Do not skip a step because an earlier one failed — the log of
the failure is the result.**

## 0. Install

1. Open `LidarScan-1.1.0-universal.dmg`, drag LidarScan to Applications.
2. It is ad-hoc signed (no Apple Developer ID). First launch: right-click →
   Open → Open. If macOS says it is damaged, run once in Terminal:
   `xattr -dr com.apple.quarantine /Applications/LidarScan.app`.
3. Launch it once with nothing plugged in. Quit. This proves the app runs on
   this Mac and creates the log folder `~/Library/Logs/LidarScan/`.

## 1. Network

- Plug the Mid-70 (with its converter/power) into the Mac's Ethernet port
  (built-in or a Thunderbolt/USB adapter — no switch needed).
- System Settings → Network → that port → Configure IPv4 **Manually**:
  IP `192.168.20.2`, mask `255.255.255.0`, no router. (The lidar was last
  seen at `192.168.20.101`; if Auto-detect later reports a different source
  IP, put the Mac on that lidar's /24 instead.)
- When macOS asks whether LidarScan may accept incoming connections, click
  **Allow**. If you clicked Deny, System Settings → Network → Firewall →
  Options → allow LidarScan.

## 2. IMU

- Plug the JuxiTech module into USB. In Terminal `ls /dev/cu.*` should show a
  new `cu.wchusbserial…` or `cu.usbserial…`. If nothing appears, the CH340
  driver is missing (macOS 13+ has it built in; older needs the WCH driver).

## 3. In the app — Capture

Power the Mid-70 and wait until its fan/motor sound settles (a cold unit
self-heats for up to 3 minutes and does not stream until then).

1. **Auto-detect.** Expected: "Mid-70 <code> at 192.168.20.101" (the code is
   the 15-character label code) and "JuxiTech IMU on /dev/cu.…". The model
   combo flips to **Livox Mid-70**, the Broadcast code row fills, the IMU
   combo selects the port. If the Mid-70 is not found: check the cable
   light, the static IP, and that no other Livox software is open — then run
   Auto-detect again. Note in your message whether it was found.
2. **Connect.** Watch the status line. Expected sequence in the log pane:
   connected (code, ip, firmware) → state Normal → configuring → **sampling**
   → points/s climbing to ~100 000. "warming up (SELF-HEATING)" is normal on
   a cold unit; wait. The IMU row should show `rate ≈ 100 Hz` within a few
   seconds. If it shows 25 Hz, the rate command did not take — note it.
3. **Hold still 10 s**, then **Start** (record). Walk a slow loop of the
   room, 30–60 s, no fast turns, come back to where you started. **Stop.**
4. Leave it connected **at least 90 s** in total so the IMU's known ~35 s
   blackout shows in the counter (`blackouts: 2` or more is expected — it is
   the module's firmware, not a fault).
5. **Disconnect.** Then Projects → open the recording → **Replay**. Note
   whether the replayed map matches the live one.

## 4. Send the results

Help → **Save diagnostics bundle…**. A folder `LidarScan-diagnostics-<time>`
appears on the Desktop and opens in Finder. Zip it and send the zip. It
contains the full log, the raw Mid-70 broadcast capture, the raw IMU byte
capture, the recording (if under 1 GB), and the machine's USB/network
listing. Nothing else is needed.

If the app crashes: relaunch it, do nothing else, Help → Save diagnostics
bundle — the crash line is already in the log.

## What I read from the bundle (for reference)

| Question | Where it is in the log |
| --- | --- |
| Did the Mid-70 broadcast reach the Mac, and what does it send? | `discovery:` lines + `mid70_broadcast-*.livoxdump` |
| Did the SDK v1 handshake complete on real firmware? | `[mid70]` lines: connected → state → configure acks → sampling |
| Which data type / timestamp mode does the firmware use? | stats lines `data_type=`, `timestamp_type=` |
| Point rate and loss | stats `points_per_sec=`, `loss_pct_window=`, `packets_lost=` |
| Did the IMU rate command take? | stats `imu rate_hz=` (100 expected, 25 = command ignored) |
| Blackout count and length | stats `blackouts=`, `worst_blackout_s=` |
| Did LIO initialise and track? | `lio` stats lines, `Start`/`Stop` lines |
| Replay fidelity | `replay:` lines after the session |
| Device self-report | stats `pps_ok=`, `time_sync_status=`, `self_heating=`, temperature/voltage/motor flags |
