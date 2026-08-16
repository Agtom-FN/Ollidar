#!/bin/bash
# LidarScan FIELD-TEST KIT v2 - shared shell library (macOS, Apple Silicon)
#
# Sourced by the TEST_*.command launchers. Everything the tests need is a
# bundled, pre-compiled arm64 binary in ./bin -- there is nothing to install
# and no Python involved:
#
#   bin/d6cli      the S1 spike's real COIN-D6 parser (cmake-built here),
#                  so the macOS D6 verdict is a genuine checksum pass rate,
#                  not a byte-rate guess like the Windows one
#   bin/mid360cap  UDP capture to .livoxdump (same container as
#                  tools/remote-capture/capture_mid360.py)
#   bin/um982cap   serial NMEA capture with live fix/satellite display
#
# Results all land in ONE folder, ~/Desktop/LIDAR_TEST_RESULT, and every test
# appends a block to the single combined TEST_RESULT.txt in it.

KIT_VERSION="2.0"
KIT_DIR="${KIT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
BIN_DIR="$KIT_DIR/bin"
RESULT_DIR="${LIDARSCAN_RESULT_DIR:-$HOME/Desktop/LIDAR_TEST_RESULT}"
RESULT_FILE="$RESULT_DIR/TEST_RESULT.txt"

D6_SECONDS="${D6_SECONDS:-30}"
MID360_SECONDS="${MID360_SECONDS:-45}"
UM982_SECONDS="${UM982_SECONDS:-90}"
UM982_BAUD="${UM982_BAUD:-115200}"
MID360_PORTS="${MID360_PORTS:-56100,56101,56200,56201,56300,56301,56400,56401,56500,56501}"
HOST_IP_WANTED="192.168.1.50"
NETMASK="255.255.255.0"

C_RESET=$'\033[0m'
C_RED=$'\033[1;31m'
C_GREEN=$'\033[1;32m'
C_YELLOW=$'\033[1;33m'
C_CYAN=$'\033[1;36m'

LOGBUF=""
KIT_SUMMARY=""

# ---------------------------------------------------------------------------
# output helpers
# ---------------------------------------------------------------------------

banner() {   # banner "TEXT" COLOR
  local text="$1" color="${2:-$C_CYAN}"
  echo
  echo "${color}==============================================================${C_RESET}"
  echo "${color}   $text${C_RESET}"
  echo "${color}==============================================================${C_RESET}"
  echo
}

show_verdict() {   # show_verdict PASS|WARN|FAIL "headline"
  case "$1" in
    PASS) banner "SUCCESS - $2" "$C_GREEN" ;;
    WARN) banner "PARTLY WORKED - $2" "$C_YELLOW" ;;
    *)    banner "PROBLEM - $2" "$C_RED" ;;
  esac
}

send_back_note() {
  echo "${C_YELLOW}  WHAT TO DO NOW:${C_RESET}"
  echo "   Send the folder  LIDAR_TEST_RESULT  (it is on your Desktop)"
  echo "   back the same way you received this test. Send the WHOLE folder."
  echo
}

photo_note() {
  echo "${C_YELLOW}  If you are stuck: take a PHOTO of this whole window and send it.${C_RESET}"
  echo
}

wait_enter() {
  echo
  read -r -p "${1:-Press the ENTER key to continue} " _dummy
}

ask_yes_no() {   # ask_yes_no "question" default(y|n) -> returns 0 for yes
  local q="$1" def="${2:-y}" ans hint="[Y/n]"
  [ "$def" = "n" ] && hint="[y/N]"
  while true; do
    read -r -p "$q $hint " ans
    ans="$(echo "$ans" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')"
    [ -z "$ans" ] && ans="$def"
    case "$ans" in
      y|yes) return 0 ;;
      n|no)  return 1 ;;
      *) echo "  Please type y or n." ;;
    esac
  done
}

log() { LOGBUF="${LOGBUF}$1"$'\n'; }

save_block() {   # save_block "TITLE" PASS|WARN|FAIL
  local title="$1" verdict="$2"
  mkdir -p "$RESULT_DIR"
  if [ ! -f "$RESULT_FILE" ]; then
    {
      echo "LidarScan FIELD TEST RESULTS"
      echo "Send this whole folder back to the LidarScan team."
      echo
    } > "$RESULT_FILE"
  fi
  {
    echo
    echo "=================================================================="
    echo "  $title"
    echo "  run at $(date '+%Y-%m-%d %H:%M:%S')   kit v$KIT_VERSION"
    echo "  computer $(hostname -s)   macOS $(sw_vers -productVersion 2>/dev/null) $(uname -m)"
    echo "------------------------------------------------------------------"
    printf '%s' "$LOGBUF" | sed 's/^/  /'
    echo "------------------------------------------------------------------"
    echo "  RESULT: $verdict"
    echo "=================================================================="
  } >> "$RESULT_FILE"
  LOGBUF=""
  KIT_SUMMARY="${KIT_SUMMARY}$(printf '%-22s %s' "$title" "$verdict")"$'\n'
}

kit_init() {
  mkdir -p "$RESULT_DIR"
  # The bundled binaries are unsigned and arrive inside a downloaded .zip, so
  # macOS tags them com.apple.quarantine and refuses to run them. Clearing our
  # OWN files needs no admin rights and no Security-settings detour.
  xattr -d -r com.apple.quarantine "$BIN_DIR" 2>/dev/null
  chmod +x "$BIN_DIR"/* 2>/dev/null
  printf '\033]0;LidarScan field test\007'
}

require_bin() {   # require_bin NAME -> 0 if runnable
  if [ ! -x "$BIN_DIR/$1" ]; then
    show_verdict FAIL "part of the test kit is missing ($1)"
    echo "  Re-download the kit folder and unzip it again, keeping all the"
    echo "  files together in one folder."
    log "missing bundled tool: $BIN_DIR/$1"
    return 1
  fi
  return 0
}

kit_key() {   # kit_key FILE KEYNAME -> prints the value
  sed -n "s/^KEY $2 //p" "$1" | tail -1
}

# ---------------------------------------------------------------------------
# TEST 1 - COIN-D6
# ---------------------------------------------------------------------------

run_d6_test() {
  banner "TEST 1 of 3 - SPINNING LIDAR (COIN-D6)" "$C_CYAN"
  echo "  Takes about $D6_SECONDS seconds and runs by itself."
  echo "  Make sure the small silver/black lidar is plugged in by USB."
  echo

  require_bin d6cli || { save_block "TEST 1 D6 LIDAR" FAIL; return 1; }

  local out="$RESULT_DIR/d6_${D6_SECONDS}s.bin"
  local tmp; tmp="$(mktemp -t d6out)"
  log "capture file: $(basename "$out")"
  log "settings: 230400 8N1, DTR cleared, start AA55F00F / stop AA55F50A"

  echo "${C_CYAN}  Looking for the lidar...${C_RESET}"
  local ports; ports="$("$BIN_DIR/d6cli" --list 2>&1)"
  log "serial devices seen: $(echo "$ports" | tr '\n' ' ')"
  echo "$ports" | sed 's/^/    /'
  echo
  echo "${C_CYAN}  RECORDING. The lidar should be SPINNING now.${C_RESET}"
  echo

  "$BIN_DIR/d6cli" --seconds "$D6_SECONDS" --capture "$out" --plot none --quiet 2>&1 | tee "$tmp"
  echo

  local rate pkts_ok bad points bytes_in
  rate="$(sed -n 's/^checksum pass rate *: *\([0-9.]*\).*/\1/p' "$tmp" | tail -1)"
  pkts_ok="$(sed -n 's/^packets ok *: *\([0-9]*\).*/\1/p' "$tmp" | tail -1)"
  bad="$(sed -n 's/^packets bad cksum *: *\([0-9]*\).*/\1/p' "$tmp" | tail -1)"
  points="$(sed -n 's/^points *: *\([0-9]*\).*/\1/p' "$tmp" | tail -1)"
  bytes_in="$(sed -n 's/^bytes in *: *\([0-9]*\).*/\1/p' "$tmp" | tail -1)"
  rm -f "$tmp"

  log "bytes captured: ${bytes_in:-0}"
  log "packets ok: ${pkts_ok:-0}, bad checksum: ${bad:-0}"
  log "checksum pass rate: ${rate:-0} % (S1 exit criterion is > 99.5 %)"
  log "points decoded: ${points:-0}"

  local verdict="FAIL"
  if [ -n "$rate" ] && [ -n "$points" ] && [ "${points:-0}" -gt 0 ] 2>/dev/null; then
    if awk "BEGIN{exit !(${rate} > 99.5)}"; then verdict="PASS"; else verdict="WARN"; fi
  elif [ -n "$bytes_in" ] && [ "${bytes_in:-0}" -gt 20000 ] 2>/dev/null; then
    verdict="WARN"
  fi

  case "$verdict" in
    PASS)
      show_verdict PASS "THE LIDAR WORKS"
      echo "${C_GREEN}  Decoded $points real measurement points, $rate% of packets clean.${C_RESET}"
      echo
      send_back_note ;;
    WARN)
      show_verdict WARN "some lidar data arrived, but it is not clean"
      echo "${C_YELLOW}  Send the result anyway - it is still useful.${C_RESET}"
      echo "  Also tell us: was the lidar spinning the whole time?"
      echo
      send_back_note ;;
    *)
      show_verdict FAIL "the lidar sent nothing"
      echo "${C_YELLOW}  Check these, in order:${C_RESET}"
      echo "   1. The lidar's power/adapter board is plugged in."
      echo "   2. The USB cable is a DATA cable, not a charge-only one."
      echo "   3. No other program has the lidar open - close everything."
      echo "   4. Unplug the USB, count to five, plug it back in, retry."
      echo
      photo_note ;;
  esac
  save_block "TEST 1 D6 LIDAR" "$verdict"
  return 0
}

# ---------------------------------------------------------------------------
# TEST 2 - Livox Mid-360
# ---------------------------------------------------------------------------

mid360_my_addresses() {
  ifconfig 2>/dev/null | awk '/^[a-z0-9]+:/{iface=substr($1,1,length($1)-1)}
                              /inet 192\.168\.1\./{print iface" "$2}'
}

mid360_wired_interfaces() {
  # Ethernet-looking hardware ports, in the order macOS lists them.
  networksetup -listallhardwareports 2>/dev/null |
    awk '/^Hardware Port:/{p=$0} /^Device:/{if (p ~ /Ethernet|LAN|Thunderbolt Bridge/) print $2}'
}

mid360_manual_steps() {
  echo
  echo "${C_YELLOW}  HOW TO SET THE NETWORK ADDRESS BY HAND (2 minutes):${C_RESET}"
  echo "   1. Apple menu -> System Settings"
  echo "   2. Click  Network  in the left list"
  echo "   3. Click the Ethernet connection the lidar cable is plugged into"
  echo "   4. Click  Details...  then  TCP/IP"
  echo "   5. Set  Configure IPv4  to  Manually"
  echo "   6. Type these EXACTLY:"
  echo "        IP Address    $HOST_IP_WANTED"
  echo "        Subnet Mask   $NETMASK"
  echo "        Router        (leave empty)"
  echo "   7. Click  OK , then  Apply"
  echo "   8. Come back here and run this test again."
  echo
  echo "${C_YELLOW}  AFTERWARDS: set Configure IPv4 back to  Using DHCP  to get${C_RESET}"
  echo "${C_YELLOW}  normal internet on that cable again.${C_RESET}"
  echo
}

mid360_set_ip() {
  local ifaces; ifaces="$(mid360_wired_interfaces)"
  if [ -z "$ifaces" ]; then
    ifaces="$(ifconfig -l 2>/dev/null | tr ' ' '\n' | grep -E '^en[0-9]+$')"
  fi
  if [ -z "$ifaces" ]; then
    echo "${C_RED}  No wired network connection found on this Mac.${C_RESET}"
    echo "  The Mid-360 needs a cable. If you use a USB-C-to-Ethernet adapter,"
    echo "  plug it in and try again."
    log "auto-IP: no wired interface found"
    return 1
  fi

  local list; list=($ifaces)
  local chosen="${list[0]}"
  if [ "${#list[@]}" -gt 1 ]; then
    echo
    echo "${C_YELLOW}  Which network socket is the lidar cable in?${C_RESET}"
    local i=1
    for f in "${list[@]}"; do echo "   [$i] $f"; i=$((i+1)); done
    read -r -p "  Type a number and press ENTER: " sel
    if [ "$sel" -ge 1 ] 2>/dev/null && [ "$sel" -le "${#list[@]}" ] 2>/dev/null; then
      chosen="${list[$((sel-1))]}"
    else
      echo "  Not a valid number - using ${list[0]}."
    fi
  fi

  echo
  echo "${C_CYAN}  Giving $chosen the address $HOST_IP_WANTED ...${C_RESET}"
  echo "${C_YELLOW}  macOS will ask for YOUR MAC PASSWORD. Type it (nothing shows${C_RESET}"
  echo "${C_YELLOW}  on screen as you type) and press ENTER.${C_RESET}"
  log "auto-IP: sudo ifconfig $chosen alias $HOST_IP_WANTED $NETMASK"
  if sudo ifconfig "$chosen" alias "$HOST_IP_WANTED" "$NETMASK"; then
    echo "${C_GREEN}  Done.${C_RESET}"
    echo "  (To undo later:  sudo ifconfig $chosen -alias $HOST_IP_WANTED )"
    log "auto-IP: success; undo with -> sudo ifconfig $chosen -alias $HOST_IP_WANTED"
    return 0
  fi
  echo "${C_RED}  That did not work.${C_RESET}"
  log "auto-IP: sudo ifconfig failed"
  return 1
}

mid360_ping_sweep() {
  # The Mid-360 lives at 192.168.1.1XX (XX from its serial number).
  local i
  for i in $(seq 100 199); do
    ( ping -c 1 -W 400 "192.168.1.$i" >/dev/null 2>&1 && echo "192.168.1.$i" ) &
  done
  wait
}

run_mid360_test() {
  banner "TEST 2 of 3 - BIG ROUND LIDAR (Livox Mid-360)" "$C_CYAN"
  echo "${C_YELLOW}  BEFORE YOU START, check all three:${C_RESET}"
  echo "   1. The round lidar has its 12 volt power connected and whirrs faintly."
  echo "   2. Its network cable goes into this Mac (or into a USB-C-to-Ethernet"
  echo "      adapter plugged into this Mac)."
  echo "   3. Livox Viewer is CLOSED."
  echo
  wait_enter "When all three are done, press ENTER"

  require_bin mid360cap || { save_block "TEST 2 MID-360 LIDAR" FAIL; return 1; }
  log "ports bound: $MID360_PORTS"

  echo
  echo "${C_CYAN}  Checking this Mac's network address...${C_RESET}"
  local mine; mine="$(mid360_my_addresses)"
  # Test hook (smoke_macos.sh): pretend the address gate is already satisfied.
  [ "${LIDARSCAN_SKIP_NET_CHECK:-}" = "1" ] && mine="(network check skipped by test hook)"
  if [ -n "$mine" ]; then
    echo "$mine" | while read -r l; do echo "${C_GREEN}    OK - $l${C_RESET}"; done
    log "host address: $(echo "$mine" | tr '\n' ' ')"
  else
    echo "${C_RED}    This Mac has NO address on the lidar's network.${C_RESET}"
    log "host address: none in 192.168.1.x"
    echo
    echo "${C_YELLOW}  The lidar talks on addresses starting with 192.168.1.${C_RESET}"
    echo "${C_YELLOW}  This Mac needs the address $HOST_IP_WANTED${C_RESET}"
    echo
    if ask_yes_no "  Shall I set it for you? (asks for your Mac password)" y; then
      mid360_set_ip || mid360_manual_steps
    else
      log "auto-IP: tester declined"
      mid360_manual_steps
      if ! ask_yes_no "  Have you set it by hand just now?" n; then
        show_verdict FAIL "network address not set - cannot test this lidar yet"
        echo "  Follow the steps above, then run this test again."
        echo
        photo_note
        save_block "TEST 2 MID-360 LIDAR" FAIL
        return 0
      fi
    fi
  fi

  echo
  echo "${C_CYAN}  Looking for the lidar on the network (a few seconds)...${C_RESET}"
  local hits; hits="$(mid360_ping_sweep | sort -t. -k4 -n | tr '\n' ' ')"
  if [ -n "$hits" ]; then
    echo "${C_GREEN}    Replies from: $hits${C_RESET}"
    log "ping sweep 192.168.1.100-199: $hits"
  else
    echo "${C_YELLOW}    No reply yet (not fatal - carrying on).${C_RESET}"
    log "ping sweep 192.168.1.100-199: no replies"
  fi

  local out="$RESULT_DIR/mid360_${MID360_SECONDS}s.livoxdump"
  local tmp; tmp="$(mktemp -t m360out)"
  log "capture file: $(basename "$out")"

  echo
  echo "${C_CYAN}  RECORDING for $MID360_SECONDS seconds. Do not touch anything.${C_RESET}"
  echo
  "$BIN_DIR/mid360cap" --seconds "$MID360_SECONDS" --out "$out" --ports "$MID360_PORTS" 2>&1 | tee "$tmp"

  local total best_port best_rate
  total="$(kit_key "$tmp" total_datagrams)"
  best_port="$(kit_key "$tmp" busiest_port)"
  best_rate="$(kit_key "$tmp" busiest_rate_per_s)"
  local verdict; verdict="$(sed -n 's/^VERDICT //p' "$tmp" | tail -1)"
  [ -z "$verdict" ] && verdict="FAIL"

  log "duration: $(kit_key "$tmp" duration_s) s"
  log "total datagrams: ${total:-0}, total bytes: $(kit_key "$tmp" total_bytes)"
  while read -r l; do log "$l"; done < <(sed -n 's/^KEY port_/port /p' "$tmp")
  log "busiest port: ${best_port:-none} at ${best_rate:-0} datagrams/s (healthy point stream is above 1500/s)"
  rm -f "$tmp"

  case "$verdict" in
    PASS)
      show_verdict PASS "THE BIG LIDAR WORKS"
      echo "${C_GREEN}  It sent $total packets of scan data.${C_RESET}"
      echo
      send_back_note ;;
    WARN)
      show_verdict WARN "the lidar is talking, but slower than expected"
      echo "${C_YELLOW}  Data arrived, so the cable and address are right.${C_RESET}"
      echo "  It may be sending only status messages, not scan data yet."
      echo "  Send the result folder anyway."
      echo
      send_back_note ;;
    *)
      show_verdict FAIL "the big lidar sent nothing"
      echo "${C_YELLOW}  Check these, in order:${C_RESET}"
      echo "   1. 12 volt power on the lidar. It should whirr faintly."
      echo "   2. The network cable is clicked in at BOTH ends."
      echo "   3. This Mac's address is $HOST_IP_WANTED (see above)."
      echo "   4. The lidar has to be TOLD to send here. If it has never been"
      echo "      connected to this Mac: open Livox Viewer 2 once, let it find"
      echo "      the lidar, then QUIT Viewer completely and run this again."
      echo
      photo_note ;;
  esac
  save_block "TEST 2 MID-360 LIDAR" "$verdict"
  return 0
}

# ---------------------------------------------------------------------------
# TEST 3 - Unicore UM982
# ---------------------------------------------------------------------------

run_um982_test() {
  banner "TEST 3 of 3 - GPS / RTK RECEIVER (Unicore UM982)" "$C_CYAN"
  echo "${C_YELLOW}  BEFORE YOU START, check all three:${C_RESET}"
  echo "   1. At least ONE antenna is screwed onto the GPS board. The socket"
  echo "      marked ANT1 (or ANT/MAIN) is the one that matters. Finger tight."
  echo "   2. The antenna is OUTSIDE, or right against a window with a clear"
  echo "      view of the sky. In the middle of a room it will never work."
  echo "   3. The GPS board's USB cable is plugged into this Mac."
  echo
  echo "${C_YELLOW}  A GPS switched on in a new place can take 1-2 minutes to find${C_RESET}"
  echo "${C_YELLOW}  satellites. That is normal.${C_RESET}"
  echo
  wait_enter "When all three are done, press ENTER"

  require_bin um982cap || { save_block "TEST 3 GPS UM982" FAIL; return 1; }

  local out="$RESULT_DIR/um982_${UM982_SECONDS}s.nmea"
  local tmp; tmp="$(mktemp -t um982out)"
  log "capture file: $(basename "$out")"
  log "requested duration: $UM982_SECONDS s, preferred baud: $UM982_BAUD"

  echo
  echo "${C_CYAN}  Looking for the GPS receiver...${C_RESET}"
  # UM982_PORT skips the probe. Set it when we already know the device path
  # (support case: "run it with UM982_PORT=/dev/cu.usbserial-XXXX").
  local port_args=()
  [ -n "${UM982_PORT:-}" ] && port_args=(--port "$UM982_PORT")
  "$BIN_DIR/um982cap" --seconds "$UM982_SECONDS" --baud "$UM982_BAUD" \
      --out "$out" "${port_args[@]}" 2>&1 | tee "$tmp"

  local verdict; verdict="$(sed -n 's/^VERDICT //p' "$tmp" | tail -1)"
  [ -z "$verdict" ] && verdict="FAIL"
  local sats best_fix_name rate cs_pct uni
  sats="$(kit_key "$tmp" max_satellites)"
  best_fix_name="$(kit_key "$tmp" best_fix_name)"
  rate="$(kit_key "$tmp" sentences_per_s)"
  cs_pct="$(kit_key "$tmp" checksum_pct)"
  uni="$(kit_key "$tmp" unicore_lines)"

  while read -r l; do log "$l"; done < <(sed -n 's/^KEY //p' "$tmp")
  rm -f "$tmp"

  case "$verdict" in
    PASS)
      show_verdict PASS "THE GPS WORKS"
      echo "${C_GREEN}  Locked onto $sats satellites: $best_fix_name${C_RESET}"
      echo "${C_GREEN}  (It says SINGLE, not RTK. That is exactly right here -${C_RESET}"
      echo "${C_GREEN}   centimetre RTK needs a correction service we are not${C_RESET}"
      echo "${C_GREEN}   using in this test.)${C_RESET}"
      echo
      send_back_note ;;
    WARN)
      if [ -n "$rate" ] && awk "BEGIN{exit !(${rate:-0} >= 3)}" &&
         [ -n "$cs_pct" ] && awk "BEGIN{exit !(${cs_pct:-0} >= 99)}"; then
        show_verdict WARN "the GPS is alive but has not found satellites yet"
        echo "${C_YELLOW}  The receiver is working perfectly - it is talking to the Mac${C_RESET}"
        echo "${C_YELLOW}  and every message is clean. It just cannot see enough sky.${C_RESET}"
        echo
        echo "  To fix: put the antenna outdoors or right against a window,"
        echo "  wait two minutes, and run this test once more."
      else
        show_verdict WARN "the GPS data arrived damaged or too slowly"
        echo "${C_YELLOW}  Messages arrived but are garbled or sparse - usually a wrong${C_RESET}"
        echo "${C_YELLOW}  speed setting or a poor USB cable. Try another cable.${C_RESET}"
      fi
      echo "  Send the result folder either way."
      echo
      send_back_note ;;
    *)
      show_verdict FAIL "no GPS receiver found, or it sent nothing"
      echo "${C_YELLOW}  Try these in order:${C_RESET}"
      echo "   1. Push the GPS board's USB plug in fully; try another socket."
      echo "   2. Is a light on the board lit? If not it has no power."
      echo "   3. Unplug it, count to five, plug it back in, retry."
      echo "   4. Look at the small chip on the board next to the USB socket:"
      echo "        marked CH340  -> macOS 14 and newer have it built in."
      echo "        marked CP2102 -> tell us; that one needs a driver."
      echo "   5. Close any other GPS software and retry."
      echo
      photo_note ;;
  esac
  [ -n "${uni:-}" ] && [ "$uni" != "0" ] && echo "  (Saw $uni Unicore dual-antenna lines - good sign.)"
  save_block "TEST 3 GPS UM982" "$verdict"
  return 0
}

# ---------------------------------------------------------------------------

kit_summary() {
  banner "RESULTS" "$C_CYAN"
  if [ -z "$KIT_SUMMARY" ]; then
    echo "  Nothing has been tested yet."
  else
    printf '%s' "$KIT_SUMMARY" | sed 's/^/   /'
  fi
  echo
  echo "  Everything is saved in:  $RESULT_DIR"
  echo
  send_back_note
}

kit_finish() {
  kit_summary
  wait_enter "Press the ENTER key to close this window"
}
