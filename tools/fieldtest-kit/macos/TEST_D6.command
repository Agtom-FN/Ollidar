#!/bin/bash
# LidarScan field test - small spinning lidar (COIN-D6). Double-click to run.
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
KIT_DIR="$(pwd)"; export KIT_DIR
source "$KIT_DIR/lib/common.sh"
kit_init
banner "LIDARSCAN EQUIPMENT TEST - SMALL SPINNING LIDAR" "$C_CYAN"
run_d6_test
kit_finish
