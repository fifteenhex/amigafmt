#!/usr/bin/env python3
# mkzorro.py <dir>: a board directory laid out as /sys/bus/zorro is, its
# devices/<n> being symbolic links to the board directories, each with a
# config file holding the board's ConfigDev (68 big-endian bytes).
import os
import shutil
import struct
import sys

top = sys.argv[1]
shutil.rmtree(top, ignore_errors=True)
os.makedirs(os.path.join(top, 'devices'))
for n, (manu, prod, typ, flags, addr, size) in enumerate([
        (5080, 148, 0xc1, 0x00, 0x00e90000, 0x10000),
        (2206, 33, 0x80, 0x30, 0x40000000, 0x1000000),
        (2206, 0xa1, 0x85, 0x30, 0x60000000, 0x20000000)]):
    name = '%02d' % n
    os.makedirs(os.path.join(top, 'boards', name))
    cd = bytearray(68)
    struct.pack_into('>BBBBH', cd, 16, typ, prod, flags, 0, manu)
    struct.pack_into('>II', cd, 32, addr, size)
    with open(os.path.join(top, 'boards', name, 'config'), 'wb') as f:
        f.write(cd)
    os.symlink(os.path.join('..', 'boards', name),
               os.path.join(top, 'devices', name))
