#!/usr/bin/env bash
# Flash the UF2.  Prefer picotool; otherwise copy to a mounted RPI-RP2 volume.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# -ge 1 ]]; then
    UF2="$1"
else
    UF2="$(ls -1 "$ROOT"/dist/cdrw_emulator_v*.uf2 2>/dev/null | sort -V | tail -n1 || true)"
    UF2="${UF2:-$ROOT/build/cdrw_emulator.uf2}"
fi

if [[ ! -f "$UF2" ]]; then
    echo "UF2 not found: $UF2" >&2
    echo "Run ./tools/build.sh first." >&2
    exit 1
fi

if command -v picotool >/dev/null 2>&1; then
    picotool load -f "$UF2" -x
    echo "Flashed with picotool."
    exit 0
fi

# Common automount locations on Linux.
for base in "/media/$USER" "/run/media/$USER"; do
    if [[ -d "$base/RPI-RP2" ]]; then
        cp "$UF2" "$base/RPI-RP2/"
        sync
        echo "Copied to $base/RPI-RP2. The board will reboot."
        exit 0
    fi
done

cat >&2 <<'EOF'
No picotool and no RPI-RP2 volume found.

1. Hold BOOTSEL while plugging the Pico 2 into USB.
2. Copy dist/cdrw_emulator_v1.1.0.uf2 to the RPI-RP2 drive.
3. The board reboots automatically.

Or install picotool and rerun this script.
EOF
exit 1
