import os
import shutil
import stat
import subprocess
import tempfile
import unittest

from stbench.arms import path_hiding_st, path_with_st, readme_agent_section, copy_without_sourcetrail, build_prompt


def make_executable(path):
    with open(path, "w") as handle:
        handle.write("#!/bin/sh\n")
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


class HideStTest(unittest.TestCase):
    def test_a_shadow_st_comes_first_and_answers_command_not_found(self):
        with tempfile.TemporaryDirectory() as root:
            real = os.path.join(root, "bin")
            os.makedirs(real)
            make_executable(os.path.join(real, "st"))
            shadow = os.path.join(root, "shadow")
            path = path_hiding_st(real, shadow)
            self.assertEqual(path.split(os.pathsep), [shadow, real])
            done = subprocess.run(["st", "callers", "X"], env={"PATH": path}, capture_output=True, text=True)
            self.assertEqual(done.returncode, 127)
            self.assertIn("command not found", done.stderr)

    def test_other_tools_in_the_same_directory_stay_reachable(self):
        with tempfile.TemporaryDirectory() as root:
            real = os.path.join(root, "bin")
            os.makedirs(real)
            make_executable(os.path.join(real, "st"))
            make_executable(os.path.join(real, "rg"))
            path = path_hiding_st(real, os.path.join(root, "shadow"))
            self.assertEqual(shutil.which("rg", path=path), os.path.join(real, "rg"))


class PathWithStTest(unittest.TestCase):
    def test_puts_the_given_binary_first_as_st(self):
        with tempfile.TemporaryDirectory() as root:
            binary = os.path.join(root, "build", "st")
            os.makedirs(os.path.dirname(binary))
            make_executable(binary)
            path = path_with_st("/usr/bin", binary, os.path.join(root, "link"))
            self.assertEqual(os.path.realpath(shutil.which("st", path=path)), os.path.realpath(binary))


README = """# Sourcetrail
intro
## `st`: code navigation for agents

st answers questions.

### For agents
table
### Build and reference
reference
## Existing application test suite
other
"""


class ReadmeSectionTest(unittest.TestCase):
    def test_takes_the_st_section_up_to_the_next_level_two_heading(self):
        section = readme_agent_section(README)
        self.assertTrue(section.startswith("## `st`: code navigation for agents"))
        self.assertIn("### Build and reference", section)
        self.assertNotIn("Existing application test suite", section)

    def test_missing_section_raises(self):
        with self.assertRaises(ValueError):
            readme_agent_section("# nothing\n")


class CopyWithoutSourcetrailTest(unittest.TestCase):
    def test_leaves_out_index_files_and_build_output(self):
        with tempfile.TemporaryDirectory() as root:
            source = os.path.join(root, "src")
            for relative in [
                "build.gradle",
                "src/main/java/A.java",
                "app.srctrlprj",
                "app.srctrldb",
                "app.srctrlbm",
                "app.srctrlst",
                "sourcetrail_dependencies/x/delombok/A.java",
                "build/classes/A.class",
                ".gradle/cache",
            ]:
                full = os.path.join(source, relative)
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "w") as handle:
                    handle.write("x")
            target = os.path.join(root, "copy")
            copy_without_sourcetrail(source, target)
            copied = sorted(
                os.path.relpath(os.path.join(directory, name), target)
                for directory, _, names in os.walk(target)
                for name in names
            )
            self.assertEqual(copied, ["build.gradle", os.path.join("src", "main", "java", "A.java")])


class BuildPromptTest(unittest.TestCase):
    def test_same_prompt_for_every_arm_with_the_answer_format(self):
        prompt = build_prompt({"prompt": "Who calls X?", "answer_items": "methods as `Class.method`"})
        self.assertIn("Who calls X?", prompt)
        self.assertIn("```answer", prompt)
        self.assertIn("methods as `Class.method`", prompt)
        self.assertNotIn(" st ", prompt)


if __name__ == "__main__":
    unittest.main()
