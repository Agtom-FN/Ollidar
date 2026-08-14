# LidarScan — Phase 0/1 Bench Procurement List

Spike **S4** (see `LidarScan Tech Spec.md` §2 Hardware & sensor facts, §2.4 Phase 1
test kit, §4 execution plan). Source docs consulted: `english coind6/README.md`,
`english coind6/1 D6 LiDAR Manuals and Documentation/COIN-D6 Single-Line LiDAR
Datasheet A1 (English translation).md`, `.../COIN-D6 LiDAR Data Format Standard
Specification V1.0 (English translation).md`, `english coind6/4 Windows Host
Software/Host Software Operation Guide V1.1 (English translation).md`.

**All prices are rough USD street prices as of writing and marked for
re-verification — do not treat any number here as a quote.** Re-check
availability, price, and (for the Mid-360) connector/cable details at order time.

---

## (a) Already owned

| Item | Status | Notes |
| --- | --- | --- |
| COIN-D6 lidar unit | Owned | 2D lidar, 360°, 4,000 pts/s, 10 Hz, UART 230400 8N1 LVTTL 3.3 V |
| COIN-D6 vendor kit contents | ⚠ VERIFY | The vendor package folder (`english coind6/`) contains manuals, ROS driver packages, STM32 example code, Windows host software + **CH340 Windows driver zip** and a **CH341SER Linux driver zip**, and 3D model files — but there is **no packing list on disk** confirming what shipped in the physical box. Before assuming a CH340 USB-serial adapter cable is physically in hand, open the box and confirm: (1) a 4-pin-to-USB adapter cable is present, (2) it is in fact CH340-based (check for a `USB-SERIAL CH340` enumeration, or inspect the board for a CH340/CH340G chip marking — see BENCH_SETUP.md §1), (3) the 4-pin 1.5 mm connector on the lidar's own pigtail is intact and matches the CVC/GND/DRX/DTX pin order in the datasheet. |
| This Mac (Apple Silicon) | Owned | Mac mini, Apple M4, macOS 26.5.1, 16 GB RAM. Has built-in Gigabit Ethernet (usable directly for Mid-360, no USB-C Ethernet adapter needed on this host) and USB-C/Thunderbolt ports. **No built-in USB-A** on most M4 Mac mini configs — check the unit; if absent, the CH340 adapter (likely USB-A) will need the USB-C hub (item below) to attach. Covers the "Apple-silicon Mac" half of §2.4's Mac requirement; there is **no Intel Mac on hand** — the spec allows "CI-only x86_64 build verification" as a substitute, so no purchase is strictly required here. |

---

## (b) To buy now — S1/S3/S7 (Android + desktop bring-up, Windows toolchain)

| Item | Example models | Rough price | Notes |
| --- | --- | --- | --- |
| Android test phone | **Pixel 7 / Pixel 7 Pro / Pixel 8** (used/refurb acceptable) | $180–350 used, $500+ new | Needs USB-C OTG, Vulkan support, ARCore certification — all three are true of the whole Pixel 7/8 line. Galaxy S22+ class is the spec's named Samsung alternative. ⚠ VERIFY ARCore device-support list at order time (google.com/ar) hasn't dropped the specific model. |
| USB-C to Gigabit Ethernet adapter (AX88179 chipset) | Cable Matters USB-C to Gigabit Ethernet Adapter; Sabrent USB 3.0/USB-C to Gigabit Ethernet | $15–25 | AX88179 has the broadest OS driver support (macOS in-box, Linux in-kernel, Windows in-box on 10/11) — buy this one first for the "known-good" host-side adapter. |
| USB-C to Gigabit Ethernet adapter (RTL8153 chipset) | UGREEN USB-C to Ethernet Adapter; Anker USB-C to Gigabit Ethernet Adapter | $12–20 | Second, different-chipset adapter — deliberately buy one of each so Android OEM Ethernet-driver variance (tech spec §Risks: "OEM USB-Ethernet variance") can be tested against two real chipsets, not assumed from one. |
| Powered USB-C hub | Anker 555 (8-in-1) or Anker 565; CalDigit USB-C dock | $40–100 | Needed to run CH340 serial + Ethernet adapter + power simultaneously off one USB-C port (phone or Mac). Must be **powered** (self-powered, not bus-powered) — the D6 + Mid-360 + adapters can exceed a phone's OTG current budget. |
| Windows x64 test PC | Beelink SER5/SER8 or GMKtec mini-PC (Ryzen/Intel, 16 GB+ RAM) | $250–450 | "Cheap mini-PC ok" per spec. Needs a free USB-A or USB-C port for CH340, and Ethernet (built-in NIC preferred over adapter) for Mid-360. |
| Linux x86_64 test box | Same mini-PC dual-booted with Ubuntu 22.04+, **or** a second cheap mini-PC dedicated to Linux | $0 (dual-boot the Windows box) or $200–350 (dedicated box) | Dual-purpose (one machine, dual-boot) is the cheaper path and satisfies §2.4; a dedicated box avoids reboot friction during S7 CI-toolchain iteration — owner's call on budget vs. convenience. |

**Subtotal (b), dual-boot path:** roughly $460–800 (excluding the phone, which may already exist in the household — confirm before buying).
**Subtotal (b), dedicated-Linux-box path:** roughly $660–1,150.

---

## (c) To buy for S2 — Livox Mid-360

| Item | Example / spec | Rough price | Notes |
| --- | --- | --- | --- |
| Livox Mid-360 lidar | Livox Mid-360 (DJI/Livox) | ⚠ VERIFY — street price commonly cited in the **$1,300–1,600** range | Price and regional availability (Livox retail has shifted distributors) must be re-verified directly with Livox/DJI or an authorized reseller before ordering. 360°×59° FOV, 200,000 pts/s, built-in 6-axis IMU @200 Hz, 100 Mbps Ethernet/UDP (Livox SDK2), 9–27 V DC ~6.5 W power. |
| Power + connector cabling | ⚠ VERIFY — the Mid-360 uses a single circular **M12 aviation-style connector** on the unit for combined power + Ethernet, broken out on the **cable Livox includes in the box** to a standard RJ45 (Ethernet) leg and a power leg. | Included with unit; spare/replacement cables ~$30–60 if sold separately | **What is uncertain and must be confirmed on receipt of the unit / from Livox documentation, not assumed**: (1) whether the power leg of the included cable terminates in bare wires, a DC barrel plug, or another connector; (2) exact barrel size if it is a barrel connector (commonly 5.5×2.1 mm for 12 V gear, but not confirmed for this cable); (3) whether the Ethernet leg is a captive RJ45 or another M12-to-RJ45 dongle. Do not wire anything to a battery until the connector on the actual included cable has been visually confirmed — reversed polarity into a bare-wire lead is a real risk (see BENCH_SETUP.md safety notes). |
| 12 V battery pack | TalentCell 12V 3000mAh/6000mAh Li-ion or LiFePO4 pack (has a 5.5×2.1 mm DC barrel output + on/off switch) | $30–55 | Sized for ~6.5 W continuous draw (~0.55 A @ 12 V) — even the smaller 3000 mAh pack gives several hours of bench runtime. Buy **after** confirming the Mid-360 cable's power-side connector (previous row); if it's bare-wire, also buy a matching barrel-jack-to-screw-terminal or barrel-jack-to-bare-wire pigtail (~$5–10) to mate cleanly instead of twisting wires. |

**Subtotal (c):** roughly $1,330–1,660 (dominated entirely by the Mid-360 unit itself; re-verify before committing spend).

---

## (d) To buy for S5 — RTK

| Item | Example | Rough price | Notes |
| --- | --- | --- | --- |
| **Budget-plus path:** u-blox ZED-F9P eval board + antenna | Ardusimple simpleRTK2B (ZED-F9P) or SparkFun GPS-RTK2 Board; paired with a survey-grade GNSS antenna (e.g., Tallysman TW3872 or the antenna bundled by Ardusimple/SparkFun kits) | Board $200–280, antenna $60–150 (bundled kits often ~$300–400 total) | DIY path: outputs NMEA over USB-serial or UART; needs a Bluetooth serial module added if BT SPP to Android is required (or use USB-serial fallback on desktops per spec §2.3). More setup work, cheapest true RTK-capable path. |
| **Turnkey path:** Emlid Reach RX | Emlid Reach RX (rover, NTRIP client built in, Bluetooth to phone/tablet) | ~$185–220 | Turnkey: has Bluetooth SPP + built-in NTRIP client out of the box, closest match to the spec's "NMEA 0183 over Bluetooth (SPP on Android)" requirement with least integration work. No base-station capability (rover-only) — fine for Phase 1 since NTRIP supplies corrections. |
| NTRIP correction service | Regional CORS network (state DOT / geodetic survey CORS, often free or low-cost) **or** a commercial caster (e.g., Point One Nav, Skylark, u-blox PointPerfect) **or** run your own base station later if a Reach RS2+/base unit is added | $0–50/mo typical for regional CORS; commercial casters vary | ⚠ VERIFY regional CORS mountpoint coverage and account signup process for the bench's actual location before S5 — availability and cost vary a lot by country/state. This is the item most likely to have a lead time (account approval), so start it early even though it's "free." |

**Recommendation:** start with the Reach RX turnkey path for S5's exit criteria ("RTK Fixed on bench," NMEA+RTCM logs) since it minimizes bring-up risk; the F9P DIY path can follow later if a cheaper high-volume rover BOM is wanted for the eventual product bundle.

**Subtotal (d):** roughly $185–400 depending on path, plus $0–50/mo for NTRIP.

---

## (e) Rig — mounts, tripod, calibration target

| Item | Example / spec | Rough price | Notes |
| --- | --- | --- | --- |
| Rigid mount bracket | 3D-printed bracket (custom, PETG/ABS) **or** aluminum L-bracket/plate (80/20 extrusion or a laser-cut plate from SendCutSend/OSH Cut) + a camera cage (e.g., SmallRig universal cage) to co-mount phone + lidar | 3D print: $0 (own printer) or $20–40 (print-on-demand service); aluminum plate + cage: $60–120 | Must hold the **phone** and whichever lidar is mounted **rigidly relative to each other** (extrinsics assumed fixed for a session — tech spec §3.5/§3.3). Two lidar orientations needed per spec: **COIN-D6 mounted vertically** (pushbroom scan plane sweeps vertically as the rig translates/rotates) and **Mid-360 mounted tilted ~15°** (per §2.2 "tilted mount recommended for floor + ceiling"). If one bracket is meant to serve both sensors, design a swappable lidar plate rather than a single fixed geometry — the two sensors have different mount angles by design, not by mistake. Camera lens must stay unobstructed (spec §2.4). |
| Tripod | Any photo/video tripod with a standard 1/4"-20 or 3/8" stud, load-rated well above the rig's weight (lidar + phone + cabling ~1–2 lb) | $40–100 | Used for static bench captures (D6 fixed-position profile mode per spec §3.3, Mid-360 static scans) and as a stable calibration rig for S6. |
| Calibration checkerboard | Print a **9×6 internal-corner checkerboard**, **30 mm square size**, on rigid backing (foam-core or acrylic-mounted print, laminated), flat to <1 mm — a common OpenCV-compatible calibration target size; adjust square size to fill roughly half the camera's working frame at the lidar's typical bench range (~1–3 m) | $10–25 (print + mount at a local print shop) | ⚠ VERIFY exact rows/cols/square-size against whatever calibration tool S6 ends up using (OpenCV `findChessboardCorners` expects **internal corner count**, i.e. a "9×6" board has 10×7 physical squares) — confirm the convention before ordering the print so the board matches the tool, not the other way around. |

**Subtotal (e):** roughly $110–265.

---

## Budget summary

| Group | Low estimate | High estimate |
| --- | --- | --- |
| (a) Already owned | $0 | $0 |
| (b) S1/S3/S7 (dual-boot Linux) | $460 | $800 |
| (c) S2 Mid-360 | $1,330 | $1,660 |
| (d) S5 RTK | $185 | $400 (+ $0–50/mo NTRIP) |
| (e) Rig | $110 | $265 |
| **Total to-buy** | **~$2,085** | **~$3,125** |

The Mid-360 unit dominates the budget by a wide margin. Everything in groups (b),
(d), and (e) is low-risk commodity gear; the Mid-360 (price, availability, and
connector confirmation) and the RTK NTRIP account (lead time) are the two items
worth starting first even though they're not the cheapest.

**Every price and model above is a rough estimate for planning purposes only —
re-verify current price and availability directly with the vendor/reseller at
order time, particularly for the Livox Mid-360.**
