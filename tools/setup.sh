#!/usr/bin/env bash
# Optional dependency installer for Debian/Ubuntu/LMDE.
set -euo pipefail
sudo apt update
sudo apt install -y \
    build-essential cmake ninja-build git \
    gcc-arm-none-eabi libnewlib-arm-none-eabi \
    python3 xorriso dosfstools mtools \
    sg3-utils wodim
echo
echo "Done. Build with ./tools/build.sh"
