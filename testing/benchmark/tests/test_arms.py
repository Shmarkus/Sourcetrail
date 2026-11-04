import os
import shutil
import stat
import subprocess
import tempfile
import unittest

from stbench.arms import (ST_README_URL, path_hiding_st, path_with_st, readme_agent_section, copy_without_sourcetrail,
                          build_prompt, claude_args, st_note)

SNIPPET = os.path.join(os.path.dirname(__file__), "..", "..", "..", "docs", "agents", "st-claude-md-snippet.md")


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


class StNoteTest(unittest.TestCase):
    def test_points_the_snippets_readme_link_at_the_copy(self):
        with open(SNIPPET, encoding="utf-8") as handle:
            note = st_note(handle.read(), "/work/st-README.md")
        self.assertIn('"For agents" in `/work/st-README.md`', note)
        self.assertNotIn(ST_README_URL, note)


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


class ClaudeArgsTest(unittest.TestCase):
    def test_default_tools_are_allowed_explicitly_instead_of_skipping_permissions(self):
        args = claude_args("Q", None, None, False, 40)
        self.assertEqual(args[args.index("--tools") + 1], "Bash,Read,Grep,Glob")
        self.assertEqual(args[args.index("--allowedTools") + 1], "Bash,Read,Grep,Glob")
        self.assertNotIn("--dangerously-skip-permissions", args)
        self.assertIn("--strict-mcp-config", args)
        self.assertNotIn("--mcp-config", args)

    def test_an_arm_adds_tools_permissions_mcp_servers_and_settings(self):
        args = claude_args("Q", "m", "note", False, 40, tools=["Bash", "LSP"], allowed_tools=["Bash", "mcp__x"],
                           mcp_config="/w/mcp.json", settings="/w/settings.json")
        self.assertEqual(args[args.index("--tools") + 1], "Bash,LSP")
        self.assertEqual(args[args.index("--allowedTools") + 1], "Bash,mcp__x")
        self.assertEqual(args[args.index("--mcp-config") + 1], "/w/mcp.json")
        self.assertEqual(args[args.index("--settings") + 1], "/w/settings.json")
        self.assertEqual(args[args.index("--append-system-prompt") + 1], "note")

class CopyLeavesOutStStateTest(unittest.TestCase):
    def test_lock_and_server_logs_stay_behind(self):
        with tempfile.TemporaryDirectory() as root:
            source = os.path.join(root, "src")
            os.makedirs(source)
            for name in ["A.java", "app.srctrlprj.lock", ".st-serve.log", ".st-refresh.log", ".st-refresh.log.verbose"]:
                with open(os.path.join(source, name), "w") as handle:
                    handle.write("x")
            target = os.path.join(root, "copy")
            copy_without_sourcetrail(source, target)
            self.assertEqual(os.listdir(target), ["A.java"])

if __name__ == "__main__":
    unittest.main()
