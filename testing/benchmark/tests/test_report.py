import unittest

from stbench.report import summarize, render_markdown
from stbench.speed import parse_refreshed, median, with_no_refresh


def run(question, arm, verdict, tokens, calls, seconds, used_st=False, kind="structural"):
    return {
        "question": question,
        "kind": kind,
        "arm": arm,
        "verdict": verdict,
        "total_tokens": tokens,
        "output_tokens": tokens // 10,
        "tool_calls": calls,
        "tool_output_chars": calls * 1000,
        "duration_s": seconds,
        "cost_usd": tokens / 1e6,
        "used_st": used_st,
        "is_error": False,
    }


RUNS = [
    run("Q1", "grep", "correct", 40000, 6, 60.0),
    run("Q1", "st", "correct", 20000, 2, 30.0, used_st=True),
    run("Q2", "grep", "partial", 60000, 8, 80.0),
    run("Q2", "st", "correct", 30000, 3, 50.0, used_st=True),
    run("Q3", "grep", "correct", 10000, 2, 20.0, kind="text"),
    run("Q3", "st", "correct", 12000, 2, 22.0, kind="text"),
]


class SummarizeTest(unittest.TestCase):
    def test_per_arm_counts_and_medians(self):
        summary = summarize(RUNS)
        grep = summary["arms"]["grep"]
        st = summary["arms"]["st"]
        self.assertEqual(grep["runs"], 3)
        self.assertEqual(grep["correct"], 2)
        self.assertEqual(grep["partial"], 1)
        self.assertEqual(st["correct"], 3)
        self.assertEqual(grep["median_tokens"], 40000)
        self.assertEqual(st["median_tokens"], 20000)
        self.assertEqual(st["median_tool_calls"], 2)
        self.assertEqual(grep["total_tool_calls"], 16)

    def test_adoption_counts_structural_questions_only(self):
        adoption = summarize(RUNS)["adoption"]
        self.assertEqual(adoption["structural_runs"], 2)
        self.assertEqual(adoption["structural_with_st"], 2)
        self.assertEqual(adoption["text_runs"], 1)
        self.assertEqual(adoption["text_with_st"], 0)

    def test_change_against_grep(self):
        change = summarize(RUNS)["change"]
        self.assertAlmostEqual(change["median_tokens"], -0.5)
        self.assertAlmostEqual(change["median_tool_calls"], -2 / 3)

    def test_errored_runs_are_counted_apart(self):
        broken = dict(RUNS[1], is_error=True, verdict="no answer")
        summary = summarize([RUNS[0], broken])
        self.assertEqual(summary["arms"]["st"]["errors"], 1)
        self.assertEqual(summary["arms"]["st"]["runs"], 0)
        self.assertEqual(summary["arms"]["st"]["correct"], 0)


class MarkdownTest(unittest.TestCase):
    def test_has_the_criteria_table_and_one_row_per_question(self):
        text = render_markdown(summarize(RUNS), RUNS, speed=None)
        self.assertIn("| Correct runs | 2 / 3 | 3 / 3 |", text)
        self.assertIn("| Q2 | structural |", text)
        self.assertIn("Adoption", text)

    def test_times_carry_units_and_zero_change_has_no_sign(self):
        same = [dict(r, duration_s=10.0) for r in RUNS]
        text = render_markdown(summarize(same), same, speed=None)
        self.assertIn("| 10.0 s | 10.0 s | 0% |", text)

    def test_setup_is_listed_when_given(self):
        meta = {"model": "claude-x", "claude_version": "2.1.316", "repeats": 3, "sourcetrail_commit": "abc1234"}
        text = render_markdown(summarize(RUNS), RUNS, speed=None, meta=meta)
        self.assertIn("claude-x", text)
        self.assertIn("abc1234", text)


class SpeedHelpersTest(unittest.TestCase):
    def test_parse_refreshed_line(self):
        self.assertEqual(parse_refreshed("x\nrefreshed 3 files in 1.2s\n"), (3, 1.2))
        self.assertIsNone(parse_refreshed("callers: 2 (project shop)\n"))

    def test_no_refresh_goes_on_every_st_of_a_pipeline(self):
        self.assertEqual(with_no_refresh("st at A.java:3 | st callers --depth 4"),
                         "st at --no-refresh A.java:3 | st callers --no-refresh --depth 4")
        self.assertEqual(with_no_refresh("st deps io.spring.api"), "st deps --no-refresh io.spring.api")

    def test_median(self):
        self.assertEqual(median([3.0, 1.0, 2.0]), 2.0)
        self.assertEqual(median([1.0, 2.0]), 1.5)
        self.assertIsNone(median([]))


if __name__ == "__main__":
    unittest.main()
