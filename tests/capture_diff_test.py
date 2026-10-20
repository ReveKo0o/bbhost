#!/usr/bin/env python3
"""Tests for tools/capture_diff.py on synthetic captures."""
import copy
import hashlib
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import capture_diff  # noqa: E402

PAGE = 16


def blob_entry(name, data):
    return {"file": f"blobs/{name}.bin", "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def write_capture(root, manifest, blobs):
    (root / "blobs").mkdir(parents=True)
    manifest = copy.deepcopy(manifest)
    manifest["blobs"] = {}
    for name, data in blobs.items():
        (root / "blobs" / f"{name}.bin").write_bytes(data)
        manifest["blobs"][name] = blob_entry(name, data)
    (root / "manifest.json").write_text(json.dumps(manifest))
    return root


def base_manifest():
    return {
        "schema": "bbhost-draw-capture",
        "version": 1,
        "identity": {"pipeline": "aaaa+bbbb", "flip": 1500, "draw_index": 10},
        "draw": {"vertex_count": 4, "instance_count": 1, "indexed": False},
        "stages": [{"stage": "vertex", "params": {"user_sgpr": ["0x1000", "0x0"], "cb_valid": "0x1"},
                    "buffers": [{"binding": 3, "guest_va": "0x2000", "blob": "vs-buffer0"}]}],
        "registers": {"ctx": ["0x1"]},
        "memory": {"page_bytes": PAGE, "pages": ["0x10000"], "blob": "memory"},
    }


def base_blobs():
    return {"vs-buffer0": b"\x01" * 8, "color0-live": b"\x02" * 8, "memory": b"\x00" * PAGE}


class CaptureDiffTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_diff(self, manifest_b, blobs_b):
        a = write_capture(self.root / "a", base_manifest(), base_blobs())
        b = write_capture(self.root / "b", manifest_b, blobs_b)
        out = io.StringIO()
        with redirect_stdout(out):
            status = capture_diff.main([str(a), str(b)])
        return status, out.getvalue()

    def test_identical(self):
        status, out = self.run_diff(base_manifest(), base_blobs())
        self.assertEqual(status, 0)
        self.assertIn("inputs: equal", out)
        self.assertIn("outputs: equal", out)

    def test_draw_index_and_addresses_do_not_fail(self):
        m = base_manifest()
        m["identity"]["draw_index"] = 11
        m["stages"][0]["params"]["user_sgpr"][0] = "0x3000"
        m["stages"][0]["buffers"][0]["guest_va"] = "0x4000"
        m["registers"]["ctx"][0] = "0x2"
        status, out = self.run_diff(m, base_blobs())
        self.assertEqual(status, 0)
        self.assertIn("addresses: 2 differ", out)
        self.assertIn("registers: 1 differ", out)

    def test_input_value_fails(self):
        m = base_manifest()
        m["draw"]["vertex_count"] = 6
        status, out = self.run_diff(m, base_blobs())
        self.assertEqual(status, 1)
        self.assertIn("draw.vertex_count: 4 -> 6", out)

    def test_input_blob_fails(self):
        blobs = base_blobs()
        blobs["vs-buffer0"] = b"\x01" * 7 + b"\x09"
        status, out = self.run_diff(base_manifest(), blobs)
        self.assertEqual(status, 1)
        self.assertIn("blob vs-buffer0: 1 of 8 bytes differ", out)

    def test_output_blob_fails(self):
        blobs = base_blobs()
        blobs["color0-live"] = b"\x03" * 8
        status, out = self.run_diff(base_manifest(), blobs)
        self.assertEqual(status, 1)
        self.assertIn("outputs: 1 differ", out)

    def test_memory_page_content(self):
        blobs = base_blobs()
        blobs["memory"] = b"\x00" * (PAGE - 1) + b"\x05"
        status, out = self.run_diff(base_manifest(), blobs)
        self.assertEqual(status, 1)
        self.assertIn("memory page 0x10000: 1 bytes differ", out)

    def test_corrupt_blob_is_refused(self):
        a = write_capture(self.root / "a", base_manifest(), base_blobs())
        b = write_capture(self.root / "b", base_manifest(), base_blobs())
        (b / "blobs" / "vs-buffer0.bin").write_bytes(b"\x07" * 8)
        manifest = json.loads((b / "manifest.json").read_text())
        manifest["blobs"]["vs-buffer0"]["sha256"] = "0" * 64
        (b / "manifest.json").write_text(json.dumps(manifest))
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            capture_diff.main([str(a), str(b)])
        self.assertEqual(raised.exception.code, 2)

    def test_missing_capture_is_refused(self):
        a = write_capture(self.root / "a", base_manifest(), base_blobs())
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            capture_diff.main([str(a), str(self.root / "missing")])
        self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
