import os
import tempfile
import time
import unittest

from stbench.build import build_fingerprint, changed_files, missing_neighbours


def touch(path, text="x"):
    with open(path, "w") as handle:
        handle.write(text)
    os.chmod(path, 0o755)


class BuildTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.st = os.path.join(self.dir, "st")
        for name in ["st", "Sourcetrail", "sourcetrail_indexer"]:
            touch(os.path.join(self.dir, name))

    def test_missing_neighbours_names_what_refresh_needs(self):
        self.assertEqual(missing_neighbours(self.st), [])
        os.remove(os.path.join(self.dir, "Sourcetrail"))
        self.assertEqual(missing_neighbours(self.st), ["Sourcetrail"])

    def test_a_rebuilt_neighbour_changes_the_fingerprint(self):
        before = build_fingerprint(self.st)
        self.assertEqual(changed_files(before, build_fingerprint(self.st)), [])
        time.sleep(0.01)
        touch(os.path.join(self.dir, "Sourcetrail"), "rebuilt")
        self.assertEqual(changed_files(before, build_fingerprint(self.st)), ["Sourcetrail"])

    def test_a_symlinked_st_is_fingerprinted_where_it_points(self):
        link = os.path.join(tempfile.mkdtemp(), "st")
        os.symlink(self.st, link)
        self.assertEqual(build_fingerprint(link), build_fingerprint(self.st))


if __name__ == "__main__":
    unittest.main()
