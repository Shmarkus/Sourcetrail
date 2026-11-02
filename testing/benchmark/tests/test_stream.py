import os
import unittest

from stbench.stream import parse_stream

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")


def load(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as handle:
        return parse_stream(handle)


class ParseStreamTest(unittest.TestCase):
    def test_counts_tool_calls_and_st_use(self):
        metrics = load("run_mixed.jsonl")
        self.assertEqual(metrics["tool_calls"], 3)
        self.assertEqual(metrics["st_calls"], 1)
        self.assertEqual(metrics["st_invocations"], 2)
        self.assertEqual(metrics["text_tool_calls"], 2)  # the grep pipeline and the Read
        self.assertTrue(metrics["used_st"])

    def test_tool_output_characters_cover_string_and_block_results(self):
        metrics = load("run_mixed.jsonl")
        expected = len("io.spring.A.x\tmethod\tA.java:3\n") + len("src/A.java:3: save(x)") + len("1\tclass A {}")
        self.assertEqual(metrics["tool_output_chars"], expected)

    def test_usage_cost_and_time_come_from_the_result(self):
        metrics = load("run_mixed.jsonl")
        self.assertEqual(metrics["input_tokens"], 100)
        self.assertEqual(metrics["cache_creation_tokens"], 2000)
        self.assertEqual(metrics["cache_read_tokens"], 30000)
        self.assertEqual(metrics["output_tokens"], 400)
        self.assertEqual(metrics["total_tokens"], 32500)
        self.assertAlmostEqual(metrics["cost_usd"], 0.0421)
        self.assertAlmostEqual(metrics["duration_s"], 12.345)
        self.assertEqual(metrics["turns"], 4)
        self.assertFalse(metrics["is_error"])

    def test_final_text_and_commands_are_kept(self):
        metrics = load("run_mixed.jsonl")
        self.assertIn("```answer", metrics["final_text"])
        self.assertEqual(metrics["commands"][0], "st callers ArticleRepository.save | st filter --kind method")

    def test_error_run(self):
        metrics = load("run_error.jsonl")
        self.assertTrue(metrics["is_error"])
        self.assertEqual(metrics["error"], "api_error: Authentication error")
        self.assertEqual(metrics["tool_calls"], 0)
        self.assertFalse(metrics["used_st"])

    def test_context_growth_is_what_the_task_added_to_the_first_context(self):
        metrics = load("run_context.jsonl")
        self.assertEqual(metrics["api_calls"], 3)  # m1 arrives twice
        self.assertEqual(metrics["initial_context_tokens"], 50005)
        self.assertEqual(metrics["final_context_tokens"], 52502 + 120)
        self.assertEqual(metrics["context_growth_tokens"], 52622 - 50005)

    def test_no_per_call_usage_leaves_context_unknown(self):
        metrics = load("run_error.jsonl")
        self.assertIsNone(metrics["context_growth_tokens"])

    def test_missing_result_line_is_an_error(self):
        metrics = parse_stream(['{"type":"system","subtype":"init"}', "not json"])
        self.assertTrue(metrics["is_error"])
        self.assertEqual(metrics["error"], "no result message (the run was cut off)")


if __name__ == "__main__":
    unittest.main()
