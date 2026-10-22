#!/usr/bin/env python3
# MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
"""Prepare prefix-loadable copies of Wine PE modules; never install them.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import argparse
import hashlib
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('proton_root', type=Path)
args = parser.parse_args()
root = args.proton_root.resolve()
for arch, machine in [('x86_64', 0x8664), ('i386', 0x14c)]:
    source = root / f'build/xgameruntime/lib/wine/{arch}-windows/xgameruntime.dll'
    dest = root / f'build/xgameruntime/prefix/{arch}/xgameruntime.dll'
    data = bytearray(source.read_bytes())
    if data[:2] != b'MZ' or data[64:96] != b'Wine builtin DLL' + bytes(16):
        raise SystemExit(f'{source}: unexpected Wine PE signature')
    pe = struct.unpack_from('<I', data, 60)[0]
    if data[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', data, pe+4)[0] != machine:
        raise SystemExit(f'{source}: unexpected PE machine')
    # This is a DOS-stub marker, outside PE headers/code. Without removing it,
    # ntdll prefers the runtime's installed builtin, even for an absolute path;
    # native-only loading rejects such marked files. Retain the raw build too.
    data[64:96] = bytes(32)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    print(f'{hashlib.sha256(data).hexdigest()}  {dest}')
