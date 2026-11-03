import importlib.util
import json
import os
import unittest

from stbench.armconfig import ARMS_DIR, load_arm

_SPEC = importlib.util.spec_from_file_location("intellij_mcp", os.path.join(ARMS_DIR, "intellij-mcp.py"))
intellij_mcp = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(intellij_mcp)

WHITELIST = ["analyze_calls", "search_symbol"]
REFUSED = {"result": {"content": [{"type": "text", "text": "Tool 'get_run_configurations' not found"}], "isError": True}}


class IsolationTest(unittest.TestCase):
    def test_the_whitelist_plus_execute_tool_and_a_refused_escape_pass(self):
        self.assertEqual(intellij_mcp.isolation_problems(WHITELIST + ["execute_tool"], WHITELIST, REFUSED), [])

    def test_a_missing_whitelisted_tool_fails(self):
        problems = intellij_mcp.isolation_problems(["analyze_calls", "execute_tool"], WHITELIST, REFUSED)
        self.assertIn("does not offer search_symbol", problems[0])

    def test_a_tool_outside_the_whitelist_fails(self):
        problems = intellij_mcp.isolation_problems(WHITELIST + ["execute_tool", "execute_terminal_command"],
                                                   WHITELIST, REFUSED)
        self.assertIn("execute_terminal_command", problems[0])

    def test_execute_tool_running_a_tool_outside_the_whitelist_fails(self):
        ran = {"result": {"content": [{"type": "text", "text": "[{\"name\": \"Application\"}]"}]}}
        problems = intellij_mcp.isolation_problems(WHITELIST + ["execute_tool"], WHITELIST, ran)
        self.assertIn("execute_tool", problems[0])

    def test_an_rpc_error_counts_as_refused(self):
        error = {"error": {"code": -32602, "message": "Tool 'get_run_configurations' not found"}}
        self.assertEqual(intellij_mcp.isolation_problems(WHITELIST + ["execute_tool"], WHITELIST, error), [])


class IntellijArmTest(unittest.TestCase):
    def test_allowed_tools_name_each_whitelisted_mcp_tool_not_the_whole_server(self):
        spec = load_arm("intellij")
        whitelist = spec.vars["BENCH_INTELLIJ_TOOLS"].split(",")
        mcp = [tool for tool in spec.allowed_tools if tool.startswith("mcp__")]
        self.assertEqual(sorted(mcp), sorted(f"mcp__intellij__{tool}" for tool in whitelist + ["execute_tool"]))
        self.assertNotIn("mcp__intellij", spec.allowed_tools)


if __name__ == "__main__":
    unittest.main()
