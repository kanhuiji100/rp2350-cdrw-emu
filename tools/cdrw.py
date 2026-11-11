#!/usr/bin/env python3
"""
Create, import, inspect and blank virtual CD-RW images for the RP2350 CD-RW
emulator.

A .CDR image is a raw sequence of 2048-byte Mode-1 user sectors.  Its sidecar
.CDR.meta file stores the tiny MMC state block that the firmware also uses.
The image itself can be written by any optical burning application; this tool
is for preparing discs and moving ISO images onto the SD card offline.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

SECTOR = 2048
META_MAGIC = b"RPCDRW01"
META_VERSION = 2
META_STRUCT = struct.Struct("<8sHHIIII16sI")
assert META_STRUCT.size == 48

FLAG_BLANK = 1 << 0
FLAG_COMPLETE = 1 << 1
FLAG_SESSION_OPEN = 1 << 2
FLAG_WRITE_PROTECTED = 1 << 3
FLAG_FORMATTED = 1 << 4

DEFAULT_MB = 700


@dataclass
class Meta:
    capacity_lba: int
    data_lba: int = 0
    nwa: int = 0
    track_start: int = 0
    flags: int = FLAG_BLANK

    @property
    def blank(self) -> bool:
        return bool(self.flags & FLAG_BLANK)

    @property
    def complete(self) -> bool:
        return bool(self.flags & FLAG_COMPLETE)

    @property
    def session_open(self) -> bool:
        return bool(self.flags & FLAG_SESSION_OPEN)

    @property
    def write_protected(self) -> bool:
        return bool(self.flags & FLAG_WRITE_PROTECTED)

    @property
    def formatted(self) -> bool:
        return bool(self.flags & FLAG_FORMATTED)

    def to_bytes(self) -> bytes:
        head = META_STRUCT.pack(
            META_MAGIC,
            META_VERSION,
            self.flags,
            self.capacity_lba,
            self.data_lba,
            self.nwa,
            self.track_start,
            b"\0" * 16,
            0,
        )
        crc = zlib.crc32(head[:-4]) & 0xFFFFFFFF
        return head[:-4] + struct.pack("<I", crc)

    @classmethod
    def from_bytes(cls, raw: bytes) -> "Meta":
        if len(raw) < META_STRUCT.size:
            raise ValueError("metadata too short")
        magic, version, flags, cap, data, nwa, start, reserved, crc = META_STRUCT.unpack_from(raw)
        if magic != META_MAGIC:
            raise ValueError("bad metadata magic")
        if version not in (1, META_VERSION):
            raise ValueError(f"unsupported metadata version {version}")
        calc = zlib.crc32(raw[:44]) & 0xFFFFFFFF
        if calc != crc:
            raise ValueError("metadata CRC mismatch")
        if not (0 < cap <= 4_000_000):
            raise ValueError("invalid capacity")
        return cls(cap, data, nwa, start, flags)


def sidesector_bytes(mb: int) -> int:
    return mb * 1024 * 1024


def meta_path(image: Path) -> Path:
    return image.with_suffix(image.suffix + ".meta")


def write_meta(image: Path, meta: Meta) -> None:
    path = meta_path(image)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(meta.to_bytes())
    os.replace(tmp, path)


def read_meta(image: Path) -> Meta | None:
    try:
        return Meta.from_bytes(meta_path(image).read_bytes())
    except (OSError, ValueError):
        return None


def preallocate(path: Path, size: int) -> None:
    """Allocate the full image so later random writes cannot run out of space."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        try:
            os.posix_fallocate(f.fileno(), 0, size)
        except (OSError, AttributeError):
            zero = b"\0" * (1024 * 1024)
            remaining = size
            while remaining:
                chunk = min(remaining, len(zero))
                f.write(zero[:chunk])
                remaining -= chunk
        f.truncate(size)


def create_blank(image: Path, mb: int, force: bool, zeros: bool = False) -> Meta:
    if image.exists() and not force:
        raise SystemExit(f"{image} exists; use --force to overwrite")
    sectors = mb * 1024 * 1024 // SECTOR
    if zeros:
        with open(image, "wb") as f:
            block = b"\0" * (1024 * 1024)
            for _ in range(mb):
                f.write(block)
    else:
        preallocate(image, sidesector_bytes(mb))
    meta = Meta(capacity_lba=sectors)
    write_meta(image, meta)
    return meta


def detect_iso_sectors(image: Path) -> int | None:
    try:
        with open(image, "rb") as f:
            f.seek(16 * SECTOR)
            pvd = f.read(96)
    except OSError:
        return None
    if len(pvd) >= 88 and pvd[1:6] == b"CD001":
        return struct.unpack_from("<I", pvd, 80)[0]
    return None


def import_iso(iso: Path, image: Path, mb: int, force: bool) -> Meta:
    iso_size = iso.stat().st_size
    if iso_size > mb * 1024 * 1024:
        raise SystemExit(f"ISO is {iso_size} bytes, larger than {mb} MiB capacity")
    sectors = mb * 1024 * 1024 // SECTOR
    used = (iso_size + SECTOR - 1) // SECTOR
    if image.exists() and not force:
        raise SystemExit(f"{image} exists; use --force to overwrite")
    preallocate(image, sidesector_bytes(mb))
    with open(iso, "rb") as src, open(image, "r+b") as dst:
        shutil.copyfileobj(src, dst, 1024 * 1024)
        # Clear the last partial sector.
        dst.seek(used * SECTOR)
    meta = Meta(
        capacity_lba=sectors,
        data_lba=used,
        nwa=used,
        flags=FLAG_COMPLETE,
    )
    write_meta(image, meta)
    return meta


def blank_image(image: Path) -> Meta:
    if not image.exists():
        raise SystemExit(f"{image} does not exist")
    meta = read_meta(image) or Meta(capacity_lba=image.stat().st_size // SECTOR or DEFAULT_MB * 512)
    meta.flags = FLAG_BLANK
    meta.data_lba = 0
    meta.nwa = 0
    meta.track_start = 0
    write_meta(image, meta)
    return meta


def print_info(image: Path) -> None:
    if not image.exists():
        raise SystemExit(f"{image} does not exist")
    size = image.stat().st_size
    meta = read_meta(image)
    fs_blocks = detect_iso_sectors(image)
    print(f"image          : {image}")
    print(f"size           : {size} bytes ({size / (1024*1024):.2f} MiB)")
    print(f"capacity       : {size // SECTOR} sectors ({size / (1024*1024):.1f} MiB)")
    print(f"metadata       : {meta_path(image)}")
    if meta:
        print(f"meta state     : {'blank' if meta.blank else ('complete' if meta.complete else 'incomplete')}")
        print(f"meta capacity  : {meta.capacity_lba} sectors")
        print(f"lead-out (LBA) : {meta.data_lba}")
        print(f"next writable  : {meta.nwa}")
        print(f"write protected: {meta.write_protected}")
    else:
        print("meta state     : missing/invalid (firmware will infer from content)")
    if fs_blocks is not None:
        print(f"ISO9660 PVD    : {fs_blocks} blocks")


def make_iso(folder: Path, output: Path, volume_id: str) -> None:
    if not folder.is_dir():
        raise SystemExit(f"{folder} is not a directory")
    if output.exists():
        raise SystemExit(f"{output} exists; choose another name")
    if shutil.which("xorriso"):
        cmd = [
            "xorriso", "-as", "mkisofs", "-iso-level", "3", "-J", "-joliet-long",
            "-r", "-V", volume_id, "-o", str(output), str(folder),
        ]
    elif shutil.which("genisoimage"):
        cmd = [
            "genisoimage", "-iso-level", "3", "-J", "-joliet-long", "-r",
            "-V", volume_id, "-o", str(output), str(folder),
        ]
    elif shutil.which("mkisofs"):
        cmd = [
            "mkisofs", "-iso-level", "3", "-J", "-joliet-long", "-r",
            "-V", volume_id, "-o", str(output), str(folder),
        ]
    else:
        raise SystemExit("install xorriso, genisoimage or mkisofs to create ISO files")
    subprocess.run(cmd, check=True)
    print(f"created {output} ({output.stat().st_size} bytes)")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="RP2350 CD-RW image tool")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("create", help="create a blank rewritable image")
    p.add_argument("image", type=Path)
    p.add_argument("--size-mb", type=int, default=DEFAULT_MB)
    p.add_argument("--force", action="store_true")
    p.add_argument("--zeros", action="store_true", help="write zeroes instead of fallocate")

    p = sub.add_parser("import", help="import an ISO into a rewritable image")
    p.add_argument("iso", type=Path)
    p.add_argument("image", type=Path)
    p.add_argument("--size-mb", type=int, default=DEFAULT_MB)
    p.add_argument("--force", action="store_true")

    p = sub.add_parser("blank", help="mark an image as blank")
    p.add_argument("image", type=Path)

    p = sub.add_parser("info", help="show image and sidecar state")
    p.add_argument("image", type=Path)

    p = sub.add_parser("mkiso", help="create an ISO9660/Joliet image from a folder")
    p.add_argument("folder", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--volume-id", default="CDRW_DATA")

    args = ap.parse_args(argv)
    if args.cmd == "create":
        meta = create_blank(args.image, args.size_mb, args.force, args.zeros)
        print(f"created blank {args.image}: {meta.capacity_lba} sectors")
    elif args.cmd == "import":
        if not args.iso.is_file():
            raise SystemExit(f"{args.iso} not found")
        meta = import_iso(args.iso, args.image, args.size_mb, args.force)
        print(f"imported {args.iso} -> {args.image} ({meta.data_lba} sectors)")
    elif args.cmd == "blank":
        meta = blank_image(args.image)
        print(f"marked {args.image} blank")
    elif args.cmd == "info":
        print_info(args.image)
    elif args.cmd == "mkiso":
        make_iso(args.folder, args.output, args.volume_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
