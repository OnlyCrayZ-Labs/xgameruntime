#!/usr/bin/env python3
# MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build-tree regressions in disposable prefixes; synthetic identity IPC only."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[4]
logs=root/'build/xgameruntime/validation/networking'
logs.mkdir(parents=True,exist_ok=True)
for arch in ('x86_64','i386'):
    with tempfile.TemporaryDirectory(prefix=f'msfs-network-regression-{arch}-') as prefix:
        env=dict(os.environ,WINEPREFIX=prefix,WINEDEBUG='-all',WINEDLLOVERRIDES='mscoree,mshtml=')
        loader=str(root/f'build/build-msfs-xodus/obj-wine-{arch}/wine')
        server=str(root/f'build/build-msfs-xodus/obj-wine-{arch}/server/wineserver')
        try:
            for suite in ('taskqueue','user','networking'):
                result=subprocess.run([loader,str(root/f'build/xgameruntime/{suite}-tests-{arch}.exe'),'xgameruntime.dll'],env=env,text=True,capture_output=True,timeout=120)
                (logs/f'{suite}-{arch}.log').write_text(result.stdout+result.stderr)
                print(arch,suite,result.returncode,result.stdout.strip(),flush=True)
                if result.returncode: raise SystemExit(1)
        finally:
            subprocess.run([server,'-k'],env=env,check=False)
            subprocess.run([server,'-w'],env=env,check=False)
for arch in ('x86_64','i386'):
    result=subprocess.run(['python3',str(root/'wine/dlls/xgameruntime/tests/user-fixture.py'),arch],cwd=root,text=True,capture_output=True,timeout=180)
    (logs/f'user-fixture-{arch}.log').write_text(result.stdout+result.stderr)
    print(result.stdout,flush=True)
    if result.returncode: raise SystemExit(result.returncode)
