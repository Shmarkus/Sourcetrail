import json
import os
import shutil
import stat
import sys
import tempfile
import textwrap
import unittest

from stbench.runner import RunConfig, run_benchmark, read_records

FAKE_CLAUDE = textwrap.dedent("""\
    #!{python}
    # Fake `claude -p`: records what it saw, answers through a stream-json log.
    import json, os, subprocess, sys
    args = sys.argv[1:]
    prompt = args[args.index("-p") + 1]
    note = args[args.index("--append-system-prompt") + 1] if "--append-system-prompt" in args else None
    st = subprocess.run("st status", shell=True, capture_output=True, text=True)
    seen = {{"cwd": os.getcwd(), "note": note, "prompt": prompt, "st_exit": st.returncode,
             "index_visible": any(n.endswith(".srctrlprj") for n in os.listdir("."))}}
    with open(os.environ["FAKE_CLAUDE_SEEN"], "a") as handle:
        handle.write(json.dumps(seen) + "\\n")
    command = "st callers A.x" if st.returncode == 0 else "grep -rn x ."
    answer = "A.x" if st.returncode == 0 else "Wrong.y"
    for line in [
        {{"type": "system", "subtype": "init"}},
        {{"type": "assistant", "message": {{"content": [
            {{"type": "tool_use", "id": "t1", "name": "Bash", "input": {{"command": command}}}}]}}}},
        {{"type": "user", "message": {{"content": [{{"type": "tool_result", "tool_use_id": "t1", "content": "out"}}]}}}},
        {{"type": "result", "subtype": "success", "is_error": False, "num_turns": 2, "duration_ms": 1000,
          "total_cost_usd": 0.01, "result": "```answer\\n" + answer + "\\n```",
          "usage": {{"input_tokens": 10, "output_tokens": 5}}}},
    ]:
        print(json.dumps(line))
""")

FAKE_ST = "#!/bin/sh\necho \"st $*\"\nexit 0\n"

QUESTIONS = {
    "project": {"name": "demo"},
    "questions": [
        {"id": "Q1", "kind": "structural", "prompt": "Who calls A.x?", "answer_items": "methods",
         "expected": {"required": ["A.x"]}},
        {"id": "Q2", "kind": "text", "prompt": "Where is key y?", "answer_items": "files",
         "expected": {"required": ["A.x"]}},
    ],
}


def executable(path, text):
    with open(path, "w") as handle:
        handle.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class RunBenchmarkTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.bin = os.path.join(self.root, "bin")
        os.makedirs(self.bin)
        executable(os.path.join(self.bin, "claude"), FAKE_CLAUDE.format(python=sys.executable))
        st_binary = os.path.join(self.root, "build", "st")
        os.makedirs(os.path.dirname(st_binary))
        executable(st_binary, FAKE_ST)
        self.project = os.path.join(self.root, "project")
        os.makedirs(os.path.join(self.project, "src"))
        for name in ["src/A.java", "demo.srctrlprj", "demo.srctrldb"]:
            with open(os.path.join(self.project, name), "w") as handle:
                handle.write("x")
        self.seen = os.path.join(self.root, "seen.jsonl")
        self.out = os.path.join(self.root, "out")
        self.config = RunConfig(
            project=self.project, questions=QUESTIONS["questions"], arms=["grep", "st"], repeats=1,
            out_dir=self.out, st_binary=st_binary, note="NOTE: st exists", model=None, bare=False,
            max_turns=5, timeout_s=60, work_dir=os.path.join(self.root, "work"),
            env={"PATH": self.bin + os.pathsep + os.environ["PATH"], "FAKE_CLAUDE_SEEN": self.seen},
        )

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def seen_runs(self):
        with open(self.seen) as handle:
            return [json.loads(line) for line in handle]

    def test_one_record_per_question_and_arm(self):
        run_benchmark(self.config, progress=lambda _: None)
        records = read_records(self.out)
        self.assertEqual(sorted((r["question"], r["arm"]) for r in records),
                         [("Q1", "grep"), ("Q1", "st"), ("Q2", "grep"), ("Q2", "st")])

    def test_arms_differ_only_in_st_index_and_note(self):
        run_benchmark(self.config, progress=lambda _: None)
        by_arm = {}
        for seen in self.seen_runs():
            arm = "st" if seen["note"] else "grep"
            by_arm.setdefault(arm, []).append(seen)
        for seen in by_arm["grep"]:
            self.assertEqual(seen["st_exit"], 127)
            self.assertFalse(seen["index_visible"])
            self.assertNotEqual(os.path.realpath(seen["cwd"]), os.path.realpath(self.project))
        for seen in by_arm["st"]:
            self.assertEqual(seen["st_exit"], 0)
            self.assertEqual(os.path.realpath(seen["cwd"]), os.path.realpath(self.project))
            self.assertEqual(seen["note"], "NOTE: st exists")
        prompts = {seen["prompt"] for seen in self.seen_runs() if "A.x" in seen["prompt"]}
        self.assertEqual(len(prompts), 1)

    def test_the_grep_copy_lives_outside_the_results_tree(self):
        run_benchmark(self.config, progress=lambda _: None)
        for seen in self.seen_runs():
            if not seen["note"]:
                self.assertFalse(os.path.realpath(seen["cwd"]).startswith(os.path.realpath(self.out)))

    def test_records_carry_score_and_metrics(self):
        run_benchmark(self.config, progress=lambda _: None)
        records = {(r["question"], r["arm"]): r for r in read_records(self.out)}
        self.assertEqual(records[("Q1", "st")]["verdict"], "correct")
        self.assertTrue(records[("Q1", "st")]["used_st"])
        self.assertEqual(records[("Q1", "grep")]["verdict"], "wrong")
        self.assertEqual(records[("Q1", "grep")]["tool_calls"], 1)
        self.assertTrue(os.path.isfile(records[("Q1", "st")]["log"]))

    def test_resume_skips_finished_runs(self):
        run_benchmark(self.config, progress=lambda _: None)
        os.remove(self.seen)
        run_benchmark(self.config, resume=True, progress=lambda _: None)
        self.assertFalse(os.path.exists(self.seen))
        self.assertEqual(len(read_records(self.out)), 4)

    def test_failed_claude_is_recorded_as_an_error(self):
        executable(os.path.join(self.bin, "claude"), "#!/bin/sh\necho boom >&2\nexit 3\n")
        run_benchmark(self.config, progress=lambda _: None)
        records = read_records(self.out)
        self.assertTrue(all(r["is_error"] for r in records))
        self.assertIn("exit 3", records[0]["error"])


if __name__ == "__main__":
    unittest.main()
