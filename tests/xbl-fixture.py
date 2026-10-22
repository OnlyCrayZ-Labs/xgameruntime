#!/usr/bin/env python3
# MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Synthetic title profile/token IPC fixtures. Never reads the keychain or starts a game.

Usage: xbl-fixture.py ARCH WINE_LOADER XGAMERUNTIME_DLL XBL_TESTS_EXE
The test executable runs from a temporary directory beside a synthetic
MicrosoftGame.Config, as a title executable would.
"""
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

MAGIC = 0x58445355
APP = b'23cf167f-8716-4d4c-8dc1-20611e03a087'
TITLE = 0x604cc91b
arch, loader, dll, exe = sys.argv[1:5]
server = str(pathlib.Path(loader).parent / 'server/wineserver') if 'obj-wine' in loader else str(pathlib.Path(loader).parent / 'wineserver')

def s(data):
    return struct.pack('<I', len(data)) + data

def read_exact(conn, n):
    data = b''
    while len(data) < n:
        part = conn.recv(n - len(data))
        if not part:
            raise EOFError
        data += part
    return data

def strings(data, count):
    out = []
    for _ in range(count):
        n, = struct.unpack_from('<I', data)
        out.append(data[4:4 + n])
        data = data[4 + n:]
    return out, data

# case: (mode, profile reply, token reply for Xbox Live URLs[, security reply for pinned URLs])
profile_ok = (0, struct.pack('<Q', 123) + s(b'Classic') + s(b'Modern') + s(b'1234') + s(b'Modern#1234') + s(b'185 254 '))
token_ok = (0, s(b'XBL3.0 x=hash;tok') + s(b'sig=='))
pins = (struct.pack('<II', 0x2800, 2) + struct.pack('<I', 1) + s(bytes.fromhex('54D9D20239080C32316ED9FF980A48988F4ADF2D'))
        + struct.pack('<I', 0) + s(bytes.fromhex('D5A9ACDB80066D0E67FF65A939BBBC952F8ED171')))
security_ok = (0, pins)
FAR = 253402300799
def item(product, sku, kind, status, start=1700000000, end=FAR, trial=0, secs=0, offer=b'', dev=b''):
    return (s(product) + s(sku) + s(kind) + s(status) + struct.pack('<qqqIII', 1700000000, start, end, 1, trial, secs)
            + s(offer) + s(dev))
# Two active game SKUs (lowest wins), one revoked, one expired, one other product.
license_items = [item(b'9P38D19T7LRV', b'0017', b'Game', b'Active'), item(b'9P38D19T7LRV', b'0010', b'Game', b'Active'),
                 item(b'9P38D19T7LRV', b'0001', b'Game', b'Revoked'), item(b'9P38D19T7LRV', b'0002', b'Game', b'Active', end=1000),
                 item(b'9OTHER', b'0010', b'Game', b'Active')]
collections_ok = (0, struct.pack('<I', len(license_items)) + b''.join(license_items))
products = [(b'9N7J4DKK7V15', b'Premium Deluxe Edition', b'Bundle', b'en-us', b'Game', 1, [(b'0010', b'Premium', item(b'9N7J4DKK7V15', b'0010', b'Game', b'Active'))]),
            (b'9ADDON000001', b'Aircraft Pack', b'Add-on', b'en-us', b'Durable', 1, [(b'0010', b'Pack', item(b'9ADDON000001', b'0010', b'Durable', b'Active', trial=1, secs=3600, offer=b'pack1', dev=b'dev1'))]),
            (b'9ADDON000002', b'Livery', b'', b'en-us', b'Durable', 0, [(b'0010', b'', item(b'9ADDON000002', b'0010', b'Durable', b'Active'))])]
def product(store, title, desc, lang, kind, dl, skus):
    return (s(store) + s(title) + s(desc) + s(lang) + s(kind) + struct.pack('<II', dl, len(skus))
            + b''.join(s(sku) + s(stitle) + it for sku, stitle, it in skus))
products_ok = (0, struct.pack('<I', len(products)) + b''.join(product(*p) for p in products))
license_token_ok = (0, s(b'eyJsaWNlbnNl.synthetic.token'))
update_current = (0, struct.pack('<I', 0) + s(b'1.9.18.0') + s(b'2ef370ab-f18a-4154-8808-50e386d68eba'))
update_available = (0, struct.pack('<I', 1) + s(b'1.9.19.0') + s(b'2ef370ab-f18a-4154-8808-50e386d68eba'))
cases = {
    'success': ('success', profile_ok, token_ok),
    'profile-service-error': ('profile-fail', (0x89245111, b''), token_ok),
    'profile-other-account': ('profile-fail', (0, struct.pack('<Q', 999) + profile_ok[1][8:]), token_ok),
    'profile-malformed': ('profile-fail', (0, profile_ok[1][:-3]), token_ok),
    'profile-bad-privilege': ('profile-fail', (0, profile_ok[1][:-12] + s(b'185 x')), token_ok),
    'token-service-error': ('token-fail', profile_ok, (0x89245111, b'')),
    'token-malformed': ('token-fail', profile_ok, (0, s(b'XBL3.0 x=hash;tok') + b'\x05\x00')),
    'token-embedded-nul': ('token-fail', profile_ok, (0, s(b'XBL3.0\x00') + s(b''))),
    'security-service-error': ('security-fail', profile_ok, token_ok, (0x89245111, b'')),
    'security-truncated': ('security-fail', profile_ok, token_ok, (0, pins[:-1])),
    'security-too-many': ('security-fail', profile_ok, token_ok, (0, struct.pack('<II', 0x2800, 17))),
    'security-bad-type': ('security-fail', profile_ok, token_ok, (0, struct.pack('<II', 0x2800, 1) + struct.pack('<I', 7) + s(b'x' * 20))),
}
cases.update({
    'package-update-available': ('package-update', profile_ok, token_ok, security_ok, collections_ok, products_ok, license_token_ok, update_available),
    'package-update-error': ('package-update-fail', profile_ok, token_ok, security_ok, collections_ok, products_ok, license_token_ok, (0x89245111, b'')),
    'license-token-error': ('license-token-fail', profile_ok, token_ok, security_ok, collections_ok, products_ok, (0x89245111, b'')),
    'license-token-empty': ('license-token-fail', profile_ok, token_ok, security_ok, collections_ok, products_ok, (0, s(b''))),
    'store-collections-error': ('store-fail', profile_ok, token_ok, security_ok, (0x89245111, b''), (0x89245111, b'')),
    'store-collections-truncated': ('store-fail', profile_ok, token_ok, security_ok, (0, collections_ok[1][:-2]), (0, products_ok[1][:-2])),
})
for name, case in cases.items():
    mode, profile_reply, token_reply = case[:3]
    security_reply = case[3] if len(case) > 3 else security_ok
    collections_reply = case[4] if len(case) > 4 else collections_ok
    products_reply = case[5] if len(case) > 5 else products_ok
    license_token_reply = case[6] if len(case) > 6 else license_token_ok
    update_reply = case[7] if len(case) > 7 else update_current
    with tempfile.TemporaryDirectory(prefix='xodus-xbl-fixture-') as directory:
        directory = pathlib.Path(directory)
        title_dir = directory / 'title'
        title_dir.mkdir()
        shutil.copy(exe, title_dir / 'xbl-tests.exe')
        (title_dir / 'MicrosoftGame.Config').write_text(
            '<Game configVersion="1"><Identity Name="Synthetic" Publisher="CN=Synthetic" Version="1.9.18.0"/>'
            f'<TitleId>{TITLE:x}</TitleId><MSAAppId>{APP.decode()}</MSAAppId><StoreId>9P38D19T7LRV</StoreId></Game>\n')
        path = str(directory / 'service.sock')
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(path)
        os.chmod(path, 0o600)
        listener.listen()
        listener.settimeout(0.2)
        stop = threading.Event()
        errors, seen = [], []
        def serve():
            while not stop.is_set():
                try:
                    conn, _ = listener.accept()
                except TimeoutError:
                    continue
                with conn:
                    try:
                        magic, version = struct.unpack('<II', read_exact(conn, 8))
                        if magic != MAGIC:
                            errors.append('bad magic'); continue
                        if version == 1:
                            conn.sendall(struct.pack('<IIIIQq', MAGIC, 1, 0, 0, 123, int(time.time()) + 300))
                            continue
                        if version != 3:
                            errors.append(f'unexpected version {version}'); continue
                        length, = struct.unpack('<I', read_exact(conn, 4))
                        payload = read_exact(conn, length)
                        op, = struct.unpack_from('<I', payload)
                        (app,), rest = strings(payload[4:], 1)
                        title, = struct.unpack_from('<I', rest)
                        rest = rest[4:]
                        if app != APP or title != TITLE:
                            errors.append('wrong title identity'); continue
                        if op == 1:
                            if rest:
                                errors.append('profile trailing data')
                            hr, body = profile_reply
                        elif op == 2:
                            options, = struct.unpack_from('<I', rest)
                            (method, url, body_bytes), rest = strings(rest[4:], 3)
                            if rest:
                                errors.append('token trailing data')
                            seen.append((options, method, url, body_bytes))
                            hr, body = token_reply if b'xboxlive.com' in url else (0, s(b'') + s(b''))
                        elif op == 3:
                            (url,), rest = strings(rest, 1)
                            if rest:
                                errors.append('security trailing data')
                            hr, body = security_reply if b'settings.xboxlive.com' in url else (0, struct.pack('<II', 0x2800, 0))
                        elif op == 4:
                            count, = struct.unpack_from('<I', rest)
                            ids, rest = strings(rest[4:], count)
                            kinds, valid = struct.unpack('<II', rest)
                            if (ids, kinds, valid) != ([b'9P38D19T7LRV'], 4, 0):
                                errors.append(f'unexpected collections query {(ids, kinds, valid)!r}')
                            hr, body = collections_reply
                        elif op == 5:
                            (store,), rest = strings(rest, 1)
                            if (store, rest) != (b'9P38D19T7LRV', struct.pack('<I', 6)):
                                errors.append(f'unexpected products query {(store, rest)!r}')
                            hr, body = products_reply
                        elif op == 6:
                            (store,), rest = strings(rest, 1)
                            count, = struct.unpack_from('<I', rest)
                            ids, rest = strings(rest[4:], count)
                            (developer,), rest = strings(rest, 1)
                            if (store, ids, developer, rest) != (b'9P38D19T7LRV', [b'9ADDON000001', b'9ADDON000002'], b'nonce-42', b''):
                                errors.append(f'unexpected license token query {(store, ids, developer)!r}')
                            hr, body = license_token_reply
                        elif op == 7:
                            (store, version), rest = strings(rest, 2)
                            if (store, version, rest) != (b'9P38D19T7LRV', b'1.9.18.0', b''):
                                errors.append(f'unexpected package update query {(store, version)!r}')
                            hr, body = update_reply
                        else:
                            errors.append(f'unexpected op {op}'); continue
                        reply = struct.pack('<IIII', MAGIC, 3, hr, len(body)) + body
                        # Exercise partial reads.
                        for i in range(0, len(reply), 7):
                            conn.sendall(reply[i:i + 7])
                    except (EOFError, OSError, struct.error) as err:
                        errors.append(f'fixture error {err!r}')
        thread = threading.Thread(target=serve)
        thread.start()
        env = dict(os.environ, WINEPREFIX=str(directory / 'prefix'), WINEDEBUG='-all',
                   WINEDLLOVERRIDES='mscoree,mshtml=;xgameruntime=b', XODUS_SOCKET_PATH=path)
        try:
            proc = subprocess.run([loader, str(title_dir / 'xbl-tests.exe'), dll, mode],
                                  env=env, text=True, capture_output=True, timeout=120)
        finally:
            subprocess.run([server, '-k'], env=env, check=False)
            stop.set()
            thread.join()
            listener.close()
        # Profile failures leave identity and tokens usable.
        if True:
            expected = [(1, b'POST', b'https://title.xboxlive.com/v1/x?y=1', b'{"x":1}'),
                        (0, b'GET', b'https://example.com/', b'')]
            if mode == 'token-fail':
                expected = expected[:1]
            if seen != expected:
                errors.append(f'unexpected token requests {seen!r}')
        print(f'{arch} {name}: {proc.stdout.strip()}', flush=True)
        if proc.returncode or errors:
            print(proc.stderr[-2000:], errors)
            sys.exit(1)
print(f'{arch}: all {len(cases)} synthetic title IPC cases passed')
