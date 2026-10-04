import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('registry_cache', Path(__file__).with_name('registry_cache.py'))
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class RegistryCacheTest(unittest.TestCase):
    def test_snapshot_round_trip_preserves_incremental_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / 'out'
            build.mkdir()
            source = root / 'source.txt'
            source.write_text('original')
            (build / 'build.ninja').write_text('rule copy\n  command = cp $in $out\nbuild result: copy ../source.txt\n')
            subprocess.run(['ninja', '-C', str(build)], check=True, capture_output=True)
            stored = root / 'registry'
            stored.mkdir()
            original_run = subprocess.run

            def registry(command, **kwargs):
                if command[:2] == ['oras', 'resolve']:
                    return subprocess.CompletedProcess(command, 0, 'sha256:' + 'a' * 64, '')
                if command[:2] == ['oras', 'push']:
                    for name in ['snapshot.json', 'snapshot.tar.gz']:
                        shutil.copyfile(Path(kwargs['cwd']) / name, stored / name)
                elif command[:2] == ['oras', 'pull']:
                    for name in ['snapshot.json', 'snapshot.tar.gz']:
                        shutil.copyfile(stored / name, Path(command[-1]) / name)
                else:
                    return original_run(command, **kwargs)
                return subprocess.CompletedProcess(command, 0)

            args = ['cache', 'push', '--reference', 'ghcr.io/example/cache:out-test', '--key', 'test', '--root', str(root), '--paths', 'out']
            with patch.object(cache.subprocess, 'run', side_effect=registry), patch.dict(os.environ, {'GITHUB_REPOSITORY': 'example/repo'}):
                with patch('sys.argv', args):
                    cache.main()
                shutil.rmtree(build)
                args[1] = 'pull'
                with patch('sys.argv', args):
                    cache.main()
                self.assertIn('no work to do', subprocess.check_output(['ninja', '-C', str(build)], text=True))
                source.write_text('changed')
                os.utime(source, (source.stat().st_atime, (build / 'result').stat().st_mtime + 2))
                self.assertNotIn('no work to do', subprocess.check_output(['ninja', '-C', str(build)], text=True))
                self.assertEqual((build / 'result').read_text(), 'changed')
                metadata = json.loads((stored / 'snapshot.json').read_text())
                metadata['key'] = 'incompatible'
                (stored / 'snapshot.json').write_text(json.dumps(metadata))
                with patch('sys.argv', args), self.assertRaisesRegex(ValueError, 'identity'):
                    cache.main()

    def test_rejects_paths_outside_snapshot_and_links(self):
        for name, link in [('out/../../escape', False), ('other/file', False), ('/out/file', False), ('out/link', True)]:
            with self.subTest(name=name):
                data = io.BytesIO()
                with tarfile.open(fileobj=data, mode='w') as archive:
                    member = tarfile.TarInfo(name)
                    if link:
                        member.type = tarfile.SYMTYPE
                        member.linkname = '../../escape'
                    archive.addfile(member)
                data.seek(0)
                with tarfile.open(fileobj=data) as archive, self.assertRaises(ValueError):
                    cache.validate(archive, ['out'])

    def test_missing_registry_snapshot_reports_cache_miss(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'outputs'
            args = ['cache', 'pull', '--reference', 'ghcr.io/example/cache:missing', '--key', 'test', '--root', directory, '--paths', 'out']
            with patch('sys.argv', args), patch.dict(os.environ, {'GITHUB_OUTPUT': str(output)}), patch.object(cache.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'not found')):
                cache.main()
            self.assertEqual(output.read_text(), 'available=false\n')
            self.assertFalse((Path(directory) / 'out').exists())


if __name__ == '__main__':
    unittest.main()
