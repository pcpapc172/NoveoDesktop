import argparse
import hashlib
import os
from pathlib import Path
import tarfile
import zipfile


def package(platform, source, output, name):
    names = ('Telegram', 'Updater') if platform == 'linux' else ('Telegram.exe', 'Updater.exe')
    files = [source / name for name in names]
    for path in files:
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f'Missing or empty build output: {path}')

    output.mkdir(parents=True, exist_ok=True)
    if platform == 'linux':
        archive = output / f'{name}.tar.xz'
        with tarfile.open(archive, 'w:xz', preset=6) as bundle:
            for path in files:
                bundle.add(path, arcname=path.name)
    else:
        archive = output / f'{name}.zip'
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
            for path in files:
                bundle.write(path, arcname=path.name)

    digest = hashlib.sha256()
    with archive.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    (output / f'{archive.name}.sha256').write_text(
        f'{digest.hexdigest()}  {archive.name}\n', encoding='utf-8')
    archive_size = archive.stat().st_size
    unpacked_size = sum(path.stat().st_size for path in files)
    message = (f'{archive.name}: {archive_size / 1_000_000:.2f} MB download; '
               f'{unpacked_size / 1_000_000:.2f} MB unpacked')
    print(message)
    if archive_size > 100_000_000:
        print(f'::warning::Release archive is above the 100 MB target: {message}')
    if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(summary, 'a', encoding='utf-8') as stream:
            stream.write(f'### Release package\n\n{message}\n\n')
    return archive


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', choices=('linux', 'windows'), required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--name', required=True)
    arguments = parser.parse_args()
    package(arguments.platform, arguments.input, arguments.output, arguments.name)
