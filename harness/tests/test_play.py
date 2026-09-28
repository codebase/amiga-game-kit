"""Launching a second game must pass its config to a fresh FS-UAE process."""
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from agk.cli import cmd_play
from agk.profiles import PROFILES

class PlayTests(unittest.TestCase):
    def test_mac_launches_requested_a1200_in_new_instance(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d,'build').mkdir()
            Path(d,'agk.toml').write_text('name="test"\nprofile="a1200"\n')
            args=SimpleNamespace(project=d,profile=None)
            with patch('agk.cli.need_adf'), patch('agk.cli.profiles.resolve',return_value=(PROFILES['a1200'],'owned.rom',None)), patch('agk.cli.sys.platform','darwin'), patch('agk.cli.os.path.exists',return_value=True), patch('agk.cli.subprocess.run') as run:
                run.return_value.returncode=0
                self.assertEqual(cmd_play(args),0)
                cfg=str(Path(d,'build/test.fs-uae'))
                run.assert_called_once_with(['open','-n','-a','FS-UAE','--args',cfg])
                self.assertIn('amiga_model = A1200',Path(cfg).read_text())
