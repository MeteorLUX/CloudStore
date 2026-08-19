#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sudo apt-get update
sudo apt-get install -y g++ cmake pkg-config libevent-dev libjsoncpp-dev \
    libhiredis-dev libssl-dev libmysqlclient-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
echo "binaries: build/cloud_server  build/cloud_client"
