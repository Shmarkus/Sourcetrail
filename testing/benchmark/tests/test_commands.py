import unittest

from stbench.commands import command_words, st_invocations, is_text_tool


class CommandWordsTest(unittest.TestCase):
    def test_single_command(self):
        self.assertEqual(command_words("st find OrderService"), ["st"])

    def test_pipeline_and_lists(self):
        self.assertEqual(
            command_words("cd src && st callers X | st filter --kind method; grep -rn foo ."),
            ["cd", "st", "st", "grep"],
        )

    def test_separators_inside_quotes_are_ignored(self):
        self.assertEqual(command_words("grep -rn 'a | st find' ."), ["grep"])
        self.assertEqual(command_words('echo "x; st y"'), ["echo"])

    def test_env_assignments_and_time_prefix_are_skipped(self):
        self.assertEqual(command_words("FOO=1 time st impact X"), ["st"])

    def test_path_to_binary_counts_by_base_name(self):
        self.assertEqual(command_words("/opt/bin/st at A.java:3"), ["st"])

    def test_command_substitution_counts(self):
        self.assertEqual(command_words("echo $(st find X)"), ["echo", "st"])

    def test_unbalanced_quotes_do_not_crash(self):
        self.assertEqual(command_words("grep 'oops"), ["grep"])


class StInvocationsTest(unittest.TestCase):
    def test_counts_each_st_in_a_pipeline(self):
        self.assertEqual(st_invocations("st at A.java:4 | st callers --depth 3 | st filter --annotated Test"), 3)

    def test_words_starting_with_st_are_not_st(self):
        self.assertEqual(st_invocations("stat file; strings x; ./start.sh"), 0)


class TextToolTest(unittest.TestCase):
    def test_grep_family(self):
        for word in ["grep", "rg", "find", "sed", "awk", "cat", "head", "tail"]:
            self.assertTrue(is_text_tool(word), word)

    def test_st_is_not_a_text_tool(self):
        self.assertFalse(is_text_tool("st"))


if __name__ == "__main__":
    unittest.main()
