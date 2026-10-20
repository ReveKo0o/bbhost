#!/usr/bin/env python3
"""Copy a bbhost save folder into shadPS4's portable layout, with the
param.sfo shadPS4 wants, so both can load the same character (side-by-side
runs):

    tools/shadps4_save.py <data>/saves/SPRJ0005 <shadPS4 dir>/user

writes <user>/home/1000/savedata/CUSA00900/SPRJ0005/ (user 1000 is
shadPS4's default). The game's own files are copied as they are; only the
SFO is made here.
"""
import os
import shutil
import struct
import sys

src, user = sys.argv[1], sys.argv[2]
name = os.path.basename(src.rstrip('/'))
dst = os.path.join(user, 'home', '1000', 'savedata', 'CUSA00900', name)
os.makedirs(os.path.join(dst, 'sce_sys'), exist_ok=True)
for f in os.listdir(src):
    if f == 'sce_param.bin':  # bbhost's own record of the save's titles
        continue
    shutil.copy2(os.path.join(src, f), os.path.join(dst, 'sce_sys', f) if f == 'icon0.png' else os.path.join(dst, f))
BINARY, STRING, INT = 0x0004, 0x0204, 0x0404
entries = [  # sorted by key, as the console writes them
    ('ACCOUNT_ID', BINARY, b'\0' * 8, 8),
    ('DETAIL', STRING, b'\0', 1024),
    ('MAINTITLE', STRING, b'Bloodborne\0', 128),
    ('SAVEDATA_BLOCKS', BINARY, struct.pack('<Q', 32768 * 32), 8),
    ('SAVEDATA_DIRECTORY', STRING, name.encode() + b'\0', 32),
    ('SAVEDATA_LIST_PARAM', INT, struct.pack('<I', 0), 4),
    ('SUBTITLE', STRING, b'\0', 128),
    ('TITLE_ID', STRING, b'CUSA00900\0', 16),
]
keys, data, index = b'', b'', b''
for key, fmt, value, size in entries:
    index += struct.pack('<HHIII', len(keys), fmt, len(value), size, len(data))
    keys += key.encode() + b'\0'
    data += value + b'\0' * (size - len(value))
while len(keys) % 4:
    keys += b'\0'
key_table = 20 + len(index)
with open(os.path.join(dst, 'sce_sys', 'param.sfo'), 'wb') as f:
    f.write(struct.pack('<IIIII', 0x46535000, 0x101, key_table, key_table + len(keys), len(entries)) + index + keys + data)
print('save at', dst)
