"""tools/pipeline_list.py on synthetic records: bbhost's hash, a manifest read
and written back byte for byte, merge keeping the newest of a key, and a list
naming its programs instead of holding them."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pipeline_list as pl  # noqa: E402

ORIGIN = {"translator": "f" * 32, "vendor": 0x10DE, "device": 0x2786, "driver": 1, "uuid": "00" * 16, "libraries": True}


def recipe(key, last_run, vs, ps, vs_name="0000aaaa", ps_name="0000bbbb", vertex_input=True):
    """One record laid out as render.cpp's recipe_file::write lays it out."""
    flags = (pl.HAS_VERTEX_INPUT if vertex_input else 0) | pl.RELINK
    state = bytes(19 * 4 + 8 + 57 * 4 + pl.GX_BLEND + pl.GX_DEPTH + pl.GX_RASTER)
    tail = struct.pack("<I", 0) * 4 + struct.pack("<I", 1) + struct.pack("<4I", 0, 1, 4, 0x8C) + struct.pack("<I", 0) * 2 + \
        struct.pack("<Q", 0x1234) + bytes(128 + 128 + 4)
    tail += struct.pack("<I", 0) + bytes(9 * 4 + 16 + 8 + 48 + 8)  # no tessellation
    body = b"".join([struct.pack("<IQII", flags, key, 1, last_run), state, struct.pack("<I", len(vs) // 4), vs,
                     struct.pack("<I", 0), struct.pack("<I", len(ps) // 4), ps, pl.wstr(vs_name), pl.wstr(ps_name), tail])
    return pl.Recipe().parse(body)


class PipelineListTest(unittest.TestCase):
    def test_hash_is_bbhosts(self):
        # Values from host/gpu.cpp's fnv1a over the same words.
        w = struct.pack("<5I", 0xBEEFCAFE, 1, 2, 3, 0xFFFFFFFF)
        self.assertEqual(pl.fnv1a(w[:0]), 1469598103934665603)
        self.assertEqual(pl.fnv1a(w[:8]), 1910414784848688967)
        self.assertEqual(pl.fnv1a(w), 5040822000519442178)

    def test_round_trip_and_merge(self):
        vs, ps = struct.pack("<3I", 1, 2, 3), struct.pack("<2I", 7, 8)
        with tempfile.TemporaryDirectory() as d:
            a, b, out = Path(d) / "a.bin", Path(d) / "b.bin", Path(d) / "m.bin"
            pl.write_file(a, 3, ORIGIN, [recipe(1, 3, vs, ps), recipe(2, 1, vs, ps)])
            run, origin, recs = pl.read_file(a)
            self.assertEqual((run, origin["device"], [r.key for r in recs]), (3, 0x2786, [1, 2]))
            pl.write_file(b, 0, origin, recs)
            self.assertEqual(a.read_bytes()[12:], b.read_bytes()[12:])  # all but the run, as it was
            pl.write_file(b, 5, ORIGIN, [recipe(2, 5, vs, struct.pack("<I", 9)), recipe(3, 5, vs, ps)])
            sys.argv = ["pipeline_list.py", "merge", "--out", str(out), str(a), str(b)]
            self.assertEqual(pl.main(), 0)
            run, _, recs = pl.read_file(out)
            by = {r.key: r for r in recs}
            self.assertEqual((run, sorted(by)), (5, [1, 2, 3]))
            self.assertEqual(by[2].ps, struct.pack("<I", 9))  # the newer start's record of key 2

    def test_list_names_programs(self):
        vs, ps = struct.pack("<3I", 1, 2, 3), struct.pack("<2I", 7, 8)
        rc = recipe(1, 1, vs, ps)
        rc.flags |= pl.VS_REF | pl.PS_REF
        rc.vs_ref, rc.ps_ref = (3, pl.fnv1a(vs)), (2, pl.fnv1a(ps))
        rc.vs = rc.ps = None
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "list.bin"
            pl.write_file(path, 0, ORIGIN, [rc])
            _, _, recs = pl.read_file(path)
            self.assertEqual((recs[0].vs, recs[0].ps), (None, None))
            self.assertEqual(recs[0].vs_ref, (3, pl.fnv1a(vs)))
            self.assertEqual((recs[0].vs_name, recs[0].ps_name), ("0000aaaa", "0000bbbb"))


    def test_tessellated_record(self):
        vs, ps, ls, hs = (struct.pack("<2I", 1, 2), struct.pack("<I", 3), struct.pack("<3I", 4, 5, 6), struct.pack("<I", 7))
        rc = recipe(9, 1, vs, ps)
        # A tessellated one: the LS and the hull after the tessellation state.
        tess = struct.pack("<I", 1 | pl.TESS_HAS_LS | pl.TESS_HAS_HS) + bytes(9 * 4 + 16 + 8 + 48 + 8)
        body = rc.body()[:-len(rc.tess)] + tess + struct.pack("<I", 3) + ls + pl.wstr("0000cccc") + struct.pack("<I", 2) + \
            struct.pack("<I", 1) + hs + pl.wstr("0000dddd") + struct.pack("<I", 0xFFFFFFFF)
        rc = pl.Recipe().parse(body)
        self.assertEqual((rc.ls, rc.hs, rc.ls_name, rc.hs_name, rc.ls_sgpr), (ls, hs, "0000cccc", "0000dddd", 2))
        self.assertEqual(rc.body(), body)
        rc.flags |= pl.LS_REF
        rc.ls_ref, rc.ls = (3, pl.fnv1a(ls)), None
        again = pl.Recipe().parse(rc.body())
        self.assertEqual((again.ls, again.ls_ref, again.hs), (None, (3, pl.fnv1a(ls)), hs))


if __name__ == "__main__":
    unittest.main()
