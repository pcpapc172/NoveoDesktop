import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

spec = importlib.util.spec_from_file_location("build_cache", Path(__file__).with_name("build_cache.py"))
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class BuildCacheTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.manifest = self.root / "out/timestamps.json"

    def track(self, name, content):
        path = self.root / name
        path.write_text(content)
        subprocess.run(["git", "add", name], cwd=self.root, check=True)
        return path

    def test_changed_and_new_files_keep_fresh_timestamps(self):
        same = self.track("same.cpp", "old\n")
        changed = self.track("changed.cpp", "old\n")
        before = same.stat().st_mtime_ns
        cache.save(self.root, self.manifest)
        fresh = before + 10_000_000_000
        os.utime(same, ns=(fresh, fresh))
        changed.write_text("new\n")  # Same size; content must still invalidate it.
        os.utime(changed, ns=(fresh, fresh))
        added = self.track("new.cpp", "added\n")
        added_time = added.stat().st_mtime_ns
        cache.restore(self.root, self.manifest)
        self.assertEqual(same.stat().st_mtime_ns, before)
        self.assertEqual(changed.stat().st_mtime_ns, fresh)
        self.assertEqual(added.stat().st_mtime_ns, added_time)

    def test_missing_manifest_is_a_cold_build(self):
        path = self.track("file.cpp", "data")
        before = path.stat().st_mtime_ns
        cache.restore(self.root, self.manifest)
        self.assertEqual(path.stat().st_mtime_ns, before)

    def test_ninja_reuses_unchanged_output_and_rebuilds_edits(self):
        source = self.track("source.txt", "first")
        build = self.root / "out"
        build.mkdir()
        (build / "build.ninja").write_text("rule copy\n  command = cp $in $out\nbuild result: copy ../source.txt\n")
        def ninja():
            return subprocess.check_output(["ninja", "-C", str(build)], text=True)
        ninja()
        cache.save(self.root, self.manifest)
        fresh = time.time_ns() + 1_000_000_000
        os.utime(source, ns=(fresh, fresh))
        cache.restore(self.root, self.manifest)
        self.assertIn("no work to do", ninja())
        source.write_text("second")
        os.utime(source, ns=(fresh, fresh))
        cache.restore(self.root, self.manifest)
        self.assertNotIn("no work to do", ninja())
        self.assertEqual((build / "result").read_text(), "second")


if __name__ == "__main__":
    unittest.main()
