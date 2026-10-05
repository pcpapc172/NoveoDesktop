"""Durable CI snapshots in an OCI registry, independent of Actions cache quotas."""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import tempfile


def output(available):
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
            stream.write(f'available={str(available).lower()}\n')


def validate(archive, paths):
    for member in archive.getmembers():
        name = PurePosixPath(member.name)
        if name.is_absolute() or '..' in name.parts or not any(
                name == PurePosixPath(path) or PurePosixPath(path) in name.parents
                for path in paths):
            raise ValueError(f'Unexpected snapshot path: {member.name}')
        if member.issym() or member.islnk():
            raise ValueError(f'Links are not allowed in Windows snapshots: {member.name}')


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
    args = parser.parse_args()
    metadata = {'version': 1, 'key': args.key, 'paths': args.paths}
    with tempfile.TemporaryDirectory(prefix='noveo-cache-') as directory:
        temporary = Path(directory)
        archive_path = temporary / 'snapshot.tar.gz'
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
            with tarfile.open(archive_path) as archive:
                validate(archive, args.paths)
                archive.extractall(args.root, filter='data')
            output(True)
        else:
            (temporary / 'snapshot.json').write_text(json.dumps(metadata))
            with tarfile.open(archive_path, 'w:gz', compresslevel=1, dereference=True) as archive:
                for path in args.paths:
                    source = args.root / path
                    if not source.is_dir():
                        raise ValueError(f'Snapshot directory missing: {source}')
                    add_snapshot(archive, source, path)
            with tarfile.open(archive_path) as archive:
                validate(archive, args.paths)
            subprocess.run(['oras', 'push', args.reference,
                            '--artifact-type', 'application/vnd.noveo.build-cache.v1',
                            '--annotation', 'org.opencontainers.image.source=https://github.com/'
                            + os.environ['GITHUB_REPOSITORY'],
                            'snapshot.json:application/json',
                            'snapshot.tar.gz:application/gzip'], cwd=directory, check=True)


if __name__ == '__main__':
    main()
