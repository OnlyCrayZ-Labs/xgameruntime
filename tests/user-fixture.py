#!/usr/bin/env python3
# MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Synthetic IPC fault fixtures. Never reads the keychain or starts a game."""
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

root = pathlib.Path(__file__).resolve().parents[4]
arch = sys.argv[1] if len(sys.argv) > 1 else 'x86_64'
cases = {
    'success': (0, 0, 123, int(time.time()) + 300, '0'),
    'no-user': (0x89245106, 0, 0, 0, '89245106'),
    'auth-failed': (0x89245111, 0, 0, 0, '89245111'),
    'expired': (0, 0, 123, 1, '89245111'),
    'zero-xuid': (0, 0, 0, int(time.time()) + 300, '89245111'),
    'invalid-age': (0, 99, 123, int(time.time()) + 300, '8007000d'),
    'invalid-version': (0, 0, 123, int(time.time()) + 300, '8007000d'),
    'truncated': (0, 0, 123, int(time.time()) + 300, '89245111'),
}
picker_errors = {'picker-cancel': 0x80004004, 'picker-busy': 0x800700aa,
                 'picker-timeout': 0x800705b4, 'picker-no-ui': 0x80004001}
cases['active-cancel'] = (0, 0, 123, int(time.time()) + 300, '0')
for name in picker_errors:
    cases[name] = (0, 0, 123, int(time.time()) + 300, '0')
for name, (hr, age, xid, expires, expected) in cases.items():
    with tempfile.TemporaryDirectory(prefix='xodus-fixture-') as directory:
        path = str(pathlib.Path(directory) / 'identity.sock')
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(path)
        os.chmod(path, 0o600)
        listener.listen()
        listener.settimeout(0.2)
        stop = threading.Event()
        errors = []
        def serve():
            while not stop.is_set():
                try:
                    conn, _ = listener.accept()
                except TimeoutError:
                    continue
                with conn:
                    request = b''
                    while len(request) < 8:
                        part = conn.recv(8 - len(request))
                        if not part:
                            break
                        request += part
                    if len(request) != 8 or struct.unpack('<II', request) not in [(0x58445355, 1), (0x58445355, 2)]:
                        errors.append('invalid request')
                        continue
                    version = struct.unpack('<II', request)[1]
                    if name == 'active-cancel':
                        pathlib.Path(directory, 'ready').touch()
                        conn.settimeout(5)
                        try:
                            if conn.recv(1) != b'':
                                errors.append('canceled client did not disconnect')
                        except OSError:
                            errors.append('canceled IPC remained open')
                        continue
                    reply_hr = picker_errors.get(name, hr) if version == 2 else hr
                    response = struct.pack('<IIIIQq', 0x58445355, 99 if name == 'invalid-version' else version,
                                           reply_hr, age if not reply_hr else 0,
                                           xid if not reply_hr else 0, expires if not reply_hr else 0)
                    # Exercise partial reads, not just a single write.
                    end = 9 if name == 'truncated' else len(response)
                    for b in response[:end]:
                        conn.sendall(bytes([b]))
        thread = threading.Thread(target=serve)
        thread.start()
        env = dict(os.environ, WINEPREFIX=str(pathlib.Path(directory) / 'prefix'), WINEDEBUG='-all',
                   WINEDLLOVERRIDES='mscoree,mshtml=', XODUS_SOCKET_PATH=path,
                   XODUS_TEST_READY="Z:" + str(pathlib.Path(directory) / "ready").replace("/", "\\"))
        try:
            proc = subprocess.run([str(root / 'build/build-msfs-xodus' / f'obj-wine-{arch}/wine'),
                str(root / f'build/xgameruntime/user-tests-{arch}.exe'), 'xgameruntime.dll', 'cancel-ipc' if name == 'active-cancel' else 'fixture', expected, format(picker_errors.get(name, 0), 'x')],
                env=env, text=True, capture_output=True, timeout=90)
        finally:
            subprocess.run([str(root / 'build/build-msfs-xodus' / f'obj-wine-{arch}/server/wineserver'), '-k'], env=env, check=False)
            subprocess.run([str(root / 'build/build-msfs-xodus' / f'obj-wine-{arch}/server/wineserver'), '-w'], env=env, check=False)
            stop.set()
            thread.join()
            listener.close()
        print(f'{arch} {name}: {proc.stdout.strip()}', flush=True)
        if proc.returncode or errors:
            print(proc.stderr)
            sys.exit(1)
