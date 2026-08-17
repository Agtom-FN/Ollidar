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
