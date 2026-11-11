#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build-host
cc -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -DCDRW_HOST_TEST \
    -Isrc \
    tests/test_scsi.c src/scsi.c src/util.c \
    -o build-host/test_scsi
./build-host/test_scsi

cc -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -DCDRW_HOST_TEST \
    -DCFG_TUSB_MCU=OPT_MCU_RP2040 -DCFG_TUSB_OS=OPT_OS_NONE -DCFG_TUSB_DEBUG=0 \
    -Isrc -Ithird_party/pico-sdk/lib/tinyusb/src \
    tests/test_usb_bot.c src/usb_cdrom.c src/scsi.c src/util.c \
    -o build-host/test_usb_bot
./build-host/test_usb_bot
python3 tests/test_tool.py

# Optional FatFs + MBR/FAT32 smoke test (needs dosfstools + mtools).
if command -v mkfs.vfat >/dev/null && command -v mcopy >/dev/null; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    dd if=/dev/zero of="$tmp/sd.img" bs=1M count=64 status=none
    python3 - "$tmp/sd.img" <<'PY'
import struct, sys
path = sys.argv[1]
with open(path, 'r+b') as f:
    mbr = bytearray(512)
    mbr[510:512] = b'\x55\xaa'
    ent = bytearray(16)
    ent[4] = 0x0c
    ent[5:8] = b'\xfe\xff\xff'
    struct.pack_into('<I', ent, 8, 2048)
    struct.pack_into('<I', ent, 12, 64 * 1024 * 1024 // 512 - 2048)
    mbr[446:462] = ent
    f.write(mbr)
PY
    mkfs.vfat -F 32 --offset 2048 -n RPCDRW "$tmp/sd.img" >/dev/null
    printf 'hello world\n' >"$tmp/hello.txt"
    mcopy -i "$tmp/sd.img@@1M" "$tmp/hello.txt" ::HELLO.TXT
    cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
        -Ithird_party/fatfs \
        tests/fatfs_smoke.c third_party/fatfs/ff.c third_party/fatfs/ffunicode.c \
        -o build-host/fatfs_smoke
    ./build-host/fatfs_smoke "$tmp/sd.img" | tee "$tmp/fatfs.out"
    grep -q "hello.txt (12): hello world" "$tmp/fatfs.out"
    grep -q "f_expand 4MiB: 0" "$tmp/fatfs.out"
    echo "fatfs smoke: all passed"
fi

echo "all host tests passed"
