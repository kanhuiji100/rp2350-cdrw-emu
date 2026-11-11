#!/usr/bin/env python3
"""Tests for tools/cdrw.py (metadata compatibility and image workflows)."""
from __future__ import annotations

import hashlib
import importlib.util
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("cdrw", ROOT / "tools" / "cdrw.py")
cdrw = importlib.util.module_from_spec(spec)
assert spec.loader is not None
import sys
sys.modules[spec.name] = cdrw
spec.loader.exec_module(cdrw)


class CdrwToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_blank_metadata_roundtrip(self) -> None:
        img = self.dir / "blank.cdr"
        meta = cdrw.create_blank(img, 8, force=False)
        self.assertEqual(meta.capacity_lba, 4096)
        self.assertTrue(meta.blank)
        self.assertEqual(img.stat().st_size, 8 * 1024 * 1024)
        raw = (self.dir / "blank.cdr.meta").read_bytes()
        self.assertEqual(len(raw), 48)
        self.assertEqual(raw[:8], b"RPCDRW01")
        self.assertEqual(struct_u32(raw, 44), zlib.crc32(raw[:44]) & 0xFFFFFFFF)
        again = cdrw.read_meta(img)
        self.assertIsNotNone(again)
        self.assertEqual(again.capacity_lba, 4096)
        self.assertTrue(again.blank)

    def test_import_and_blank(self) -> None:
        iso = self.dir / "tiny.iso"
        # A minimal 3-sector file is enough to test the raw mapping.
        iso.write_bytes(b"A" * (3 * 2048))
        img = self.dir / "disc.cdr"
        meta = cdrw.import_iso(iso, img, 8, force=False)
        self.assertFalse(meta.blank)
        self.assertTrue(meta.complete)
        self.assertEqual(meta.data_lba, 3)
        with open(img, "rb") as f:
            self.assertEqual(f.read(3 * 2048), iso.read_bytes())
        cdrw.blank_image(img)
        self.assertTrue(cdrw.read_meta(img).blank)

    def test_corrupt_meta_rejected(self) -> None:
        img = self.dir / "bad.cdr"
        cdrw.create_blank(img, 8, force=False)
        meta_path = self.dir / "bad.cdr.meta"
        raw = bytearray(meta_path.read_bytes())
        raw[20] ^= 0xFF
        meta_path.write_bytes(raw)
        self.assertIsNone(cdrw.read_meta(img))

    def test_mkiso_if_available(self) -> None:
        folder = self.dir / "folder"
        folder.mkdir()
        (folder / "hello.txt").write_text("hello\n")
        if not any(__import__("shutil").which(x) for x in ("xorriso", "genisoimage", "mkisofs")):
            self.skipTest("no ISO authoring tool installed")
        out = self.dir / "out.iso"
        cdrw.make_iso(folder, out, "TESTVOL")
        self.assertGreater(out.stat().st_size, 0)
        data = out.read_bytes()
        self.assertEqual(data[16 * 2048 + 1:16 * 2048 + 6], b"CD001")


def struct_u32(raw: bytes, off: int) -> int:
    import struct
    return struct.unpack_from("<I", raw, off)[0]


if __name__ == "__main__":
    unittest.main(verbosity=2)
