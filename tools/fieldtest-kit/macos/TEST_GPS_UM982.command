#!/bin/bash
# LidarScan field test - GPS / RTK receiver (Unicore UM982). Double-click to run.
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
KIT_DIR="$(pwd)"; export KIT_DIR
source "$KIT_DIR/lib/common.sh"
kit_init
banner "LIDARSCAN EQUIPMENT TEST - GPS RECEIVER" "$C_CYAN"
run_um982_test
kit_finish
