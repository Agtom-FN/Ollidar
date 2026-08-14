# LidarScan Remote Capture Kit — Instructions

Thank you for helping capture data for the LidarScan project! You don't need
to know anything about lidar or programming to do this. Just follow the
steps for your computer's operating system below.

There are up to three captures we may ask for, depending on what hardware
you have on hand:

1. **COIN-D6** (the small spinning 2D lidar) — `capture_d6.py`
2. **Livox Mid-360** (the box-shaped 3D lidar on an Ethernet cable) — `capture_mid360.py`
3. **GNSS/RTK rover** (the survey antenna/receiver) — `capture_gnss.py`

You only need to run the script(s) for the hardware you actually have.

---

## 0. One-time setup: install Python + pyserial

You only need to do this once.

### Windows

1. If you don't have Python yet, install it from https://python.org/downloads
   (check "Add python.exe to PATH" during install), or from the Microsoft
   Store. This kit is tested with the `py` launcher, which that installer
   provides.
2. Open **Command Prompt** (search "cmd" in the Start menu), `cd` into the
   folder where you unzipped this kit, and run:
   ```
   py -m pip install pyserial
   ```

### macOS

1. macOS usually already has Python 3. Open **Terminal** (search
   "Terminal" in Spotlight), `cd` into the folder where you unzipped this
   kit, and run:
   ```
   python3 -m pip install --user pyserial
   ```
   If that says `python3: command not found`, install Python from
   https://python.org/downloads first, then retry.

### Linux

1. Open a terminal, `cd` into the folder where you unzipped this kit, and
   run:
   ```
   sudo apt update && sudo apt install -y python3 python3-pip
   python3 -m pip install --user pyserial
   ```
   (adjust the package-manager line for non-Debian distros; Python 3 is
   preinstalled on most).

**Note:** `capture_mid360.py` needs no install at all (Windows, macOS, and
Linux all have everything it needs built in) — only `capture_d6.py` and
`capture_gnss.py` need pyserial.

---

## 1. COIN-D6 capture

### 1a. Find the port name

Plug the D6's CH340 USB-serial adapter into the computer, then run:

- **Windows:** `py capture_d6.py --list`
- **macOS:** `python3 capture_d6.py --list`
- **Linux:** `python3 capture_d6.py --list`

Look for a line marked `<-- looks like a CH340`. Note the port name shown
before the description — that's what you pass to `--port` next.

- Windows port names look like `COM3`, `COM5`, etc.
- macOS port names look like `/dev/cu.usbserial-1140`.
- Linux port names look like `/dev/ttyUSB0`.

If nothing shows up, see the Troubleshooting table below before continuing.

### 1b. Run the capture

Power on the D6 (it should be spinning/have an LED lit before you start),
then run one of:

- **Windows:** `py capture_d6.py --port COM5 --seconds 30 --out bench_d6_30s.bin`
- **macOS:** `python3 capture_d6.py --port /dev/cu.usbserial-1140 --seconds 30 --out bench_d6_30s.bin`
- **Linux:** `python3 capture_d6.py --port /dev/ttyUSB0 --seconds 30 --out bench_d6_30s.bin`

You'll see a live counter counting bytes up for 30 seconds, then a summary.

**What success looks like:** a file named `bench_d6_30s.bin` about
**300-400 KB** in size, and the last line says
`Sanity verdict: PASS`.

---

## 2. Livox Mid-360 capture

The Mid-360 connects over Ethernet, not USB-serial — no driver install
needed for this script.

**Important:** the lidar needs to already be configured to send its data to
this computer's IP address. If you have the vendor's **Livox Viewer 2** app
and it shows the point cloud live, the lidar is already streaming to this
machine — that's exactly the condition this script needs. **Quit Livox
Viewer 2 before running this script** (otherwise Viewer 2 will be holding
the network ports open and this script won't be able to bind them).

Run:

- **Windows:** `py capture_mid360.py --seconds 60 --out bench_mid360_60s.livoxdump`
- **macOS:** `python3 capture_mid360.py --seconds 60 --out bench_mid360_60s.livoxdump`
- **Linux:** `python3 capture_mid360.py --seconds 60 --out bench_mid360_60s.livoxdump`

You'll see live per-port packet counters for 60 seconds, then a summary.

**What success looks like:** a file named `bench_mid360_60s.livoxdump`
several **MB to tens of MB** in size (the Mid-360 streams a lot of data —
~20+ Mbps), with packets showing up on more than one port, and
`Sanity verdict: PASS`.

If you get an error about a port already being in use, make sure Livox
Viewer 2 (or any other Livox tool) is fully closed and try again.

---

## 3. GNSS/RTK rover capture

Pair/connect the rover first:

- **Bluetooth rover:** pair it in your OS's Bluetooth settings like any
  other Bluetooth device, using the name/PIN in the rover's manual.
- **USB-cable rover:** just plug it in.

Then find its port the same way as the D6 (the rover won't say "CH340" —
look for a new port that appeared after pairing/plugging in):

- **Windows:** `py capture_gnss.py --list`
- **macOS:** `python3 capture_gnss.py --list`
- **Linux:** `python3 capture_gnss.py --list`

Run the capture (adjust `--seconds` if asked for a different duration):

- **Windows:** `py capture_gnss.py --port COM6 --seconds 120 --out bench_gnss_120s.nmea`
- **macOS:** `python3 capture_gnss.py --port /dev/cu.usbserial-1141 --seconds 120 --out bench_gnss_120s.nmea`
- **Linux:** `python3 capture_gnss.py --port /dev/ttyUSB1 --seconds 120 --out bench_gnss_120s.nmea`

If nothing is captured, try adding `--baud 115200` or `--baud 38400` to the
command above — different receivers use different rates.

**What success looks like:** a file named `bench_gnss_120s.nmea` with
hundreds of lines of text starting with things like `$GPGGA` or `$GNRMC`,
and `Sanity verdict: PASS`.

---

## 4. Sending the files back

Use whatever file-transfer method is easiest for you — email (if the files
are small enough), a shared drive, Slack/Teams/WhatsApp, WeTransfer, a USB
stick, etc. Any method is fine.

Please keep the default output filenames if possible, since we use them to
tell the files apart automatically:

| File | Expected size (for the defaults above) |
| --- | --- |
| `bench_d6_30s.bin` | ~300-400 KB |
| `bench_mid360_60s.livoxdump` | a few MB to tens of MB |
| `bench_gnss_120s.nmea` | tens to low hundreds of KB |

If a script's summary said `WARN` or `FAIL`, send the file anyway and let us
know what the message said — it's still useful for diagnosis.

---

## 5. Troubleshooting

### COIN-D6

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `--list` shows no ports at all | Adapter not plugged in, or driver missing | Re-seat the USB cable; on Windows install the CH340 driver (see below) |
| Windows: port doesn't show "CH340" in Device Manager | CH340 driver not installed | Run the installer inside `CH340_WINDOWS.zip` (in the vendor kit under `english coind6\4 Windows Host Software\CH340 Driver\`) as Administrator, then reboot and replug |
| "Could not open port... Access is denied / busy" | Another program has the port open | Close any other serial-monitor/vendor tool, unplug/replug, try again |
| Linux: "Permission denied" opening `/dev/ttyUSB0` | Your user isn't in the `dialout` group | Run `sudo usermod -aG dialout $USER`, then **log out and back in**, or just prefix the command with `sudo` for a one-off run |
| Capture runs but stays at 0 bytes / WARNING about no data | Lidar not powered, or wiring swapped | Check the D6 is spinning/lit; check the 4-pin connector is fully seated; if you built a custom cable, verify TX/RX aren't swapped |

### Livox Mid-360

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| "could not bind ... address already in use" | Livox Viewer 2 (or another tool) already has the port open | Fully quit Livox Viewer 2, then re-run the script |
| All per-port counters stay at 0 | Lidar not yet configured/streaming to this machine's IP | Open Livox Viewer 2 first, confirm the point cloud shows up live, then quit it and re-run this script (it keeps streaming to the same host) |
| No Ethernet link at all | Cable/adapter issue, or lidar not powered (9-27V, needs external battery) | Check the Ethernet cable is seated at both ends; check the lidar's power LED |
| Only 1 of 5 ports shows packets | Normal in some configs — not every port always carries data | Note it in the message when you send the file back; not necessarily a problem |
| Script exits immediately with a Python error | Wrong Python version (very old Python) | Confirm `python3 --version` / `py --version` reports 3.7 or newer |

### GNSS/RTK rover

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `--list` doesn't show the rover | Not paired (Bluetooth) or not plugged in (USB) | Pair via OS Bluetooth settings first, or check the USB cable |
| Capture runs but 0 bytes captured | Wrong baud rate | Re-run with `--baud 115200` or `--baud 38400` added to the command |
| Bytes captured but "no line breaks seen" warning | Receiver is outputting raw UBX/binary, not NMEA text | Check the receiver's output-format setting is NMEA (u-center or the receiver's app can usually toggle this) |
| Rover shows no fix / "no fix" in every line | No sky view / antenna not connected | Move to an open-sky area; check the antenna cable is seated |
| Bluetooth keeps disconnecting mid-capture | Out of range, or interference | Keep the computer within a few meters of the rover during the capture |

---

If anything doesn't match these instructions or an error message isn't
covered above, just send us a screenshot of the terminal output along with
whatever file did get produced — that's usually enough to diagnose from our
end.
