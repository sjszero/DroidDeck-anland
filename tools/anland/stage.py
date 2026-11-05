#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


UPSTREAM = Path(__file__).resolve().parent / 'upstream'


def verify():
    manifest = json.loads((UPSTREAM / 'SOURCE.json').read_text())
    for name, expected in manifest['files'].items():
        path = UPSTREAM / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise SystemExit(f'Anland source checksum mismatch: {name}')
    return manifest


def stage(backend, source):
    manifest = verify()
    source = source.resolve(strict=True)
    parents = {'gamescope': 'src/Backends', 'kwin': 'src/backends'}
    target = source / parents[backend] / 'anland'
    if target.exists():
        raise SystemExit(f'Refusing to replace an existing backend: {target}')
    patch = UPSTREAM / 'patches' / f'{backend}.patch'
    command = ['patch', '--batch', '--forward', '--fuzz=0', '--no-backup-if-mismatch', '-p1', '-i', str(patch)]
    subprocess.run(command + ['--dry-run'], cwd=source, check=True)
    shutil.copytree(UPSTREAM / backend, target)
    for shared in ('common', 'libdisplay_producer'):
        shutil.copytree(UPSTREAM / shared, target / shared)
    subprocess.run(command, cwd=source, check=True)
    print(f'{backend}: Anland {manifest["revision"]}; existing Wayland backend retained')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('backend', choices=['gamescope', 'kwin', 'verify'])
    parser.add_argument('source', nargs='?', type=Path)
    args = parser.parse_args()
    if args.backend == 'verify':
        print(verify()['revision'])
    elif args.source is None:
        parser.error('source directory required')
    else:
        stage(args.backend, args.source)
