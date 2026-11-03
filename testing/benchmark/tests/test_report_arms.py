import unittest

from stbench.report import render_markdown, summarize


def run(question, arm, verdict, tokens, calls, seconds, kind="structural", used=None, names=None):
    record = {
        "context_growth_tokens": tokens // 4, "missing": [], "extras": [], "forbidden_hits": [],
        "question": question, "kind": kind, "arm": arm, "verdict": verdict, "total_tokens": tokens,
        "output_tokens": tokens // 10, "tool_calls": calls, "tool_output_chars": calls * 1000,
        "duration_s": seconds, "cost_usd": tokens / 1e6, "used_st": arm == "st" and bool(used), "is_error": False,
        "used_arm_tool": used, "tool_names": names or {}, "repeat": 1,
    }
    return record


RUNS = [
    run("Q1", "grep", "correct", 40000, 6, 60.0),
    run("Q1", "st", "correct", 20000, 2, 30.0, used=True, names={"Bash": 2}),
    run("Q1", "intellij", "partial", 30000, 3, 40.0, used=True,
        names={"mcp__intellij__analyze_calls": 2, "Read": 1}),
    run("Q2", "grep", "correct", 10000, 2, 20.0, kind="text"),
    run("Q2", "st", "correct", 12000, 2, 22.0, kind="text", used=False, names={"Bash": 2}),
    run("Q2", "intellij", "correct", 10000, 2, 20.0, kind="text", used=False, names={"Grep": 2}),
]

WARM_UP = [
    {"arm": "intellij", "phase": "warm_up", "seconds": 95.2, "exit": 0, "details": {"warm_up_s": 87.0, "ready_s": 87.0}},
    {"arm": "jdtls", "phase": "warm_up", "seconds": 20.0, "exit": 0, "details": {}},
    {"arm": "jdtls", "phase": "startup", "seconds": 7.0, "exit": 0, "details": {}},
    {"arm": "jdtls", "phase": "startup", "seconds": 29.0, "exit": 0, "details": {"startup_s": 9.0}},
    {"arm": "jdtls", "phase": "startup", "seconds": 8.0, "exit": 0, "details": {}},
    {"arm": "jdtls", "phase": "shut_down", "seconds": 0.1, "exit": 0, "details": {}},
]


class MultiArmSummaryTest(unittest.TestCase):
    def test_adoption_per_arm_counts_the_arms_own_tool(self):
        adoption = summarize(RUNS)["adoption_by_arm"]
        self.assertNotIn("grep", adoption)
        self.assertEqual(adoption["intellij"]["structural_runs"], 1)
        self.assertEqual(adoption["intellij"]["structural_with_tool"], 1)
        self.assertEqual(adoption["intellij"]["text_with_tool"], 0)
        self.assertEqual(adoption["intellij"]["calls_by_tool"], {"Grep": 2, "mcp__intellij__analyze_calls": 2, "Read": 1})

    def test_old_st_records_without_the_generic_field_still_count(self):
        old = [{k: v for k, v in r.items() if k not in ("used_arm_tool", "tool_names")} for r in RUNS[:2]]
        self.assertEqual(summarize(old)["adoption_by_arm"]["st"]["structural_with_tool"], 1)

    def test_change_against_grep_for_every_arm(self):
        changes = summarize(RUNS)["change_by_arm"]
        self.assertAlmostEqual(changes["intellij"]["median_tokens"], (20000 - 25000) / 25000)
        self.assertNotIn("grep", changes)


class MultiArmMarkdownTest(unittest.TestCase):
    def test_one_column_per_arm_with_the_change_against_grep(self):
        text = render_markdown(summarize(RUNS), RUNS, speed=None)
        self.assertIn("| Metric | grep | st | intellij | Target |", text)
        self.assertIn("| Correct runs | 2 / 2 | 2 / 2 | 1 / 2 | no worse than grep |", text)
        self.assertIn("| Tokens per question (median) | 25,000 | 16,000 (-36%) | 20,000 (-20%) |", text)

    def test_columns_follow_the_given_arm_order_whatever_ran_first(self):
        shuffled = [RUNS[2], RUNS[1], RUNS[0]] + RUNS[3:]
        text = render_markdown(summarize(shuffled), shuffled, speed=None, meta={"arms": "grep, st, intellij"})
        self.assertIn("| Metric | grep | st | intellij | Target |", text)
        self.assertIn("| Question | Kind | grep | st | intellij |", text)

    def test_adoption_lines_per_arm_with_calls_by_tool(self):
        text = render_markdown(summarize(RUNS), RUNS, speed=None)
        self.assertIn("- `intellij`: its tool used on 1 of 1 structural and stack-trace runs (100%", text)
        self.assertIn("mcp__intellij__analyze_calls 2", text)

    def test_per_question_cells_per_arm(self):
        text = render_markdown(summarize(RUNS), RUNS, speed=None)
        self.assertIn("| Question | Kind | grep | st | intellij |", text)
        self.assertIn("| Q1 | structural | ✓ 6 / 40,000 | ✓ 2 / 20,000 | ½ 3 / 30,000 |", text)

    def test_two_arm_reports_keep_their_shape(self):
        two = [r for r in RUNS if r["arm"] != "intellij"]
        text = render_markdown(summarize(two), two, speed=None)
        self.assertIn("| Metric | grep | st | Change | Target | Met |", text)
        self.assertNotIn("Warm-up", text)


class WarmUpMarkdownTest(unittest.TestCase):
    def test_warm_up_and_startup_are_reported_apart_from_question_time(self):
        text = render_markdown(summarize(RUNS), RUNS, speed=None, warm_up=WARM_UP)
        self.assertIn("## Warm-up and per-session startup", text)
        self.assertIn("| intellij | 87.0 s | 95.2 s | ready_s 87.0 | none |", text)
        self.assertIn("| jdtls | 20.0 s | 20.0 s |  | 8.0 s (7.0–9.0 s, 3 runs) |", text)

    def test_only_times_are_shown_as_the_tools_own_measure(self):
        entries = [dict(WARM_UP[0], details={"ready_s": 87.0, "tools": 11, "pid": 5})]
        text = render_markdown(summarize(RUNS), RUNS, speed=None, warm_up=entries)
        self.assertIn("| intellij | 95.2 s | 95.2 s | ready_s 87.0 | none |", text)

    def test_a_failed_warm_up_shows_its_exit_code(self):
        failed = [dict(WARM_UP[0], exit=3)]
        text = render_markdown(summarize(RUNS), RUNS, speed=None, warm_up=failed)
        self.assertIn("failed (exit 3)", text)


class SafetyMarkdownTest(unittest.TestCase):
    def test_permission_denials_are_totalled_per_arm(self):
        runs = [dict(r, permission_denials=1 if r["arm"] == "intellij" else 0) for r in RUNS]
        text = render_markdown(summarize(runs), runs, speed=None)
        self.assertIn("| Permission denials (total) | 0 | 0 | 2 |", text)

    def test_runs_touching_question_or_result_files_are_flagged(self):
        runs = [dict(r, leaks=["/questions/"] if r["question"] == "Q1" and r["arm"] == "st" else []) for r in RUNS]
        text = render_markdown(summarize(runs), runs, speed=None)
        self.assertIn("| Runs that touched question or result files | 0 | 1 | 0 |", text)
        self.assertIn("- Q1 st #1: `/questions/`", text)

    def test_two_arm_reports_show_denials_and_leaks_too(self):
        two = [dict(r, permission_denials=0, leaks=[]) for r in RUNS if r["arm"] != "intellij"]
        text = render_markdown(summarize(two), two, speed=None)
        self.assertIn("| Permission denials (total) | 0 | 0 |", text)
        self.assertIn("| Runs that touched question or result files | 0 | 0 |", text)


class SessionStartTest(unittest.TestCase):
    def test_question_time_with_and_without_a_tool_start_in_the_session(self):
        runs = [run("Q1", "jdtls", "correct", 1000, 2, 40.0), run("Q2", "jdtls", "correct", 1000, 2, 10.0),
                run("Q3", "jdtls", "correct", 1000, 2, 12.0), run("Q1", "grep", "correct", 1000, 2, 9.0)]
        for record, started in zip(runs, [1, 0, 0, None]):
            record["tool_started_in_session"] = started
        text = render_markdown(summarize(runs), runs, speed=None, warm_up=WARM_UP)
        self.assertIn("- `jdtls`: median question time 40.0 s in 1 runs that started the tool, "
                      "11.0 s in 2 runs that did not.", text)

if __name__ == "__main__":
    unittest.main()
