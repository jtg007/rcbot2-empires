"""Offline checks for the dev runner's log-driven control flow."""
import argparse
import contextlib
import io
import os
import re
import threading
import unittest
from unittest.mock import Mock, patch

import dev_run


def server_with_lines(*lines):
    server = dev_run.Server.__new__(dev_run.Server)
    server.lines = list(lines)
    server.lock = threading.Lock()
    server.alive = Mock(return_value=True)
    server.send = Mock()
    return server


class ScenarioTests(unittest.TestCase):
    def run_commands(self, server, commands, until=None, fail=None):
        args = argparse.Namespace(bots=1, minutes=0)
        with (
            patch.dict(os.environ, {"DEV_CMDS": commands}),
            patch.multiple(dev_run, UNTIL=until, FAIL=fail, MANUAL=False, TIMESCALE=1),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            return dev_run.run_scenario(server, args, None, 0, 0)

    def test_wait_searches_only_new_lines_after_the_first_scan(self):
        server = server_with_lines("server boot", "plugin ready")
        with (
            patch.object(server, "scan", wraps=server.scan) as scan,
            patch.object(dev_run.time, "sleep", side_effect=lambda _: server.lines.append("done")),
        ):
            match = server.wait_for("done", 1)
        self.assertEqual(match.group(), "done")
        self.assertEqual([call.args[1] for call in scan.call_args_list], [0, 2])

    def test_mark_excludes_previous_success(self):
        server = server_with_lines("done")
        mark = self.run_commands(server, "mark", until="done")
        self.assertIsNone(server.scan(re.compile("done"), mark)[0])

    def test_missing_prerequisite_stops_before_next_command(self):
        server = server_with_lines()
        with self.assertRaisesRegex(dev_run.ScenarioFailed, "missing prerequisite: factory built"):
            self.run_commands(server, "waitfor 0 factory built;add buyer")
        server.send.assert_not_called()

    def test_waitfor_keeps_trailing_blank(self):
        server = server_with_lines("classname=emp_building_vehiclefactory_panel")
        with self.assertRaisesRegex(dev_run.ScenarioFailed, "missing prerequisite"):
            self.run_commands(server, " waitfor 0 classname=emp_building_vehiclefactory ; mark ")
        server.lines.append("classname=emp_building_vehiclefactory ")
        self.assertEqual(self.run_commands(server, "waitfor 0 classname=emp_building_vehiclefactory ; mark "), 2)

    def test_terminal_failure_interrupts_a_scripted_delay(self):
        server = server_with_lines()
        with patch.object(dev_run.time, "sleep", side_effect=lambda _: server.lines.append("exit timeout")):
            with self.assertRaisesRegex(dev_run.ScenarioFailed, "exit timeout"):
                self.run_commands(server, "wait 60;next command", fail="exit timeout")
        server.send.assert_not_called()

    def test_exit_status_distinguishes_gameplay_failure_from_crash(self):
        for crash, failure, status in ((None, None, 0), (None, "missing expected event", 1), ("server died", None, 1)):
            with self.subTest(crash=crash, failure=failure):
                output = io.StringIO()
                with (
                    patch("sys.argv", ["dev_run.py"]),
                    patch.object(dev_run, "run_match", return_value=(None, crash, [], [], failure)),
                    contextlib.redirect_stdout(output),
                ):
                    self.assertEqual(dev_run.main(), status)
                self.assertIn("Crashes: " + ("YES" if crash else "none"), output.getvalue())
                self.assertIn("Run result: " + ("FAIL" if status else "OK"), output.getvalue())


if __name__ == "__main__":
    unittest.main()
