"""Scenario checkpoints: 'checkpoint NAME' saves the machine, 'start-at NAME' resumes there."""
import os
import tempfile
import unittest
from unittest import mock

from agk import runner, scenario


class CheckpointTests(unittest.TestCase):
    def test_checkpoint_saves_at_a_video_frame_boundary(self):
        sc = scenario.parse('wait-serial "go"\npress fire 2\ncheckpoint boss\nwait 5\n')
        i = sc.checkpoints["boss"]
        self.assertEqual(sc.lines[i - 3:i], ["wait 1 frame", "agk serial {ckpt:boss}.serial.txt",
                                             "agk snapsave {ckpt:boss}"])
        self.assertEqual(len(sc.lines), len(sc.origins))

    def test_errors(self):
        for text, msg in [("hold fire\ncheckpoint x", "release fire"),
                          ("wait 1\nstart-at x", "first command"),
                          ("checkpoint x\ncheckpoint x", "used twice"),
                          ("start-at", "usage")]:
            with self.assertRaisesRegex(scenario.ScenarioError, msg):
                scenario.parse(text)
        # checkpoint names don't clash with screenshot names (different files)
        scenario.parse("screenshot boss\ncheckpoint boss")

    def test_start_at(self):
        sc = scenario.parse("# fight\nstart-at boss\nwait 10\n")
        self.assertEqual(sc.start_at, ("boss", 2))

    def test_the_defining_run_and_start_at_agree_on_the_file(self):
        # the path depends on the lines before the checkpoint only: the scenario
        # that saves it and the one that starts from it must compute the same one
        text = 'wait-serial "go"\ncheckpoint boss\nscreenshot after\n'
        calls = []
        with mock.patch.object(runner, "checkpoint_path",
                               lambda adf, p, b, n, prefix: calls.append(tuple(prefix)) or f"/c/{n}"):
            sc = scenario.parse(text)
            lines = runner._checkpoint_lines(sc, "g.adf", "a500", "AGK ready")
        self.assertIn("agk snapsave /c/boss", lines)
        self.assertEqual(calls[0], tuple(sc.lines[:sc.checkpoints["boss"] - 3]))

    def test_find_checkpoint(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "tests"))
            with open(os.path.join(d, "tests", "demo.agk"), "w") as f:
                f.write('wait-serial "go"\ncheckpoint boss   # here\n')
            sc, path = runner.find_checkpoint(d, "boss")
            self.assertIn("boss", sc.checkpoints)
            with self.assertRaisesRegex(runner.RunError, "no scenario in tests/ or demo/ has 'checkpoint nope'"):
                runner.find_checkpoint(d, "nope")


if __name__ == "__main__":
    unittest.main()
