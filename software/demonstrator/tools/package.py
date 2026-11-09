#!/usr/bin/env python3
"""Package built Java/Python demonstrators and offline UI for copying to a Pi."""
from pathlib import Path
import io
import tarfile
import argparse

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT/'software/demonstrator'
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path,default=APP/'build/keel-demonstrator.tar.gz')
args=parser.parse_args()
for required in (APP/'java/build/keel-demo.jar',APP/'web/dist/index.html',APP/'web/dist/assets/robot.glb'):
    if not required.is_file():
        parser.error('Missing build output: '+str(required))
paths=set()
for rel in ('software/demonstrator/python','software/demonstrator/java/src','software/demonstrator/web/src',
            'software/demonstrator/images',
            'software/demonstrator/web/tests','software/demonstrator/web/dist','software/demonstrator/tools',
            'software/bindings/python/src','software/bindings/java/src/main/java'):
    paths.update(p for p in (ROOT/rel).rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix!='.pyc')
for rel in ('LICENSE','LICENSE-HARDWARE','THIRD_PARTY_NOTICES.md','software/demonstrator/README.md',
            'software/demonstrator/API.md','software/demonstrator/manage.py','software/demonstrator/java/build.py',
            'software/demonstrator/java/README.md','software/demonstrator/java/build/keel-demo.jar',
            'software/demonstrator/web/package.json','software/demonstrator/web/package-lock.json',
            'software/demonstrator/web/vite.config.js','software/demonstrator/web/index.html'):
    p=ROOT/rel
    if p.is_file():paths.add(p)
# Public build assets are also included as source assets so the copied UI can be rebuilt.
paths.update(p for p in (APP/'web/public').rglob('*') if p.is_file())
args.output.parent.mkdir(parents=True,exist_ok=True)
with tarfile.open(args.output,'w:gz') as archive:
    for p in sorted(paths):
        archive.add(p,arcname=p.relative_to(ROOT).as_posix(),recursive=False)
    wrapper=b'#!/bin/sh\nexec python3 "$(dirname "$0")/software/demonstrator/manage.py" "$@"\n'
    info=tarfile.TarInfo('demo');info.size=len(wrapper);info.mode=0o755
    archive.addfile(info,io.BytesIO(wrapper))
print(args.output.resolve())
print(f'{len(paths)} files; {args.output.stat().st_size:,} bytes')
