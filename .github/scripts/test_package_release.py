import hashlib
import importlib.util
from pathlib import Path
import tarfile
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('package_release', Path(__file__).with_name('package_release.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
prepare_spec = importlib.util.spec_from_file_location('prepare_release', Path(__file__).with_name('prepare_release.py'))
prepare = importlib.util.module_from_spec(prepare_spec)
prepare_spec.loader.exec_module(prepare)


class PackageReleaseTest(unittest.TestCase):
    def test_archives_preserve_files_and_verify_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for platform, names in (('linux', ('Telegram', 'Updater')), ('windows', ('Telegram.exe', 'Updater.exe'))):
                source, output = root / platform, root / (platform + '-out')
                source.mkdir()
                for index, name in enumerate(names):
                    path = source / name
                    path.write_bytes(bytes([index + 1]) * 1024)
                    path.chmod(0o755)
                archive = module.package(platform, source, output, platform)
                checksum, filename = (output / (archive.name + '.sha256')).read_text().split()
                self.assertEqual(filename, archive.name)
                self.assertEqual(checksum, hashlib.sha256(archive.read_bytes()).hexdigest())
                if platform == 'linux':
                    with tarfile.open(archive, 'r:xz') as bundle:
                        self.assertEqual(set(bundle.getnames()), set(names))
                        for name in names:
                            self.assertEqual(bundle.extractfile(name).read(), (source / name).read_bytes())
                            self.assertTrue(bundle.getmember(name).mode & 0o111)
                else:
                    with zipfile.ZipFile(archive) as bundle:
                        self.assertEqual(set(bundle.namelist()), set(names))
                        for name in names:
                            self.assertEqual(bundle.read(name), (source / name).read_bytes())

    def test_missing_or_empty_binary_fails_without_publishing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'Telegram').write_bytes(b'')
            with self.assertRaises(ValueError):
                module.package('linux', root, root / 'out', 'invalid')
            self.assertFalse((root / 'out').exists())

    def test_windows_preparation_preserves_release_crash_handler(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'Telegram/build/prepare/prepare.py').read_text(encoding='utf-8')
        prepared = prepare.without_symbol_tool(source)
        self.assertNotIn('msbuild -m dump_syms.vcxproj', prepared)
        self.assertIn('ninja -C out/Release%FolderPostfix% common crash_generation_client exception_handler', prepared)
        with self.assertRaises(RuntimeError):
            prepare.without_symbol_tool(prepared)

    def test_windows_qt_preparation_builds_only_optimized_configuration(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'Telegram/build/prepare/prepare.py').read_text(encoding='utf-8')
        prepared = prepare.release_qt_only(source)
        qt = prepared[prepared.index("else: # qt > '6'"):prepared.index("stage('tg_owt'")]
        self.assertIn('SET CONFIGURATIONS=-release', qt)
        self.assertIn('cmake --build . --config Release', qt)
        self.assertNotIn('cmake --build . --config Debug', qt)
        self.assertNotIn('        -force-debug-info ^', qt)
        with self.assertRaises(RuntimeError):
            prepare.release_qt_only(prepared)


if __name__ == '__main__':
    unittest.main()
