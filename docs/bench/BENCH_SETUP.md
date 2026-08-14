# LidarScan — Bench / Rig Setup Guide

Spike **S4**. Companion to `PROCUREMENT.md`. Covers wiring, driver install per OS,
static-IP setup for the Mid-360, and safety notes. Written against the datasheet
translation in `english coind6/1 D6 LiDAR Manuals and Documentation/COIN-D6
Single-Line LiDAR Datasheet A1 (English translation).md` and the vendor's
`sc_mini.rules` in `english coind6/2 ROS1 ROS2 Driver Package
Documentation/cspc_lidar_sdk_ros2_D4_20250731/cspc_lidar_sdk_ros2/`.

No photos yet — none were found in the vendor package folders on disk (checked
`english coind6/` for `.jpg/.png/.jpeg`, none present). **TODO once hardware is on
the bench: photograph the assembled rig from the front, side, and the D6 connector
close-up, and drop them in this file** (tech spec S4 exit criteria calls for
"setup doc with photos").

---

## 1. COIN-D6 → CH340 → host wiring

### 1.1 Connector and pinout

Per the datasheet (§"Interface and communication"):

> COIN-D6 connects to the external system through a standard **4-pin 1.5 mm
> connector**, which provides power, rotation control and data reception.

| Pin # (order on connector) | Name | Function |
| --- | --- | --- |
| 1 | **CVC** | Power supply, 5 V DC (see §1.2 below) |
| 2 | **GND** | Ground / return |
| 3 | **DRX** | Data receive at the *lidar* — i.e., host TX connects here |
| 4 | **DTX** | Data transmit at the *lidar* — i.e., host RX connects here |

**TX/RX crossover:** this is a standard UART crossover, not a straight-through
wire. The lidar's **DTX** (its transmit pin) must go to the CH340 adapter's
**RXD**, and the lidar's **DRX** (its receive pin) must go to the CH340 adapter's
**TXD**. If the vendor's own 4-pin-to-USB pigtail cable is used as shipped, this
crossover is already done inside the cable — no action needed. Only worry about
this wiring if building a custom cable or a breakout for the CH340 board directly.

⚠ VERIFY the vendor kit's pigtail cable is present and intact before assuming this
wiring is pre-made — see `PROCUREMENT.md` (a).

Signal levels are **LVTTL 3.3 V**, not RS-232 — do not wire this into an RS-232
(±12 V) port or level-shifter meant for RS-232; use the CH340 USB-serial adapter,
which is a native 3.3/5 V TTL-to-USB bridge.

### 1.2 Power

| Item | Value |
| --- | --- |
| Supply voltage | 4.5–5.5 V DC, 5 V typical, **<100 mV ripple recommended** |
| Peak current | ~800 mA |
| Steady-state system current | 200–500 mA @ 5 V |

The 5 V line is supplied over the same 4-pin connector (CVC/GND) — there is no
separate power jack. If the CH340 adapter cable brings its own 5 V from USB VBUS
(typical for these vendor pigtails), a host USB port or the powered hub covers
this automatically; no separate bench PSU is required. If instead the vendor
cable exposes CVC/GND as bare leads expecting an external 5 V supply, use a clean,
low-ripple 5 V source (a phone charger or USB power bank through a USB-A-to-barrel
or USB-A-to-bare-wire adapter works) — **do not use an unregulated wall-wart**,
ripple above 100 mV is out of spec and can cause erratic ranging.

⚠ VERIFY which of the two cases above matches the actual vendor cable once it's in
hand (visually trace the CVC/GND leads from the 4-pin connector to the CH340
board/USB plug).

### 1.3 Serial parameters

| Parameter | Value |
| --- | --- |
| Baud rate | 230400 |
| Format | 8 data bits, 1 stop bit, no parity (8N1) |
| Voltage | LVTTL 3.3 V (2.4–3.6 V high, ≤0.4 V low output; 2.0–3.6 V high, ≤0.8 V low input) |

### 1.4 Start/stop and expected data (for the smoke test in `TEST_CHECKLIST.md`)

- Start command: `AA 55 F0 0F` → lidar begins scanning
- Stop command: `AA 55 F5 0A` → lidar stops
- ACK frames come back per the vendor's Data Format Standard Specification
  (`COIN-D6 LiDAR Data Format Standard Specification V1.0 (English translation).md`,
  §2.1/2.2) — a device-info upload frame (`A5 5A 14 00 E3 02 01 ...`, ASCII
  `"COIN-D6"` in the data area) is the *first* thing the lidar sends after
  power-up, before any start command — useful as a "the wiring works at all"
  smoke check even before parsing scan data.
- After power-up the lidar starts in a **stopped** state; nothing streams until
  the start command is sent.
- While spin-up is stabilizing, `0xFE`/`0xFF` filler bytes appear in the stream —
  expected, not an error; the D6 driver (workstream A2) is responsible for
  filtering these.

### 1.5 CH340 driver install per OS

**macOS (this Mac, and any Mac 13+):** Apple ships an in-box CH34x USB-serial
driver — per the tech spec (§3.1) this is expected to "just work" with no
install step. Plug in, then check `ls /dev/tty.usb*` (or `/dev/cu.usb*`) — a
device node should appear within a couple of seconds. ⚠ VERIFY the exact macOS
version cutoff for in-box support before relying on it for older test Macs (the
tech spec states "13+"; this Mac is on macOS 26.5.1, well above that line, so it
should be a non-issue here).

**Windows:** driver is already on disk — `english coind6/4 Windows Host
Software/CH340 Driver/CH340_WINDOWS.zip`. Unzip and run the installer (`SETUP.EXE`
or similar inside the archive) as Administrator, then reboot if prompted. After
install, plug in the adapter and confirm Device Manager → Ports (COM & LPT) shows
**USB-SERIAL CH340 (COMx)** — this is the exact string called out in the vendor's
own Host Software Operation Guide (`4 Windows Host Software/Host Software
Operation Guide V1.1 (English translation).md`, Step 2) as the install-success
signal. Note the COM port number for the app's serial-port picker.

**Linux (Ubuntu 22.04+):** the CH340 driver (`ch341`) is in-kernel on modern
Ubuntu — no separate driver install should be needed (the vendor's
`CH341SER_LINUX.zip` in the same folder is a fallback for kernels missing the
module; unlikely to be needed on 22.04+). What *is* needed is a **udev rule** so
the device is accessible without root and shows up at a stable path. The vendor
ships a precedent rule (`sc_mini.rules`, from `english coind6/2 ROS1 ROS2 Driver
Package Documentation/cspc_lidar_sdk_ros2_D4_20250731/cspc_lidar_sdk_ros2/`):

```
KERNEL=="ttyUSB*", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE:="0666", SYMLINK+="sc_mini"
KERNEL=="ttyUSB*", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", MODE:="0777", SYMLINK+="sc_m_c"
```

The first line is the CH340 rule (`1a86:7523` is the standard CH340 USB
vendor:product ID) — this is the one that applies to the D6 kit. The second line
targets a CP2102 chip (`10c4:ea60`) used on some of the vendor's other products;
harmless to leave in but not relevant to the D6. Install:

```bash
sudo cp sc_mini.rules /etc/udev/rules.d/99-sc-mini.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

After plugging in, `/dev/sc_mini` should appear as a symlink to the actual
`/dev/ttyUSB*` node, world read/write. Confirm the vendor ID with `lsusb` before
relying on the rule (`Bus 00X Device 00Y: ID 1a86:7523 ... CH340 serial
converter` is the expected line).

**Android:** no OS-level driver install — the app will use
`usb-serial-for-android` (per tech spec §3.1) to talk to the CH340 chip directly
via the Android USB host API, gated by the standard USB-permission-intent flow.
Nothing to do on the bench beyond having a USB-C OTG cable/adapter to the phone
(most Pixel phones need a USB-C-to-USB-A adapter or hub since the CH340 pigtail
is typically USB-A).

---

## 2. Livox Mid-360 — power + Ethernet, per host OS

### 2.1 Power

9–27 V DC, ~6.5 W. Connect the battery pack (see `PROCUREMENT.md` (c)) to the
Mid-360's power leg **only after confirming the actual connector on the included
cable** — see the ⚠ VERIFY note in `PROCUREMENT.md` (c). Do not improvise a
connection to bare wires without first confirming polarity (see Safety, §4).

### 2.2 Ethernet + static IP, per host

Per tech spec §2.2: lidar sits at `192.168.1.1xx`, host must be static on
`192.168.1.x` (same /24, different host address, avoiding the lidar's own
address). ⚠ VERIFY the lidar's exact factory-default IP (commonly `192.168.1.1xx`
per unit serial/config — confirm from the Livox SDK2 docs or the unit's label
once it's in hand) before picking the host's static address.

**macOS:** System Settings → Network → (select the Ethernet interface, e.g. the
Mac mini's built-in port, or the USB-C adapter) → Details → TCP/IP → Configure
IPv4: **Manually** → IP Address `192.168.1.50` (any free address in the subnet,
avoiding the lidar's address) → Subnet Mask `255.255.255.0` → Router: leave blank
(no gateway needed for a direct point-to-point link). Apply.

**Windows:** Settings → Network & Internet → Ethernet → (select adapter) → IP
assignment → Edit → Manual → IPv4 On → IP address `192.168.1.50` → Subnet mask
`255.255.255.0` → Gateway: blank → Save. (Older-UI path: Control Panel → Network
and Sharing Center → Change adapter settings → right-click adapter → Properties →
Internet Protocol Version 4 → Use the following IP address.)

**Linux (Ubuntu, NetworkManager):**

```bash
nmcli connection add type ethernet ifname <iface> con-name mid360-static \
  ipv4.addresses 192.168.1.50/24 ipv4.method manual
nmcli connection up mid360-static
```

Or via GUI: Settings → Network → (gear icon on the wired connection) → IPv4 →
Manual → Address `192.168.1.50`, Netmask `255.255.255.0`, leave Gateway blank →
Apply.

**Android (USB-C Ethernet adapter):** plug the AX88179 or RTL8153 adapter into
the phone; Android typically auto-detects it as an Ethernet interface. Static IP
config path varies by OEM/Android version — generally Settings → Network &
Internet → Ethernet (only appears once an adapter is attached) → tap the
interface → switch from DHCP to Static → enter IP `192.168.1.50`, netmask/prefix
`24` (or `255.255.255.0`), gateway blank/`0.0.0.0`, DNS optional. This is exactly
the OEM-variance risk flagged in the tech spec (§Risks: "OEM USB-Ethernet
variance ... Opus-assigned B3, self-test wizard") — if the Ethernet option
doesn't appear at all on a given phone/adapter combo, that itself is a data point
worth recording for B3, not just a bench annoyance. Test with both adapter
chipsets bought in `PROCUREMENT.md` (b).

### 2.3 Verifying the link (before any SDK code exists)

Once wired and statically addressed, confirm basic reachability before assuming
the Livox SDK2 side will work: `ping 192.168.1.1XX` (the lidar's address) from
the host. A reply confirms cabling + IP config are correct independent of any
LidarScan code. If ping fails: check cable (a direct host↔lidar link needs no
switch, but if a switch/hub is in the path, confirm it's passing traffic),
confirm both ends are actually on the same /24, and confirm the lidar is powered
(most Livox units show a status LED — ⚠ VERIFY LED behavior/meaning once the unit
is in hand, not documented locally).

---

## 3. Rig assembly

1. Mount the **COIN-D6 vertically** on its plate/bracket — the datasheet's
   coordinate-system note ("left-hand coordinate system... rotation angle
   increases clockwise... zero-degree direction marked in the figure," see
   datasheet §"Scan data coordinate system definition") means the zero-angle
   reference mark on the unit should be noted/photographed at mount time so the
   pushbroom assembler (workstream A8) has a known reference orientation.
   Optical window (laser exit) sits 26.9 mm above the sensor's base plane per
   the datasheet — keep the window unobstructed by the bracket itself.
2. Mount the **Mid-360 tilted ~15°** from horizontal per tech spec §2.2/§2.4
   ("tilted mount recommended for floor + ceiling").
3. Co-mount the phone in a camera cage so its rear camera has an unobstructed
   view (tech spec §2.4: "camera view unobstructed") — needed for both ARCore
   pose tracking and camera colorization capture.
4. Keep the phone-to-lidar geometry **rigid** — any flex between phone and
   lidar directly corrupts the mount-calibration solve (S6) and the pushbroom
   assembler's extrinsics assumption (§3.3).
5. Route cabling (CH340 pigtail, Mid-360 Ethernet + power) so nothing snags or
   flexes the 4-pin D6 connector, which is a small, non-locking 1.5 mm pitch
   connector — strain-relieve it (a small zip-tie loop or tape loop near the
   connector, not pulling on the connector itself) rather than letting cable
   weight hang off the pins.
6. Tripod-mount the whole rig for static bench captures; hand-held / rig-on-a-
   pole is a later field-test concern (workstream E3), not needed for Phase 0
   bench work.

---

## 4. Safety notes

- **12 V battery polarity (Mid-360):** confirm + and − before connecting.
  Reversed polarity into the Mid-360's power input risks damaging a $1,300+
  sensor — this is the single highest-consequence wiring mistake on this bench.
  If the included Livox cable's power leg is bare wire (⚠ VERIFY, see
  `PROCUREMENT.md` (c)), use a multimeter to confirm polarity against the
  battery pack's labeled output *before* the first connection, and consider
  marking the confirmed-correct leads with tape/heat-shrink so it's not
  re-derived from memory on every bench session.
- **COIN-D6 5 V line:** less catastrophic than the Mid-360's 12–27 V range, but
  still reverse the same check — confirm CVC/GND polarity before powering from
  anything other than the vendor's own pigtail cable.
- **Laser safety (COIN-D6):** per the datasheet, COIN-D6 uses a 905 nm pulsed
  laser and **meets Class 1 (IEC 60825)**, which by definition is safe under
  all normal operating conditions including direct viewing — no special laser
  PPE or precautions are required for bench work. The datasheet also notes the
  unit has internal protection that shuts down laser emission on an over-power
  or abnormal-rotation-speed fault, so a malfunctioning unit fails safe rather
  than exceeding Class 1 output.
- ⚠ VERIFY Mid-360 laser classification directly from Livox's own documentation
  before bench work with it — no Livox datasheet was found locally in
  `mid-360/` (the folder exists but is empty) to confirm its class.
- General bench hygiene: don't hot-swap the D6's 4-pin connector while powered
  if avoidable (small connector, easy to short pins momentarily while mating);
  keep the Mid-360's Ethernet/power cabling away from pinch points if the rig
  is handled/rotated during setup.
