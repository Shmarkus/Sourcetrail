import os
import shutil
import stat
import tempfile
import unittest

from stbench.speed import measure_refresh


def fake_st(directory, refreshes):
    path = os.path.join(directory, "st")
    line = 'echo "refreshed 1 files in 2.0s" >&2\n' if refreshes else ""
    with open(path, "w") as handle:
        handle.write("#!/bin/sh\n" + line + "echo \"find: 0 (project p)\" >&2\nexit 1\n")
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
    return {"PATH": directory + os.pathsep + os.environ["PATH"]}


class MeasureRefreshTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.project = os.path.join(self.root, "p")
        os.makedirs(os.path.join(self.project, "src"))
        self.file = os.path.join("src", "A.java")
        with open(os.path.join(self.project, self.file), "w") as handle:
            handle.write("class A {}\n")

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def test_repeats_and_restores_the_file(self):
        result = measure_refresh(self.project, self.file, "st find probe", fake_st(self.root, True), repeats=3)
        self.assertEqual(result["valid"], 3)
        self.assertEqual(len(result["attempts"]), 3)
        self.assertIsNotNone(result["median_s"])
        with open(os.path.join(self.project, self.file)) as handle:
            self.assertEqual(handle.read(), "class A {}\n")

    def test_no_refreshed_line_means_the_refresh_failed(self):
        result = measure_refresh(self.project, self.file, "st find probe", fake_st(self.root, False), repeats=2)
        self.assertEqual(result["valid"], 0)
        self.assertIsNone(result["median_s"])


if __name__ == "__main__":
    unittest.main()
