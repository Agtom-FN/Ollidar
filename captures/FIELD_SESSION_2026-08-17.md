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
