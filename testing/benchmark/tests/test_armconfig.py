import json
import os
import tempfile
import unittest

from stbench.armconfig import ARMS_DIR, ArmError, load_arm, resolve


def write_arm(directory, name, **fields):
    data = {"name": name, "tools": ["Bash", "Read", "Grep", "Glob"]}
    data.update(fields)
    path = os.path.join(directory, f"{name}.json")
    with open(path, "w") as handle:
        json.dump(data, handle)
    return path


class BuiltInArmsTest(unittest.TestCase):
    def test_grep_works_on_a_clean_copy_without_st_or_mcp(self):
        spec = load_arm("grep")
        self.assertEqual(spec.workdir, "copy")
        self.assertEqual(spec.copy_name, "project-without-sourcetrail")
        self.assertEqual(spec.st, "hidden")
        self.assertEqual(spec.tools, ["Bash", "Read", "Grep", "Glob"])
        self.assertEqual(spec.allowed_tools, spec.tools)
        self.assertIsNone(spec.mcp_servers)
        self.assertIsNone(spec.note)

    def test_st_works_on_the_indexed_project_with_st_on_path_and_its_note(self):
        spec = load_arm("st")
        self.assertEqual(spec.workdir, "project")
        self.assertEqual(spec.st, "on_path")
        self.assertEqual(spec.tools, ["Bash", "Read", "Grep", "Glob"])
        self.assertTrue(spec.note.endswith("st-claude-md-snippet.md"))
        self.assertTrue(os.path.isfile(spec.note))
        self.assertTrue(spec.note_readme_section)
        self.assertEqual(spec.adoption["programs"], ["st"])

    def test_every_arm_file_loads_and_shares_the_base_tools(self):
        names = sorted(name[:-5] for name in os.listdir(ARMS_DIR) if name.endswith(".json"))
        self.assertEqual(names, ["grep", "intellij", "jdtls", "st"])
        for name in names:
            spec = load_arm(name)
            self.assertEqual(spec.name, name)
            self.assertEqual(spec.tools[:4], ["Bash", "Read", "Grep", "Glob"], name)
            if spec.note:
                self.assertTrue(os.path.isfile(spec.note), spec.note)

    def test_tool_arms_hide_st_and_name_the_tools_they_adopt(self):
        intellij, jdtls = load_arm("intellij"), load_arm("jdtls")
        self.assertEqual(intellij.st, "hidden")
        self.assertIn("intellij", intellij.mcp_servers)
        self.assertEqual(intellij.adoption["tool_prefixes"], ["mcp__intellij__"])
        self.assertEqual(jdtls.st, "hidden")
        self.assertIn("LSP", jdtls.tools)
        self.assertEqual(jdtls.adoption["tools"], ["LSP"])


class LoadArmTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def test_a_path_loads_and_relative_files_resolve_beside_it(self):
        with open(os.path.join(self.dir, "note.md"), "w") as handle:
            handle.write("note")
        spec = load_arm(write_arm(self.dir, "x", note="note.md"))
        self.assertEqual(spec.note, os.path.join(self.dir, "note.md"))
        self.assertEqual(spec.directory, self.dir)

    def test_unknown_keys_and_bad_values_are_refused(self):
        with self.assertRaisesRegex(ArmError, "unknown"):
            load_arm(write_arm(self.dir, "x", colour="red"))
        with self.assertRaisesRegex(ArmError, "workdir"):
            load_arm(write_arm(self.dir, "y", workdir="somewhere"))
        with self.assertRaisesRegex(ArmError, "st"):
            load_arm(write_arm(self.dir, "z", st="maybe"))
        with self.assertRaisesRegex(ArmError, "tools"):
            load_arm(write_arm(self.dir, "w", tools=[]))

    def test_an_unknown_arm_name_says_where_it_looked(self):
        with self.assertRaisesRegex(ArmError, "nope"):
            load_arm("nope")


class ResolveTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def test_placeholders_come_from_the_run_then_the_environment_then_the_arm_defaults(self):
        spec = load_arm(write_arm(
            self.dir, "x", vars={"PORT": "1", "HOME_DIR": "~/x"},
            env={"A": "{work}/a", "B": "{PORT}", "C": "{HOME_DIR}"},
            path_prepend=["{arms}/bin"], warm_up="start --port {PORT} --dir {workdir}",
            mcp_servers={"t": {"type": "sse", "url": "http://127.0.0.1:{PORT}/sse"}}))
        resolved = resolve(spec, {"work": "/w", "workdir": "/w/p"}, environ={"PORT": "7"})
        self.assertEqual(resolved.env, {"A": "/w/a", "B": "7", "C": os.path.expanduser("~/x")})
        self.assertEqual(resolved.path_prepend, [os.path.join(self.dir, "bin")])
        self.assertEqual(resolved.warm_up, "start --port 7 --dir /w/p")
        self.assertEqual(resolved.mcp_servers["t"]["url"], "http://127.0.0.1:7/sse")

    def test_an_unknown_placeholder_is_an_error(self):
        spec = load_arm(write_arm(self.dir, "x", warm_up="run {nothing}"))
        with self.assertRaisesRegex(ArmError, "nothing"):
            resolve(spec, {}, environ={})

    def test_resolving_leaves_the_loaded_spec_alone(self):
        spec = load_arm(write_arm(self.dir, "x", env={"A": "{work}"}))
        resolve(spec, {"work": "/w"}, environ={})
        self.assertEqual(spec.env, {"A": "{work}"})


if __name__ == "__main__":
    unittest.main()
