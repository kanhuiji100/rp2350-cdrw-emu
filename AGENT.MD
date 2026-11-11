# AGENT.MD — RP2350 USB CD-RW emulator

This is the entry point for a new developer or an AI coding agent. Read
this, then `TODO.MD`, then `README.md` and `docs/`.

## Project identity

- **Positioning:** plug-and-play, low-cost, USB 1.1 CD-RW emulator.
- **Hardware:** Raspberry Pi Pico 2 / RP2350A, microSD over SPI, optional
  SSD1306 OLED and EC11 encoder. No host PC daemon needed.
- **Host view:** SCSI MMC optical drive (`PDT=5`), CD-ROM or CD-RW.
- **Sibling project:** `pi3b-cdrw-emu` — high-capacity, high-performance
  USB 2.0 CD/DVD/BD emulator. This project intentionally does **not**
  implement DVD/BD, USB 2.0 or FunctionFS.

## Build and test

```bash
# Host-side unit tests (no Pico required)
bash tests/run.sh

# Firmware (Pico SDK vendored in third_party/pico-sdk)
./tools/build.sh
# -> build/cdrw_emulator.uf2
# -> dist/cdrw_emulator_v1.2.0.uf2
```

The host tests compile `src/scsi.c` and `src/util.c` with stubs for media,
plus a TinyUSB BOT unit test and the Python image tool.

Dependencies on Debian:

```bash
sudo apt install build-essential cmake ninja-build git \
  gcc-arm-none-eabi libnewlib-arm-none-eabi \
  libstdc++-arm-none-eabi-newlib python3 xorriso dosfstools mtools
```

## Architecture

```
host -> TinyUSB MSC/BOT -> src/usb_cdrom.c
     -> src/scsi.c (MMC subset, shared behavior with pi3b)
     -> src/media.c (CD-RW image state, .meta)
     -> src/sd_spi.c / third_party/fatfs
     -> src/oled.c / encoder.c / ui.c
```

Important files:

| File | Responsibility |
|---|---|
| `src/scsi.c` | MMC commands, sense, CD-RW profiles/state |
| `src/media.c` | image open/blank/format/write/close, meta v2 |
| `src/media_meta.h` | 48-byte little-endian metadata |
| `src/usb_cdrom.c` | TinyUSB BOT, write/read pipeline |
| `src/storage.c` | SD/FatFs image and `.meta` storage |
| `src/ui.c` | OLED/encoder menus |
| `tests/` | host SCSI/BOT/tool tests |

## Invariants and hard-won knowledge

1. CD-RW only. Do not add DVD/BD profiles without a separate design;
   the USB 1.1 link and SD card cannot provide high-capacity performance.
2. **A4 REPORT KEY** returns `00 06 00 00 25 ff 01 00`; a CHECK CONDITION
   makes Windows reset the device.
3. **GET EVENT** advertises classes `0x5E`; class `0x5A` returns
   `00 06 06 5E ...`. This is required for Windows format polling.
4. **FORMAT UNIT/BLANK are asynchronous:** TUR reports `2/04/04` during
   format and `2/04/07` during erase, then READY.
5. **Disc Information:** blank 36 bytes; complete 34 bytes, `0x1E`,
   bytes 12–15 `00 02 99 00`.
6. **TOC:** CD-RW control `0x15`; format 0 lead-out `359336`, format 2
   lead-out `359848`. Do not synthesize a TOC before UDF data exists.
7. **Track:** blank 48 bytes, flags `4F/01`; complete 36 bytes,
   flags `B1/02`.
8. **CLOSE TRACK/SESSION:** function 2 closes without finalizing;
   function 3 finalizes. This is required by ImgBurn multi-session.
9. Meta is **version 2** with `FORMATTED` flag; the loader still accepts
   v1 sidecars. `tools/cdrw.py` writes v2.
10. Do not commit `build/`, `build-host/`, `discs/`, captures or firmware
    signing secrets. Release UF2 artifacts may be force-added intentionally.

## Current known issue (P0)

Windows built-in **Live File System** formatting still fails (“Windows
无法完成格式化”). ImgBurn and Linux cdrecord paths work. The CD-RW SCSI
behaviors above have been synced from the Orange Pi project; the remaining
difference is on the Windows formatter/BOT side. See `TODO.MD`.

The RP2350 BOT path is TinyUSB and cannot use the Linux FunctionFS
short-packet workaround. If Windows fails during an IN command, inspect
`src/usb_cdrom.c` error/CSW handling rather than blindly changing SCSI
fields.

## Sibling sync

The Orange Pi `pi3b-cdrw-emu` is the reference for CD-RW protocol
behavior. When changing shared CD-RW semantics, update both projects and
run both test suites. DVD/BD and FunctionFS code stay in the Orange Pi
project.

## Working style

- Run `bash tests/run.sh` after every SCSI/media change.
- Run `./tools/build.sh` before claiming firmware works.
- Prefer captures/real-device evidence over speculative changes.
- Keep the firmware small and USB-1.1-friendly; avoid blocking loops.
- Never store Wi-Fi/GitHub tokens in the repo.
