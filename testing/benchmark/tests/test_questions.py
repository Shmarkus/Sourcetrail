import json
import os
import tempfile
import unittest

from stbench.questions import load_questions, QuestionError

VALID = {
    "project": {"name": "demo", "repo": "https://example.com/demo.git", "commit": "abc"},
    "questions": [
        {"id": "Q1", "kind": "structural", "prompt": "Who calls X?", "answer_items": "methods",
         "expected": {"required": ["A.x"]}},
        {"id": "Q2", "kind": "text", "prompt": "Where is key y set?", "answer_items": "files",
         "expected": {"required": [["a.properties", "a.yml"]], "allow_extra": True}},
    ],
}


def write(data):
    handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump(data, handle)
    handle.close()
    return handle.name


class LoadQuestionsTest(unittest.TestCase):
    def tearDown(self):
        for path in getattr(self, "paths", []):
            os.remove(path)

    def load(self, data, only=None):
        self.paths = getattr(self, "paths", []) + [write(data)]
        return load_questions(self.paths[-1], only)

    def test_valid_file(self):
        data = self.load(VALID)
        self.assertEqual([q["id"] for q in data["questions"]], ["Q1", "Q2"])
        self.assertEqual(data["project"]["name"], "demo")

    def test_only_selects_questions(self):
        self.assertEqual([q["id"] for q in self.load(VALID, only=["Q2"])["questions"]], ["Q2"])

    def test_unknown_selection_is_an_error(self):
        with self.assertRaisesRegex(QuestionError, "Q9"):
            self.load(VALID, only=["Q9"])

    def test_duplicate_ids(self):
        data = json.loads(json.dumps(VALID))
        data["questions"][1]["id"] = "Q1"
        with self.assertRaisesRegex(QuestionError, "duplicate"):
            self.load(data)

    def test_unknown_kind(self):
        data = json.loads(json.dumps(VALID))
        data["questions"][0]["kind"] = "vibes"
        with self.assertRaisesRegex(QuestionError, "kind"):
            self.load(data)

    def test_required_answers_are_mandatory(self):
        data = json.loads(json.dumps(VALID))
        data["questions"][0]["expected"] = {"required": []}
        with self.assertRaisesRegex(QuestionError, "required"):
            self.load(data)

    def test_prompt_must_not_name_st(self):
        data = json.loads(json.dumps(VALID))
        data["questions"][0]["prompt"] = "Use st to find who calls X"
        with self.assertRaisesRegex(QuestionError, "names st"):
            self.load(data)


if __name__ == "__main__":
    unittest.main()
