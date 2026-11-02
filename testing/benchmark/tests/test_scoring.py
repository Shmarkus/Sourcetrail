import unittest

from stbench.scoring import extract_answer, score


ANSWER_TEXT = """I traced the callers.

```answer
- `io.spring.api.ArticleApi.updateArticle(String, User, UpdateArticleParam)`
- ArticleMutation#updateArticle
```
"""


class ExtractAnswerTest(unittest.TestCase):
    def test_takes_lines_of_the_answer_block(self):
        self.assertEqual(
            extract_answer(ANSWER_TEXT),
            ["io.spring.api.ArticleApi.updateArticle(String, User, UpdateArticleParam)", "ArticleMutation.updateArticle"],
        )

    def test_last_answer_block_wins(self):
        text = "```answer\nA.first\n```\nchanged my mind\n```answer\nB.second\n```"
        self.assertEqual(extract_answer(text), ["B.second"])

    def test_missing_block_is_none(self):
        self.assertIsNone(extract_answer("no block here\n```\nplain\n```"))

    def test_empty_block_is_an_empty_list(self):
        self.assertEqual(extract_answer("```answer\n\n```"), [])

    def test_numbered_items_and_double_colon(self):
        self.assertEqual(extract_answer("```answer\n1. A::b\n2) C.d\n```"), ["A.b", "C.d"])


class ScoreTest(unittest.TestCase):
    def test_all_required_and_nothing_forbidden_is_correct(self):
        result = score(
            ["io.spring.api.ArticleApi.updateArticle(String)", "ArticleMutation.updateArticle"],
            {"required": ["ArticleApi.updateArticle", "ArticleMutation.updateArticle"]},
        )
        self.assertEqual(result["verdict"], "correct")
        self.assertEqual(result["recall"], 1.0)

    def test_identifier_boundaries(self):
        result = score(["ArticleApi.updateArticleTitle"], {"required": ["ArticleApi.updateArticle"]})
        self.assertEqual(result["verdict"], "wrong")

    def test_alternatives(self):
        result = score(["UsersApi.java"], {"required": [["UsersApi.createUser", "UsersApi.java"]]})
        self.assertEqual(result["verdict"], "correct")

    def test_missing_one_is_partial(self):
        result = score(["A.x"], {"required": ["A.x", "B.y"]})
        self.assertEqual(result["verdict"], "partial")
        self.assertEqual(result["recall"], 0.5)
        self.assertEqual(result["missing"], ["B.y"])

    def test_forbidden_item_makes_a_full_answer_partial(self):
        result = score(["A.x", "Lookalike.x"], {"required": ["A.x"], "forbidden": ["Lookalike.x"]})
        self.assertEqual(result["verdict"], "partial")
        self.assertEqual(result["forbidden_hits"], ["Lookalike.x"])

    def test_extras_count_only_when_not_allowed(self):
        expected = {"required": ["A.x"], "allow_extra": False}
        self.assertEqual(score(["A.x", "B.z"], expected)["verdict"], "partial")
        self.assertEqual(score(["A.x", "B.z"], dict(expected, allow_extra=True))["verdict"], "correct")
        self.assertEqual(score(["A.x", "B.z"], expected)["extras"], ["B.z"])

    def test_optional_items_are_neither_required_nor_extras(self):
        expected = {"required": ["A.x"], "optional": ["Iface.x"]}
        self.assertEqual(score(["A.x", "Iface.x"], expected)["verdict"], "correct")
        self.assertEqual(score(["A.x"], expected)["verdict"], "correct")
        self.assertEqual(score(["Iface.x"], expected)["verdict"], "wrong")

    def test_no_answer_block(self):
        self.assertEqual(score(None, {"required": ["A.x"]})["verdict"], "no answer")

    def test_paths_match_as_suffix(self):
        result = score(["src/main/resources/application.properties"], {"required": ["application.properties"]})
        self.assertEqual(result["verdict"], "correct")


if __name__ == "__main__":
    unittest.main()
