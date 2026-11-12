"""The test runner's own reading of a run (tools/dev/scene.py), on made-up logs: no game needed."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.dev import scene

HEADER = ("frame,t_s,interval_ms,work_ms,present_ms,wait_ms,draws,cpu_pct,rss_mb,gpu_busy_ms,gpu_span_ms,"
          "submit_ms,submit_other_ms,sync_ms\n")


def perf_log(folder: Path, frames: int, slow: dict[int, float], events: list[dict]) -> Path:
    """A run of `frames` frames at 20 ms each: a loading screen (20 draws) for the first 200, then the 3D world
    (300 draws). `slow`: frame -> its time in ms."""
    rows, t = [], 0.0
    for f in range(1, frames + 1):
        ms = slow.get(f, 20.0)
        t += ms / 1000
        rows.append(f"{f},{t:.3f},{ms:.2f},{ms - 2:.2f},0.00,2.00,{20 if f <= 200 else 300},100,3000,12.00,12.50,0.00,0.00,0.00")
    csv = folder / "perf_test.csv"
    csv.write_text(HEADER + "\n".join(rows) + "\n")
    # the last line cut short, as when the game is stopped while writing
    (folder / "perf_test.jsonl").write_text("\n".join(json.dumps(e) for e in events) + '\n{"kind":"sta')
    return csv


class SceneTest(unittest.TestCase):
    def test_scheduled_frames(self):
        env = {"YW4_SCREENSHOTS": "1700, 1800", "YW4_SET_AT": "3000:YW4_ASPECT=32:9,3300:YW4_UPSCALE=1",
               "YW4_FLASH_SAVE": "500-520", "YW4_AB_PASS": "material", "YW4_FPS": "120"}
        self.assertEqual(sorted(scene.scheduled_frames(env)),
                         [("YW4_FLASH_SAVE", 500), ("YW4_SCREENSHOTS", 1700), ("YW4_SCREENSHOTS", 1800),
                          ("YW4_SET_AT", 3000), ("YW4_SET_AT", 3300)])

    def test_known_variables(self):
        known = scene.known_variables()
        self.assertIn("YW4_SCREENSHOTS", known)
        self.assertIn("YW4_EXIT_AT", known)
        self.assertNotIn("YW4_SHADOW", known)  # the typo the runner must refuse (YW4_SHADOWS)

    def test_play_starts_after_loading_and_spikes_are_counted(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            # a slow frame while loading (not counted) and two in play
            csv = perf_log(t, 3000, {100: 400.0, 1500: 55.0, 2500: 90.0}, [{"kind": "end", "frame": 3000}])
            r = scene.analyse(csv, csv.with_suffix(".jsonl"))
            self.assertEqual(r["last_frame"], 3000)
            self.assertEqual(r["events"], [{"kind": "end", "frame": 3000}])  # the cut line is dropped
            p = r["play"]
            self.assertGreater(p["from_frame"], 200 + 8 * 50 - 30)  # 8 s after the world's first frame
            self.assertEqual(p["spikes"], 2)
            self.assertEqual([w["frame"] for w in p["worst"]], [2500, 1500])
            self.assertAlmostEqual(p["p50_ms"], 20.0)
            self.assertEqual(p["draws"], 300)

    def test_checks(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "frame_001700.bmp").write_bytes(b"")
            result = {"last_frame": 1750, "returncode": 0, "timed_out": False, "exit_at": 1830, "events": [
                {"kind": "ab", "frame": 1700, "pass": "material", "target": 0, "differ": 0, "texels": 100},
                {"kind": "ab", "frame": 1700, "pass": "material", "target": 1, "differ": 7, "texels": 100}]}
            bad = scene.check(result, {"YW4_SCREENSHOTS": "1700,1800"}, t, [1700, 1800])
            self.assertEqual(len(bad), 3, bad)
            self.assertTrue(any("never reached: YW4_SCREENSHOTS frame 1800" in b for b in bad))
            self.assertTrue(any("no screenshot of frame 1800" in b for b in bad))
            self.assertTrue(any("7 of 100 texels differ" in b for b in bad))
            ok = {"last_frame": 1830, "returncode": 0, "timed_out": False, "exit_at": 1830, "events": []}
            self.assertEqual(scene.check(ok, {"YW4_SCREENSHOTS": "1700"}, t, [1700]), [])
            stopped = dict(ok, last_frame=900, returncode=None, timed_out=True,
                           events=[{"kind": "fatal", "frame": 0, "text": "no translated shader"}])
            bad = scene.check(stopped, {}, t, [])
            self.assertTrue(any("the game stopped: no translated shader" in b for b in bad))
            self.assertTrue(any("time limit" in b for b in bad))


if __name__ == "__main__":
    unittest.main()
