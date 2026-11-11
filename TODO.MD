# TODO — RP2350 USB CD-RW emulator

**Positioning:** plug-and-play, low-cost, USB 1.1 CD-RW emulator.
For high-capacity DVD/BD and USB 2.0 performance, use
`pi3b-cdrw-emu`.

## P0 — Windows Live File System

- [ ] Make Windows Explorer “类似于 U 盘” formatting complete.
- [ ] Verify the freshly built v1.2.0 firmware on hardware; the CD-RW
      protocol sync currently has host-test + firmware build coverage only.
- [ ] Capture Windows Explorer live-FS formatting on the RP2350 and compare
      post-FORMAT `READ TRACK`/`WRITE` with the real-drive capture.
- [ ] Inspect TinyUSB BOT failed-IN handling in `src/usb_cdrom.c`; unlike
      Linux FunctionFS, TinyUSB can stall endpoints, so a protocol-correct
      error path is possible.

## P1 — Hardware validation of v1.2.0

- [ ] Flash `dist/cdrw_emulator_v1.2.0.uf2` and verify enumeration.
- [ ] ImgBurn erase → burn → verify on a 700 MB `.CDR`.
- [ ] ImgBurn multi-session append after the non-finalizing close fix.
- [ ] Linux `cdrecord`/Kylin read/write.
- [ ] OLED + EC11: load, new blank, erase, delete.
- [ ] Check blank-disc Explorer “刻录光盘” dialog still opens.

## P1 — CD-RW protocol regressions

- [ ] Add host tests for:
  - [ ] A4 REPORT KEY response
  - [ ] GET EVENT 0x5A/0x5E
  - [ ] async BLANK/FORMAT TUR (`2/04/07`, `2/04/04`)
  - [ ] complete DiscInfo 34 / blank 36
  - [ ] TOC control/lead-out and TOC-before-write failure
  - [ ] Track 48 blank / 36 complete
  - [ ] format capacities 358400 + 295264
- [ ] Verify `tools/cdrw.py` v2 metadata with old v1 images.

## P2 — Performance and robustness

- [ ] Document measured USB 1.1 throughput and SD SPI clock.
- [ ] Check SD write pipeline on slow cards; avoid watchdog/timeouts.
- [ ] Add a release checksum update to `tools/build.sh`.
- [ ] Keep `dist/` release artifacts reproducible.

## Sync status with pi3b-cdrw-emu

Ported:
- [x] A4 REPORT KEY
- [x] GET EVENT 0x5E/0x5A
- [x] async BLANK/FORMAT
- [x] DiscInfo 34/36 and complete fields
- [x] TOC control/lead-outs
- [x] Track 48/36
- [x] format capacity 295264/physical 358400
- [x] meta v2 and non-finalizing close session

Not applicable / intentionally not ported:
- [ ] DVD-RW / BD-RE profiles and Disc Structure
- [ ] FunctionFS / USB 2.0 stack
- [ ] Linux OLED/i2c-dev backend
- [ ] BOT failed-IN short-packet workaround (TinyUSB path differs)
