#!/usr/bin/env python3
"""fdcheck.py <ndk fd dir>: check every hand-written and generated vector
table in src/*.c (struct fdent) against the NDK's .fd files: offset and
registers per function name.  Names the NDK marks private are skipped.

  tools/fdcheck.py '/path/to/NDK_3.1/Includes&Libs/fd'
"""
import glob, os, re, sys

def parse(path):
    d, bias = {}, 30
    for line in open(path, errors='replace'):
        line = line.strip()
        if line.startswith('##bias'):
            bias = int(line.split()[1])
            continue
        if not line or line.startswith('*') or line.startswith('##'):
            continue
        m = re.match(r'(\w+)\((.*?)\)\((.*?)\)', line)
        if m:
            d.setdefault(m.group(1), []).append((bias, m.group(3).replace(',', '/').lower()))
        bias += 6
    return d

ndk = {}
for f in glob.glob(os.path.join(sys.argv[1], '*.fd')):
    for k, v in parse(f).items():
        ndk.setdefault(k, []).extend(v)
checked = bad = 0
for src in sorted(glob.glob(os.path.join(os.path.dirname(__file__), '..', 'src', '*.c'))):
    s = open(src).read()
    for tm in re.finditer(r'struct fdent (\w+)\[\]\s*=\s*\{', s):
        body = s[tm.end():s.index('};', tm.end())]
        for e in re.finditer(r'\{\s*(\d+)\s*,\s*"(\w+)"\s*,\s*"([^"]*)"', body):
            off, name, regs = int(e.group(1)), e.group(2), e.group(3).lower()
            if name not in ndk or 'Private' in name:
                continue
            if (name, off) in (('BeginIO', 30), ('AbortIO', 36)):
                continue    # a device's own vectors, not exec's functions
            checked += 1
            if (off, regs) not in ndk[name]:
                bad += 1
                print(f'{os.path.basename(src)} {tm.group(1)} {name}: -{off} {regs}, NDK {ndk[name]}')
print(f'{checked} entries checked, {bad} mismatches')
sys.exit(bad != 0)
