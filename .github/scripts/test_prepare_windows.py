import importlib.util
from pathlib import Path
import unittest


folder = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('prepare_windows', folder / 'prepare_windows.py')
windows = importlib.util.module_from_spec(spec)
spec.loader.exec_module(windows)
spec = importlib.util.spec_from_file_location('prepare_release', folder / 'prepare_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class WindowsPreparationTest(unittest.TestCase):
    def test_pinned_backport_precedes_stock_patches_and_preserves_other_stages(self):
        source = (folder.parents[1] / 'Telegram/build/prepare/prepare.py').read_text()
        result = windows.with_windows7(source)
        compile(result, 'prepare.py', 'exec')
        start = source.index("else: # qt > '6'")
        end = source.index("stage('tg_owt'", start)
        self.assertTrue(result.startswith(source[:start]))
        self.assertTrue(result.endswith(source[end:]))
        self.assertIn(windows.PATCH_REVISION, result)
        qt = result[result.index("else: # qt > '6'"):]
        self.assertLess(qt.index('for %%i in (..\\\\..\\\\qt6_windows7'), qt.index('for /r %%i in (..\\\\..\\\\patches\\\\qtbase_%QT%'))
        self.assertIn('depends:qt6_windows7/*.patch', result)
        self.assertIn("qt != '6.11.2'", result)
        optimized = windows.with_windows7(release.release_qt_only(release.without_symbol_tool(source)))
        compile(optimized, 'prepare-release.py', 'exec')
        self.assertIn('SET CONFIGURATIONS=-release', optimized)
        with self.assertRaises(RuntimeError):
            windows.with_windows7(source.replace('    setlocal enabledelayedexpansion\n', ''))


if __name__ == '__main__':
    unittest.main()
