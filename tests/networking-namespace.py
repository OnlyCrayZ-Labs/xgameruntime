#!/usr/bin/env python3
# MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Creates a user/network namespace: only manipulate a private dummy network interface.

Uses disposable /tmp prefixes and build-tree DLLs. No Internet access needed.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[4]
if len(sys.argv) == 1:
    raise SystemExit(subprocess.run(['unshare', '-Urn', sys.executable, __file__, '--inside', os.readlink('/proc/self/ns/net')]).returncode)
if len(sys.argv) != 3 or sys.argv[1] != '--inside':
    raise SystemExit('Use this script without arguments')
if os.readlink('/proc/self/ns/net') == sys.argv[2]:
    raise SystemExit('Refusing to change interfaces in the host network namespace')
subprocess.run(['ip', 'link', 'add', 'msfsfixture', 'type', 'dummy'], check=True)
subprocess.run(['ip', 'addr', 'add', '192.0.2.1/24', 'dev', 'msfsfixture'], check=True)
for arch in ('x86_64', 'i386'):
    subprocess.run(['ip', 'link', 'set', 'msfsfixture', 'up'], check=True)
    with tempfile.TemporaryDirectory(prefix=f'msfs-network-{arch}-') as prefix:
        env = dict(os.environ, WINEPREFIX=prefix, WINEDEBUG='-all', WINEDLLOVERRIDES='mscoree,mshtml=')
        # Keep stdout as a pipe so markers trigger changes in the same namespace.
        command = [str(root / f'build/build-msfs-xodus/obj-wine-{arch}/wine'),
                   str(root / f'build/xgameruntime/networking-tests-{arch}.exe'), 'xgameruntime.dll', 'transitions']
        proc = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, text=True)
        try:
            for line in proc.stdout:
                print(f'{arch}: {line.rstrip()}', flush=True)
                if line.strip() == 'NETWORK_READY_DOWN':
                    subprocess.run(['ip', 'link', 'set', 'msfsfixture', 'down'], check=True)
                elif line.strip() == 'NETWORK_READY_UP':
                    subprocess.run(['ip', 'link', 'set', 'msfsfixture', 'up'], check=True)
            if proc.wait(timeout=60):
                raise SystemExit(f'{arch} failed')
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            subprocess.run([str(root / f'build/build-msfs-xodus/obj-wine-{arch}/server/wineserver'), '-k'], env=env, check=False)
            subprocess.run([str(root / f'build/build-msfs-xodus/obj-wine-{arch}/server/wineserver'), '-w'], env=env, check=False)
