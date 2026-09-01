#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON
cmake --build "$ROOT/build" -j
echo "Built: $ROOT/build/cloud_server $ROOT/build/cloud_client $ROOT/build/cloud_gui"
