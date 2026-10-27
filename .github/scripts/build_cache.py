"""Keep Ninja incremental across clean checkouts without hiding changed inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def tracked_files(root):
    output = subprocess.check_output(
        ["git", "ls-files", "--recurse-submodules", "-z"], cwd=root
    )
    for name in output.split(b"\0"):
        if name:
            relative = os.fsdecode(name)
            path = root / relative
            if path.is_file() and not path.is_symlink():
                yield relative, path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fingerprint(root):
    result = hashlib.sha256()
    for name in (".ninja_log", ".ninja_deps"):
        path = root / "out" / name
        result.update(path.read_bytes() if path.exists() else b"missing")
    return result.hexdigest()


def save(root, manifest):
    records = {
        name: [digest(path), path.stat().st_mtime_ns]
        for name, path in tracked_files(root)
    }
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(records), encoding="utf-8")
    print(f"Recorded {len(records)} build inputs.")
    before = root / "out/.noveo-before-build"
    changed = not before.exists() or before.read_text() != fingerprint(root)
    if output := os.environ.get("GITHUB_OUTPUT"):
        with open(output, "a", encoding="utf-8") as stream:
            stream.write(f"changed={str(changed).lower()}\n")
    print(f"Build outputs changed: {changed}")


def restore(root, manifest):
    before = root / "out/.noveo-before-build"
    before.parent.mkdir(parents=True, exist_ok=True)
    before.write_text(fingerprint(root))
    if not manifest.exists():
        print("No previous build input timestamps; initializing cache.")
        return
    records = json.loads(manifest.read_text(encoding="utf-8"))
    restored = 0
    for name, path in tracked_files(root):
        record = records.get(name)
        if record and digest(path) == record[0]:
            stat = path.stat()
            os.utime(path, ns=(stat.st_atime_ns, record[1]))
            restored += 1
    print(f"Restored timestamps for {restored} unchanged inputs; changed inputs remain fresh.")
    restore_dependency_times(root)


def restore_dependency_times(root):
    build = (root / 'out').resolve()
    ninja = shutil.which('ninja')
    if not ninja or not (build / '.ninja_deps').exists():
        return
    result = subprocess.run([ninja, '-t', 'deps'], cwd=build,
                            capture_output=True, text=True)
    if result.returncode:
        print('Dependency timestamps unavailable; Ninja will revalidate outputs.')
        return
    restored = 0
    for line in result.stdout.splitlines():
        match = re.fullmatch(r'(.+): #deps \d+, deps mtime (\d+) \([^)]*\)', line)
        if not match:
            continue
        path = build / match[1]
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(build):
            continue
        stamp = int(match[2])
        stat = path.stat()
        if 0 < abs(stat.st_mtime_ns - stamp) <= 1000:
            os.utime(path, ns=(stat.st_atime_ns, stamp))
            restored += 1
    print(f'Restored exact timestamps for {restored} archived dependency outputs.')


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["save", "restore"])
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--manifest", type=Path, default=Path("out/.noveo-input-times.json"))
    args = parser.parse_args()
    root = args.root.resolve()
    manifest = args.manifest if args.manifest.is_absolute() else root / args.manifest
    {"save": save, "restore": restore}[args.mode](root, manifest)
