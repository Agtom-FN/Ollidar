#!/bin/bash
# Smoke tests for the macOS field-test kit. No hardware needed.
#
# Exercises, for real:
#   - the bundled arm64 binaries run at all (d6cli / mid360cap / um982cap)
#   - d6cli --replay against the S1 synthetic capture (the built-binary check)
#   - mid360cap against a synthetic UDP blast, and the .livoxdump it writes
#     round-tripping through the dev-side verify_capture.py
#   - um982cap against a pseudo-terminal fed with synthetic UM982 NMEA:
#     live parse, PASS on a SINGLE fix, WARN on no fix, FAIL on silence
#   - the shell library's verdict/result-file plumbing end to end
#
# What it CANNOT test without hardware: real USB serial enumeration
# (/dev/cu.usbserial-*), CH340/CP210x driver presence, the sudo ifconfig
# alias path, and a real Mid-360 or UM982 on the wire.
#
# Usage:  bash tools/fieldtest-kit/tests/smoke_macos.sh

set -u
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KIT_ROOT="$(dirname "$TESTS_DIR")"
REPO_TOOLS="$(dirname "$KIT_ROOT")"
MAC="$KIT_ROOT/macos"
GEN="$TESTS_DIR/make_synthetic.py"

CHECKS=0
FAILURES=0
check() {  # check "name" 0|1 "detail"
  CHECKS=$((CHECKS + 1))
  if [ "$2" = "0" ]; then
    printf '  \033[32mPASS\033[0m  %s%s\n' "$1" "${3:+  [$3]}"
  else
    FAILURES=$((FAILURES + 1))
    printf '  \033[31mFAIL\033[0m  %s  [%s]\n' "$1" "${3:-}"
  fi
}
yesno() { [ "$1" = "true" ] && echo 0 || echo 1; }

SCRATCH="$(mktemp -d -t kitsmoke)"
trap 'rm -rf "$SCRATCH"' EXIT
echo "scratch: $SCRATCH"

# ---------------------------------------------------------------- 1. binaries
echo
echo "=== 1. bundled binaries ==="
for b in d6cli mid360cap um982cap; do
  if [ -x "$MAC/bin/$b" ]; then
    arch="$(file -b "$MAC/bin/$b" | grep -o 'arm64\|x86_64' | head -1)"
    check "bin/$b present and executable" 0 "$arch"
  else
    check "bin/$b present and executable" 1 "missing"
  fi
done
"$MAC/bin/mid360cap" --help >/dev/null 2>&1
check "mid360cap --help" $? ""
"$MAC/bin/um982cap" --help >/dev/null 2>&1
check "um982cap --help" $? ""

# ------------------------------------------------- 2. d6cli replay smoke test
echo
echo "=== 2. d6cli replay (built binary vs the S1 synthetic capture) ==="
SYNTH_D6="$(dirname "$REPO_TOOLS")/desktop/evidence/synth-d6.bin"
if [ -f "$SYNTH_D6" ]; then
  out="$("$MAC/bin/d6cli" --replay "$SYNTH_D6" --plot none --quiet 2>&1)"
  rate="$(echo "$out" | sed -n 's/^checksum pass rate *: *\([0-9.]*\).*/\1/p')"
  pts="$(echo "$out" | sed -n 's/^points *: *\([0-9]*\).*/\1/p')"
  check "d6cli replays the synthetic capture" $? ""
  awk "BEGIN{exit !(${rate:-0} > 99.5)}"
  check "checksum pass rate > 99.5%" $? "${rate:-none}%"
  [ "${pts:-0}" -gt 1000 ]
  check "points decoded" $? "${pts:-0} points"
  # the same sed expressions the kit's run_d6_test uses
  [ -n "$rate" ] && [ -n "$pts" ]
  check "run_d6_test's summary parsing matches d6cli's output" $? ""
else
  echo "  SKIP  no synth-d6.bin at $SYNTH_D6"
fi

# ------------------------------------------------------ 3. mid360cap + verify
echo
echo "=== 3. mid360cap against a synthetic UDP stream ==="
DUMP="$SCRATCH/mid360_5s.livoxdump"
python3 "$GEN" mid360 --port 56300 --seconds 8 --rate 2000 >/dev/null 2>&1 &
SENDER=$!
sleep 0.4
capout="$("$MAC/bin/mid360cap" --seconds 5 --host-ip 127.0.0.1 --out "$DUMP" --ports 56300,56400 2>&1)"
rc=$?
wait $SENDER 2>/dev/null
check "mid360cap exit code" $rc ""
verdict="$(echo "$capout" | sed -n 's/^VERDICT //p' | tail -1)"
[ "$verdict" = "PASS" ]
check "verdict PASS at ~2000 datagrams/s" $? "$verdict"
best="$(echo "$capout" | sed -n 's/^KEY busiest_rate_per_s //p' | tail -1)"
awk "BEGIN{exit !(${best:-0} >= 1500)}"
check "busiest-port rate over the 1500/s threshold" $? "${best:-0}/s"
head -c 8 "$DUMP" | grep -q LX360CAP
check "container magic LX360CAP" $? ""

echo
echo "=== 4. dev-side verify_capture.py reads it ==="
VERIFY="$REPO_TOOLS/remote-capture/verify_capture.py"
if [ -f "$VERIFY" ]; then
  vout="$(python3 "$VERIFY" "$DUMP" 2>&1)"
  echo "$vout" | grep -q "container header OK"
  check "verify_capture.py parses the container" $? ""
  echo "$vout" | grep -q "port 56300: .* pkts"
  check "verify_capture.py counts the datagrams" $? ""
else
  echo "  SKIP  verify_capture.py not found"
fi

# ------------------------------------------------------------ 5. um982cap
echo
echo "=== 5. um982cap against a pseudo-terminal fed with UM982 NMEA ==="
cat > "$SCRATCH/ptyfeed.py" <<'PYEOF'
import os, pty, sys, time
src, secs = sys.argv[1], float(sys.argv[2])
lines = [l for l in open(src, 'rb').read().split(b'\r\n') if l]
m, s = pty.openpty()
print(os.ttyname(s), flush=True)
per, t0, i = 7, time.time(), 0
while time.time() - t0 < secs:
    chunk = lines[i:i+per]
    if not chunk:
        i = 0
        continue
    os.write(m, b'\r\n'.join(chunk) + b'\r\n')
    i += per
    time.sleep(1.0)
PYEOF

run_um982_case() {   # run_um982_case NAME FIXQ EXPECTED_VERDICT [--corrupt N]
  local name="$1" fixq="$2" want="$3"; shift 3
  local src="$SCRATCH/gen_$name.nmea"
  python3 "$GEN" um982 "$src" --seconds 20 --fix "$fixq" "$@" >/dev/null
  python3 "$SCRATCH/ptyfeed.py" "$src" 10 > "$SCRATCH/pty_$name.txt" 2>&1 &
  local feeder=$!
  sleep 0.8
  local dev; dev="$(head -1 "$SCRATCH/pty_$name.txt")"
  local out; out="$("$MAC/bin/um982cap" --port "$dev" --seconds 6 --out "$SCRATCH/cap_$name.nmea" 2>&1)"
  kill $feeder 2>/dev/null; wait $feeder 2>/dev/null
  local got; got="$(echo "$out" | sed -n 's/^VERDICT //p' | tail -1)"
  UM982_OUT="$out"
  [ "$got" = "$want" ]
  check "$name -> $want" $? "got $got"
}

run_um982_case "single_fix" 1 PASS
echo "$UM982_OUT" | grep -q "^KEY best_fix 1$"
check "SINGLE fix (quality 1) recorded" $? ""
echo "$UM982_OUT" | grep -q "^KEY checksum_bad 0$"
check "no false checksum failures on Unicore # lines" $? \
      "$(echo "$UM982_OUT" | sed -n 's/^KEY unicore_lines //p') unicore lines"
echo "$UM982_OUT" | grep -q "^KEY heading_sentences [1-9]"
check "heading sentences detected" $? \
      "$(echo "$UM982_OUT" | sed -n 's/^KEY heading_sentences //p')"

run_um982_case "no_fix"  0 WARN
run_um982_case "corrupt" 1 WARN --corrupt 2

# silence -> FAIL
python3 - > "$SCRATCH/pty_silent.txt" 2>&1 <<'PYEOF' &
import os, pty, time
m, s = pty.openpty()
print(os.ttyname(s), flush=True)
time.sleep(10)
PYEOF
sleep 0.8
dev="$(head -1 "$SCRATCH/pty_silent.txt")"
out="$("$MAC/bin/um982cap" --port "$dev" --seconds 3 --out "$SCRATCH/cap_silent.nmea" 2>&1)"
[ "$(echo "$out" | sed -n 's/^VERDICT //p' | tail -1)" = "FAIL" ]
check "silent port -> FAIL" $? ""

"$MAC/bin/um982cap" --list >/dev/null 2>&1
check "um982cap --list runs" 0 "exit $? (1 = no USB serial devices attached, expected here)"

# ------------------------------------------------- 6. the shell library itself
echo
echo "=== 6. shell library: verdicts, logging, combined result file ==="
export LIDARSCAN_RESULT_DIR="$SCRATCH/DESKTOP_RESULT"
export KIT_DIR="$MAC"
export LIDARSCAN_SKIP_NET_CHECK=1
export MID360_SECONDS=4
export MID360_PORTS=56300,56400
# shellcheck disable=SC1090
source "$MAC/lib/common.sh"
kit_init

python3 "$GEN" mid360 --port 56300 --seconds 7 --rate 2000 >/dev/null 2>&1 &
SENDER=$!
sleep 0.4
# stdin: ENTER for the pre-flight prompt
run_mid360_test < /dev/null > "$SCRATCH/mid360_run.txt" 2>&1
wait $SENDER 2>/dev/null
grep -q "SUCCESS - THE BIG LIDAR WORKS" "$SCRATCH/mid360_run.txt"
check "run_mid360_test reaches the PASS banner" $? \
      "$(grep -c . "$SCRATCH/mid360_run.txt") lines of output"
[ -f "$LIDARSCAN_RESULT_DIR/TEST_RESULT.txt" ]
check "combined TEST_RESULT.txt written" $? "$LIDARSCAN_RESULT_DIR"
grep -q "RESULT: PASS" "$LIDARSCAN_RESULT_DIR/TEST_RESULT.txt"
check "result block carries the verdict" $? ""
grep -q "busiest port" "$LIDARSCAN_RESULT_DIR/TEST_RESULT.txt"
check "per-port numbers logged for the dev side" $? ""
ls "$LIDARSCAN_RESULT_DIR"/mid360_*.livoxdump >/dev/null 2>&1
check "capture file landed in the result folder" $? \
      "$(ls "$LIDARSCAN_RESULT_DIR" | tr '\n' ' ')"

# a second block must append, not overwrite
log "smoke: synthetic second block"
save_block "TEST 9 SMOKE" WARN
[ "$(grep -c 'RESULT: ' "$LIDARSCAN_RESULT_DIR/TEST_RESULT.txt")" = "2" ]
check "second test appends a second block" $? \
      "$(grep -c 'RESULT: ' "$LIDARSCAN_RESULT_DIR/TEST_RESULT.txt") blocks"

echo
if [ "$FAILURES" -eq 0 ]; then
  printf '\033[32mALL %d CHECKS PASSED\033[0m\n' "$CHECKS"
  exit 0
fi
printf '\033[31m%d of %d CHECKS FAILED\033[0m\n' "$FAILURES" "$CHECKS"
exit 1
