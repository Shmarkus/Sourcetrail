import unittest

from stbench.rescore import rescore


class RescoreTest(unittest.TestCase):
    def test_new_ground_truth_changes_the_verdict_from_the_stored_answer(self):
        records = [{"question": "Q6", "arm": "st", "answer": ["A.x", "B.y"], "verdict": "partial",
                    "extras": ["B.y"], "is_error": False}]
        questions = {"Q6": {"expected": {"required": ["A.x", "B.y"]}}}
        updated = rescore(records, questions)
        self.assertEqual(updated[0]["verdict"], "correct")
        self.assertEqual(updated[0]["extras"], [])
        self.assertEqual(records[0]["verdict"], "partial")  # the input stays as it was

    def test_failed_runs_and_unknown_questions_are_left_alone(self):
        records = [{"question": "Q1", "arm": "st", "answer": None, "verdict": "no answer", "is_error": True},
                   {"question": "Q9", "arm": "st", "answer": ["A.x"], "verdict": "wrong", "is_error": False}]
        updated = rescore(records, {"Q1": {"expected": {"required": ["A.x"]}}})
        self.assertEqual([r["verdict"] for r in updated], ["no answer", "wrong"])


if __name__ == "__main__":
    unittest.main()
