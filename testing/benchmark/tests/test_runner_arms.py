import json
import os
import shutil
import stat
import sys
import tempfile
import textwrap
import unittest

from stbench.armconfig import load_arm
from stbench.runner import (RunConfig, WarmUpFailed, _arm_order, config_markers, read_records, read_warm_up,
                            run_benchmark, work_problems)

FAKE_CLAUDE = textwrap.dedent("""\
    #!{python}
    # Fake `claude -p`: records its arguments and surroundings, uses the arm's tool when it has one.
    import json, os, shutil, subprocess, sys
    args = sys.argv[1:]
    tools = args[args.index("--tools") + 1].split(",")
    mcp = args[args.index("--mcp-config") + 1] if "--mcp-config" in args else None
    st = subprocess.run("st status", shell=True, capture_output=True, text=True)
    seen = {{"args": args, "cwd": os.getcwd(), "st_exit": st.returncode, "mytool": shutil.which("mytool"),
             "arm_env": os.environ.get("ARM_ENV"), "mcp": json.load(open(mcp)) if mcp else None,
             "index_visible": any(n.endswith(".srctrlprj") for n in os.listdir("."))}}
    with open(os.environ["FAKE_CLAUDE_SEEN"], "a") as handle:
        handle.write(json.dumps(seen) + "\\n")
    use = [{{"type": "tool_use", "id": "t0", "name": "Bash", "input": {{"command": "grep -rn x ."}}}}]
    if "LSP" in tools:
        use.append({{"type": "tool_use", "id": "t1", "name": "LSP", "input": {{"operation": "incomingCalls"}}}})
    for line in [
        {{"type": "system", "subtype": "init", "model": "claude-fake", "claude_code_version": "9.9.9"}},
        {{"type": "assistant", "message": {{"content": use}}}},
        {{"type": "result", "subtype": "success", "is_error": False, "num_turns": 2, "duration_ms": 1000,
          "total_cost_usd": 0.01, "result": "```answer\\nA.x\\n```", "usage": {{"input_tokens": 10}}}},
    ]:
        print(json.dumps(line))
""")

QUESTIONS = [
    {"id": "Q1", "kind": "structural", "prompt": "Who calls A.x?", "expected": {"required": ["A.x"]}},
    {"id": "Q2", "kind": "text", "prompt": "Where is key y?", "expected": {"required": ["A.x"]}},
]


def executable(path, text):
    with open(path, "w") as handle:
        handle.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class ConfiguredArmTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.bin = os.path.join(self.root, "bin")
        os.makedirs(self.bin)
        executable(os.path.join(self.bin, "claude"), FAKE_CLAUDE.format(python=sys.executable))
        executable(os.path.join(self.bin, "st"), "#!/bin/sh\nexit 0\n")
        self.project = os.path.join(self.root, "project")
        os.makedirs(os.path.join(self.project, "src"))
        for name in ["src/A.java", "demo.srctrlprj"]:
            with open(os.path.join(self.project, name), "w") as handle:
                handle.write("x")
        self.arms = os.path.join(self.root, "arms")
        os.makedirs(os.path.join(self.arms, "tool-bin"))
        executable(os.path.join(self.arms, "tool-bin", "mytool"), "#!/bin/sh\nexit 0\n")
        with open(os.path.join(self.arms, "tool-note.md"), "w") as handle:
            handle.write("NOTE: mytool exists")
        self.marks = os.path.join(self.root, "marks")
        self.seen = os.path.join(self.root, "seen.jsonl")
        self.out = os.path.join(self.root, "out")

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def spec(self, **fields):
        data = {"name": "tool", "workdir": "copy", "st": "hidden", "path_prepend": ["{arms}/tool-bin"],
                "env": {"ARM_ENV": "{workdir}"}, "tools": ["Bash", "Read", "Grep", "Glob", "LSP"],
                "mcp_servers": {"tool": {"type": "sse", "url": "http://127.0.0.1:{PORT}/sse"}},
                "vars": {"PORT": "64999"}, "note": "tool-note.md", "adoption": {"tools": ["LSP"]}}
        data.update(fields)
        path = os.path.join(self.arms, "tool.json")
        with open(path, "w") as handle:
            json.dump(data, handle)
        return load_arm(path)

    def config(self, *specs):
        return RunConfig(
            project=self.project, questions=QUESTIONS, arms=[spec.name for spec in specs], repeats=1,
            out_dir=self.out, st_binary="", note="", max_turns=5, timeout_s=60,
            work_dir=os.path.join(self.root, "work"), specs={spec.name: spec for spec in specs},
            notes={"tool": "NOTE: mytool exists"},
            env={"PATH": self.bin + os.pathsep + os.environ["PATH"], "FAKE_CLAUDE_SEEN": self.seen},
        )

    def seen_runs(self):
        with open(self.seen) as handle:
            return [json.loads(line) for line in handle]

    def test_the_agent_gets_the_arms_tools_path_env_mcp_and_note_on_its_own_copy(self):
        run_benchmark(self.config(self.spec()), progress=lambda _: None)
        for seen in self.seen_runs():
            args = seen["args"]
            self.assertEqual(args[args.index("--tools") + 1], "Bash,Read,Grep,Glob,LSP")
            self.assertEqual(args[args.index("--allowedTools") + 1], "Bash,Read,Grep,Glob,LSP")
            self.assertNotIn("--dangerously-skip-permissions", args)
            self.assertIn("--strict-mcp-config", args)
            self.assertEqual(seen["mcp"], {"mcpServers": {"tool": {"type": "sse",
                                                                    "url": "http://127.0.0.1:64999/sse"}}})
            self.assertEqual(args[args.index("--append-system-prompt") + 1], "NOTE: mytool exists")
            self.assertEqual(seen["mytool"], os.path.join(self.arms, "tool-bin", "mytool"))
            self.assertEqual(seen["st_exit"], 127)
            self.assertFalse(seen["index_visible"])
            self.assertEqual(seen["arm_env"], seen["cwd"])
            self.assertTrue(os.path.realpath(seen["cwd"]).startswith(os.path.realpath(os.path.join(self.root, "work"))))

    def test_two_copy_arms_get_separate_copies(self):
        other = load_arm(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "arms", "grep.json"))
        run_benchmark(self.config(self.spec(), other), progress=lambda _: None)
        cwds = {tuple(seen["args"][seen["args"].index("--tools") + 1].split(",")): seen["cwd"]
                for seen in self.seen_runs()}
        self.assertEqual(len(set(cwds.values())), 2)

    def test_records_count_the_arms_own_tool_and_every_tool_name(self):
        run_benchmark(self.config(self.spec()), progress=lambda _: None)
        record = read_records(self.out)[0]
        self.assertEqual(record["arm_tool_calls"], 1)
        self.assertTrue(record["used_arm_tool"])
        self.assertEqual(record["tool_names"], {"Bash": 1, "LSP": 1})
        self.assertEqual(record["model"], "claude-fake")

    def test_warm_up_and_startup_are_timed_apart_and_shut_down_always_runs(self):
        spec = self.spec(warm_up=f"echo warm >> {self.marks}; echo '{{\"ready_s\": 1.5}}'",
                         startup=f"echo start >> {self.marks}", startup_repeats=2,
                         shut_down=f"echo stop >> {self.marks}")
        run_benchmark(self.config(spec), progress=lambda _: None)
        with open(self.marks) as handle:
            self.assertEqual(handle.read().split(), ["warm", "start", "start", "stop"])
        entries = read_warm_up(self.out)
        self.assertEqual([e["phase"] for e in entries], ["warm_up", "startup", "startup", "shut_down"])
        self.assertEqual(entries[0]["details"], {"ready_s": 1.5})
        self.assertTrue(all(e["exit"] == 0 and e["seconds"] >= 0 and e["arm"] == "tool" for e in entries))

    def test_a_commands_own_measure_is_read_from_stdout_even_with_noise_on_stderr(self):
        spec = self.spec(warm_up="echo '{\"ready_s\": 2.5}'; echo 'a warning' >&2")
        run_benchmark(self.config(spec), progress=lambda _: None)
        self.assertEqual(read_warm_up(self.out)[0]["details"], {"ready_s": 2.5})

    def test_a_run_that_reads_the_question_files_is_flagged(self):
        config = self.config(self.spec())
        config.leak_markers = ["grep -rn x ."]
        run_benchmark(config, progress=lambda _: None)
        self.assertEqual(read_records(self.out)[0]["leaks"], ["grep -rn x ."])
        config.leak_markers = ["/questions/"]
        os.remove(os.path.join(self.out, "runs.jsonl"))
        run_benchmark(config, progress=lambda _: None)
        self.assertEqual(read_records(self.out)[0]["leaks"], [])

    def test_the_results_dir_is_always_a_leak_marker(self):
        config = self.config(self.spec())
        self.assertIn(os.path.realpath(self.out), config_markers(config))

    def test_tool_starts_inside_a_session_are_counted_from_the_session_log(self):
        log = os.path.join(self.root, "starts.log")
        spec = self.spec(session_log=log,
                         warm_up=f"echo \"$(date +%s.%N) start\" >> {log}")
        executable(os.path.join(self.bin, "claude"), "#!/bin/sh\n"
                   f"echo \"$(date +%s.%N) start\" >> {log}\n"
                   f"exec {sys.executable} {os.path.join(self.bin, 'claude.py')} \"$@\"\n")
        with open(os.path.join(self.bin, "claude.py"), "w") as handle:
            handle.write(FAKE_CLAUDE.format(python=sys.executable))
        run_benchmark(self.config(spec), progress=lambda _: None)
        self.assertEqual([r["tool_started_in_session"] for r in read_records(self.out)], [1, 1])

    def test_without_a_session_log_the_count_is_unknown(self):
        run_benchmark(self.config(self.spec()), progress=lambda _: None)
        self.assertIsNone(read_records(self.out)[0]["tool_started_in_session"])

    def test_a_failed_warm_up_stops_before_any_agent_run_and_still_shuts_down(self):
        spec = self.spec(warm_up="echo broken >&2; exit 4", shut_down=f"echo stop >> {self.marks}")
        with self.assertRaisesRegex(WarmUpFailed, "tool.*exit 4"):
            run_benchmark(self.config(spec), progress=lambda _: None)
        self.assertEqual(read_records(self.out), [])
        self.assertFalse(os.path.exists(self.seen))
        with open(self.marks) as handle:
            self.assertEqual(handle.read().split(), ["stop"])

    def test_grep_and_st_ask_for_their_tools_without_skipping_permissions(self):
        st_binary = os.path.join(self.bin, "st")
        config = RunConfig(
            project=self.project, questions=QUESTIONS[:1], arms=["grep", "st"], repeats=1, out_dir=self.out,
            st_binary=st_binary, note="NOTE: st", max_turns=5, timeout_s=60,
            work_dir=os.path.join(self.root, "work"), skip_warm_up=True,
            env={"PATH": self.bin + os.pathsep + os.environ["PATH"], "FAKE_CLAUDE_SEEN": self.seen})
        run_benchmark(config, progress=lambda _: None)
        for seen in self.seen_runs():
            args = seen["args"]
            self.assertEqual(args[args.index("--tools") + 1], "Bash,Read,Grep,Glob")
            self.assertEqual(args[args.index("--allowedTools") + 1], "Bash,Read,Grep,Glob")
            self.assertNotIn("--dangerously-skip-permissions", args)
            self.assertNotIn("--mcp-config", args)


class ArmOrderTest(unittest.TestCase):
    def test_two_arms_alternate_as_before(self):
        for position in range(6):
            expected = ["grep", "st"] if position % 2 == 0 else ["st", "grep"]
            self.assertEqual(_arm_order(["grep", "st"], position), expected)

    def test_every_arm_goes_first_and_last_equally_often(self):
        arms = ["grep", "st", "intellij", "jdtls"]
        orders = [_arm_order(arms, position) for position in range(8)]
        for arm in arms:
            self.assertEqual(sum(order[0] == arm for order in orders), 2)
            self.assertEqual(sum(order[-1] == arm for order in orders), 2)
            self.assertTrue(all(sorted(order) == sorted(arms) for order in orders))

class WorkFolderTest(unittest.TestCase):
    def test_the_work_folder_must_not_hold_the_questions_or_the_results(self):
        self.assertEqual(work_problems("/w", "/r/out", "/q/questions.json", "/repo"), [])
        self.assertTrue(work_problems("/w", "/w/out", "/q/questions.json", "/repo"))
        self.assertTrue(work_problems("/w", "/r/out", "/w/q/questions.json", "/repo"))
        self.assertTrue(work_problems("/repo/x", "/r/out", "/q/questions.json", "/repo"))

if __name__ == "__main__":
    unittest.main()
