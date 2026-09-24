#!/usr/bin/env python3
"""Fail-closed SQL-luatest source topology checks."""

import importlib.util
from pathlib import Path
import unittest


MODULE = Path(__file__).with_name("luatest_capture.py")
SPEC = importlib.util.spec_from_file_location("luatest_capture", MODULE)
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


class SourceTopologyTest(unittest.TestCase):
    def test_single_child_string_sql_is_supported(self):
        source = "g.server = server:new({})\ng.server:exec(function() " \
                 "box.execute('SELECT 1') end)"
        self.assertIsNone(capture.unsupported_luatest_source(source))

    def test_multiple_children_are_rejected(self):
        source = "a = server:new({})\nb = server:new({})"
        self.assertIn("one child", capture.unsupported_luatest_source(source))

    def test_restart_is_rejected(self):
        source = "a = server:new({})\na:restart()"
        self.assertIn("restarted", capture.unsupported_luatest_source(source))

    def test_net_box_sql_is_rejected(self):
        source = "a = server:new({})\nconn = a.net_box\nconn:execute('SELECT 1')"
        self.assertIn("net.box", capture.unsupported_luatest_source(source))

    def test_prepared_sql_is_rejected(self):
        source = "a = server:new({})\nid = box.prepare('SELECT 1').stmt_id\n" \
                 "box.execute(id)"
        self.assertIn("prepared", capture.unsupported_luatest_source(source))


if __name__ == "__main__":
    unittest.main()
