#!/usr/bin/env python3
"""Record local package identity; this is provenance, not qualification."""
import argparse
import hashlib
import json
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    status = git('status', '--porcelain', '--untracked-files=all').decode()
    inputs = ['tools/gnmic/go.mod', 'tools/gnmic/go.sum',
              'danos-build/Dockerfile.mgrd', 'CMakeLists.txt']
    manifest = {
        'schema_version': 1,
        'kind': 'package-provenance',
        'qualification': 'NOT_ASSERTED',
        'utc': datetime.now(timezone.utc).isoformat(),
        'git_commit': git('rev-parse', 'HEAD').decode().strip(),
        'source_dirty': bool(status),
        'source_status': status,
        'tracked_diff_sha256': hashlib.sha256(git('diff', 'HEAD', '--binary')).hexdigest(),
        'untracked_sha256': {
            name: digest(ROOT / name)
            for name in git('ls-files', '--others', '--exclude-standard', '-z').decode().split('\0')
            if name and (ROOT / name).is_file()
        },
        'inputs_sha256': {name: digest(ROOT / name) for name in inputs},
        'platform': platform.platform(),
        'artifact': {'name': args.artifact.name,
                     'size': args.artifact.stat().st_size,
                     'sha256': digest(args.artifact)},
        'limitations': ['Base image and apt repositories are not snapshot-pinned.',
                        'A package manifest does not establish release eligibility.',
                        'Source identity is sampled at packaging, not attested at compilation.'],
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
