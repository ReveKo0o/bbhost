"""Checks that incomplete/debug evidence cannot masquerade as a timing window."""

import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location("world_baseline", Path(__file__).resolve().parents[1] / "tools/world_baseline.py")
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


class BaselineTest(unittest.TestCase):
    def window(self, summary, end, seconds=16.0, profile=True, complete=True):
        summary.feed(f"[bbhost] sceGnmSubmitAndFlip #{end} handle=1 buf=0 arg={end} cp-backlog=1 | "
                     f"{seconds} s: draws=425000 dispatches=47000 submits=1200 gpu-wait=7000 ms pipelines=3 transfers=189000/15000")
        summary.feed("[bbhost]   cp-thread ms: draw=5700 dispatch=650 prefetch=1250 bind=4100 label-wait=0")
        if profile:
            summary.feed("[bbhost]   gpu profile: total=14600ms cmdbuf=15500ms(1200) "
                         "graphics=14000ms compute=600ms 8747a367+a22c7f71=1600ms(13500)")
        if complete:
            summary.feed("[bbhost]   fill: pending=20000 applied=9000")

    def test_complete_windows_and_pipeline_identity(self):
        summary = baseline.LogSummary()
        for end in (1200, 1500, 1800, 2100, 2400):
            self.window(summary, end)
        result = summary.result(1500, 2400)
        self.assertTrue(result["complete_windows"])
        self.assertEqual(len(result["windows"]), 3)
        window = result["windows"][0]
        self.assertEqual(window["gpu"]["8747a367+a22c7f71"], {"ms": 1600, "count": 13500})
        self.assertEqual(window["gpu"]["compute"]["ms"], 600)
        self.assertEqual(window["counters"]["gpu-wait"], 7000)
        self.assertEqual(window["submit_fps"], 18.75)
        self.assertNotIn("p95", window)

    def test_gnm_ops_per_window(self):
        summary = baseline.LogSummary()
        self.window(summary, 1200)
        self.window(summary, 1500)
        summary.feed("[bbhost]   gnm ops: total=900 native-draw-tokens=40 set-context-reg=500 nop=60 op-58=12")
        window = summary.windows[-1]
        self.assertEqual(window["gnm_ops"], {"total": 900, "native-draw-tokens": 40, "set-context-reg": 500, "nop": 60, "op-58": 12})
        self.assertNotIn("gnm_ops", summary.windows[0])

    def test_launch_on_one_numa_node(self):
        argv = ["build/bbhost", "--config", "bbhost.toml"]
        self.assertEqual(baseline.launch_argv(argv, None), argv)
        self.assertEqual(baseline.launch_argv(argv, 0),
                         ["numactl", "--cpunodebind=0", "--membind=0", "build/bbhost", "--config", "bbhost.toml"])

    def test_missing_boundary_is_not_assumed_to_be_300(self):
        summary = baseline.LogSummary()
        for end in (1500, 2100, 2400):
            self.window(summary, end)
        self.assertFalse(summary.result(1500, 2400)["complete_windows"])

    def test_no_profile_or_truncated_window_is_incomplete(self):
        for profile, complete in ((False, True), (True, False)):
            with self.subTest(profile=profile, complete=complete):
                summary = baseline.LogSummary()
                self.window(summary, 1500)
                self.window(summary, 1800, profile=profile, complete=complete)
                self.assertFalse(summary.result(1500, 1800)["complete_windows"])

    def test_failure_retained_even_outside_selected_window(self):
        summary = baseline.LogSummary()
        summary.feed("[bbhost] render: descriptor set allocation failed")
        self.window(summary, 1500)
        self.window(summary, 1800)
        summary.feed("[bbhost] SIGSEGV at 0x2ab0a98")
        result = summary.result(1500, 1800)
        self.assertTrue(result["complete_windows"])
        self.assertEqual(len(result["failures"]), 2)

    def test_real_wait_failure_but_not_zero_failure_summary(self):
        summary = baseline.LogSummary()
        summary.feed("[bbhost] hang: 0 address(es) had a wait give up")
        self.assertEqual(summary.failures, [])
        summary.feed("[bbhost]   wait 0x123 gave up 3 time(s) (fn=3 ref=0x1)")
        summary.feed("[bbhost] startup: skip intro refused: unexpected logo bytes at 0x4d99138")
        self.assertEqual(len(summary.failures), 2)

    def test_capture_written_rejected_and_abandoned(self):
        summary = baseline.LogSummary()
        summary.feed("[bbhost] capture: 8747a367+a22c7f71 draw 41 (flip 1500) not captured: indirect draw: its arguments are GPU data")
        summary.feed("[bbhost] capture: wrote /tmp/x/captures/8747a367+a22c7f71-f1500-d42/manifest.json "
                     "(8747a367+a22c7f71, flip 1500, draw 42, 31 blobs, 104857600 bytes)")
        self.assertEqual(summary.captures, ["/tmp/x/captures/8747a367+a22c7f71-f1500-d42"])
        self.assertEqual(summary.failures, [])
        summary.feed("[bbhost] capture: 8747a367+a22c7f71 draw 43 abandoned before it was written")
        summary.feed("[bbhost] capture: /tmp/x/captures/y: writing blobs failed; no manifest written")
        self.assertEqual(len(summary.failures), 2)
        self.assertEqual(len(summary.result(1500, 1800)["captures"]), 1)

    def test_inventory_requires_selected_draw_not_a_binding_log(self):
        summary = baseline.LogSummary()
        summary.feed("[bbhost] render: first bind of pipeline 8747a367+a22c7f71")
        self.assertFalse(summary.pilot_inventory)
        summary.feed("[bbhost] render: detail for 8747a367+a22c7f71: index_va=0x0 type=0 count=4")
        self.assertTrue(summary.pilot_inventory)


if __name__ == "__main__":
    unittest.main()
