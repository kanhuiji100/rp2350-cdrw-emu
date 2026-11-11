#!/usr/bin/env bash
# Build the RP2350 CD-RW firmware.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${PICO_SDK_PATH:-$ROOT/third_party/pico-sdk}"
BOARD="${PICO_BOARD:-pico2}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

if [[ ! -f "$SDK/pico_sdk_init.cmake" ]]; then
    echo "Pico SDK not found at $SDK" >&2
    echo "Set PICO_SDK_PATH or clone the SDK into third_party/pico-sdk." >&2
    exit 1
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DPICO_SDK_PATH="$SDK" \
    -DPICO_BOARD="$BOARD" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

VERSION="$(sed -n 's/.*CDRW_VERSION "\([^"]*\)".*/\1/p' "$ROOT/src/version.h" | head -n1)"
VERSION="${VERSION:-unknown}"
mkdir -p "$ROOT/dist"
RELEASE="$ROOT/dist/cdrw_emulator_v${VERSION}.uf2"
cp "$BUILD_DIR/cdrw_emulator.uf2" "$RELEASE"
( cd "$ROOT" && sha256sum "dist/cdrw_emulator_v${VERSION}.uf2" > dist/SHA256SUMS )
echo
echo "Firmware: $BUILD_DIR/cdrw_emulator.uf2"
echo "Release copy: $RELEASE"
