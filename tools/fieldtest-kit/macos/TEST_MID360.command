#!/bin/bash
# LidarScan field test - big round lidar (Livox Mid-360). Double-click to run.
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1
KIT_DIR="$(pwd)"; export KIT_DIR
source "$KIT_DIR/lib/common.sh"
kit_init
banner "LIDARSCAN EQUIPMENT TEST - BIG ROUND LIDAR" "$C_CYAN"
run_mid360_test
kit_finish
