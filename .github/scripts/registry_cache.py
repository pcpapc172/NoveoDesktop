"""Durable CI snapshots in an OCI registry, independent of Actions cache quotas."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import tempfile


def output(available):
    if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(summary, 'a', encoding='utf-8') as stream:
            stream.write(f'Registry snapshot: {"restored" if available else "not available (cold/fallback build)"}.\n')
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
            stream.write(f'available={str(available).lower()}\n')


def validate_members(members, paths):
    for member in members:
        name = PurePosixPath(member.name)
        if name.is_absolute() or '..' in name.parts or not any(
                name == PurePosixPath(path) or PurePosixPath(path) in name.parents
                for path in paths):
            raise ValueError(f'Unexpected snapshot path: {member.name}')
        if member.issym() or member.islnk():
            raise ValueError(f'Links are not allowed in snapshots: {member.name}')
        yield member


def validate(archive, paths):
    for _ in validate_members(archive, paths):
        pass


@contextmanager
def open_archive(path, operation, compression):
    if compression == 'gzip':
        options = {'compresslevel': 1, 'dereference': True} if operation == 'w' else {}
        with tarfile.open(path, operation + ':gz', **options) as archive:
            yield archive
        return
    command = ['zstd', '-T0', '-1', '-q', '-o', str(path)] if operation == 'w' else ['zstd', '-dc', str(path)]
    process = subprocess.Popen(command, stdin=subprocess.PIPE if operation == 'w' else None,
                               stdout=subprocess.PIPE if operation == 'r' else None)
    pipe = process.stdin if operation == 'w' else process.stdout
    try:
        with tarfile.open(fileobj=pipe, mode=operation + '|', dereference=True) as archive:
            yield archive
        pipe.close()
        if process.wait():
            raise RuntimeError('Snapshot compression/decompression failed')
    finally:
        pipe.close()
        if process.poll() is None:
            process.terminate()
        process.wait()


def add_snapshot(archive, source, name):
    if source.is_symlink() and not source.exists():
        print(f'::notice::Skipping dangling dependency link: {name}')
        return
    archive.add(source, arcname=name, recursive=False)
    if source.is_dir():
        for child in sorted(source.iterdir()):
            add_snapshot(archive, child, f'{name}/{child.name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=['pull', 'push'])
    parser.add_argument('--reference', required=True)
    parser.add_argument('--key', required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--paths', nargs='+', required=True)
    parser.add_argument('--compression', choices=['gzip', 'zstd'], default='gzip')
    args = parser.parse_args()
    metadata = {'version': 1, 'key': args.key, 'paths': args.paths}
    if args.compression == 'zstd':
        metadata.update(version=2, compression='zstd')
    with tempfile.TemporaryDirectory(prefix='noveo-cache-') as directory:
        temporary = Path(directory)
        archive_name = 'snapshot.tar.zst' if args.compression == 'zstd' else 'snapshot.tar.gz'
        archive_path = temporary / archive_name
        if args.operation == 'pull':
            result = subprocess.run(['oras', 'resolve', args.reference],
                                    capture_output=True, text=True)
            if result.returncode:
                print(f'::notice::Registry snapshot unavailable: {result.stderr.strip()}')
                output(False)
                return
            digest = result.stdout.strip()
            if not digest.startswith('sha256:') or len(digest) != 71:
                raise ValueError('Registry returned an invalid digest')
            reference = args.reference.rsplit(':', 1)[0] + '@' + digest
            subprocess.run(['oras', 'pull', reference, '-o', directory], check=True)
            if json.loads((temporary / 'snapshot.json').read_text()) != metadata:
                raise ValueError('Snapshot identity does not match this build')
            with open_archive(archive_path, 'r', args.compression) as archive:
                for member in validate_members(archive, args.paths):
                    archive.extract(member, args.root, filter='data')
            print(f'Restored registry snapshot: {reference}')
            output(True)
        else:
            (temporary / 'snapshot.json').write_text(json.dumps(metadata))
            with open_archive(archive_path, 'w', args.compression) as archive:
                for path in args.paths:
                    source = args.root / path
                    if not source.is_dir():
                        raise ValueError(f'Snapshot directory missing: {source}')
                    add_snapshot(archive, source, path)
            with open_archive(archive_path, 'r', args.compression) as archive:
                validate(archive, args.paths)
            subprocess.run(['oras', 'push', args.reference,
                            '--artifact-type', 'application/vnd.noveo.build-cache.v1',
                            '--annotation', 'org.opencontainers.image.source=https://github.com/'
                            + os.environ['GITHUB_REPOSITORY'],
                            'snapshot.json:application/json',
                            archive_name + (':application/zstd' if args.compression == 'zstd' else ':application/gzip')], cwd=directory, check=True)


if __name__ == '__main__':
    main()
