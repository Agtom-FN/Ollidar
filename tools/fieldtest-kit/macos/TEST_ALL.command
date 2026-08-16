#!/bin/bash
# LidarScan field test - everything, one test after the other.
# THIS IS THE ONE TO DOUBLE-CLICK IF YOU ARE UNSURE.
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
KIT_DIR="$(pwd)"; export KIT_DIR
source "$KIT_DIR/lib/common.sh"
kit_init

banner "LIDARSCAN EQUIPMENT TEST - ALL THREE DEVICES" "$C_CYAN"
echo "  Roughly four minutes in total. Each test explains itself."
echo "  If a device is not connected, that test says so and the next one"
echo "  still runs. Everything lands in one folder on your Desktop:"
echo
echo "    $RESULT_DIR"
echo
wait_enter "Press ENTER to begin"

run_d6_test
wait_enter "Press ENTER for the next test"
run_mid360_test
wait_enter "Press ENTER for the next test"
run_um982_test

kit_finish
