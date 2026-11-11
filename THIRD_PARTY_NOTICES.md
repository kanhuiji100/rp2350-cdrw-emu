# Third-party notices

This project vendors or links the following third-party components.  Their
licenses remain in effect; the MIT license in `LICENSE` applies only to the
original project code.

## Raspberry Pi Pico SDK 2.1.1

Location: `third_party/pico-sdk`

Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
Licensed under the BSD 3-Clause license.  See the license headers in the SDK
sources and `third_party/pico-sdk/LICENSE.TXT`.

## TinyUSB

Location: `third_party/pico-sdk/lib/tinyusb`

Copyright (c) 2018, hathach (tinyusb.org)
Licensed under the MIT license.  See `third_party/pico-sdk/lib/tinyusb/LICENSE`.

## FatFs R0.15

Location: `third_party/fatfs`

Copyright (C) 2025, ChaN, all right reserved.
FatFs is distributed under the FatFs license; see
`third_party/fatfs/LICENSE.txt`.

## Adafruit GFX 5x7 font data

Location: `src/font5x7.c`

The classic 5x7 fixed-space font data originates from Adafruit GFX
(https://github.com/adafruit/Adafruit-GFX-Library), whose license is BSD.
The glyphs are a classic public-domain-style bitmap font.  Only the font table
for ASCII 0x20-0x7E is vendored here.

## MMC / SCSI command references

The implementation was written against publicly available SCSI MMC drafts and
Linux/QEMU optical command implementations.  No source code from those
projects is included except the general protocol knowledge.
