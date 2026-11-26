#!/usr/bin/env python3
"""
mkhunk.py <in.o> <out> : turn a relocatable m68k ELF object (from
m68k-linux-gnu-as/-gcc -c, combined with ld -r) into an AmigaOS hunk
executable, for amigafmt's test programs.

Executable sections go into one CODE hunk (in input order, so the start
code must come first), other allocated PROGBITS into DATA, NOBITS into
BSS.  R_68K_32 becomes RELOC32; PC-relative references are resolved when
both ends are in the same hunk and refused otherwise; undefined symbols
are refused.
"""
import struct
import sys

R_68K_32, R_68K_PC32, R_68K_PC16, R_68K_PC8 = 1, 4, 5, 6
SHT_PROGBITS, SHT_SYMTAB, SHT_RELA, SHT_NOBITS, SHT_REL = 1, 2, 4, 8, 9
SHF_ALLOC, SHF_EXECINSTR = 2, 4
CODE, DATA, BSS = 0, 1, 2


def die(msg):
    sys.exit(f'mkhunk: {msg}')


def main(src, dst):
    elf = open(src, 'rb').read()
    if elf[:4] != b'\x7fELF' or elf[4] != 1 or elf[5] != 2:
        die('not a 32-bit big-endian ELF')
    (e_type, e_machine, _, _, _, e_shoff, _, _, _, _, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack('>HHIIIIIHHHHHH', elf[16:52])
    if e_type != 1 or e_machine != 4:
        die('not a relocatable m68k object')
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        secs.append(struct.unpack('>IIIIIIIIII', elf[o:o + 40]))
    shstr = secs[e_shstrndx]

    def name(off, tab=shstr):
        s = elf[tab[4] + off:]
        return s[:s.index(b'\0')].decode()

    # place allocated sections into hunks
    hunks = [bytearray(), bytearray(), 0]
    where = {}
    for i, s in enumerate(secs):
        sh_name, sh_type, sh_flags, _, sh_off, sh_size, _, _, align, _ = s
        if not sh_flags & SHF_ALLOC or sh_size == 0:
            continue
        align = max(align, 2)
        if sh_type == SHT_NOBITS:
            hunks[BSS] = (hunks[BSS] + align - 1) // align * align
            where[i] = (BSS, hunks[BSS])
            hunks[BSS] += sh_size
            continue
        h = CODE if sh_flags & SHF_EXECINSTR else DATA
        buf = hunks[h]
        while len(buf) % align:
            buf.append(0)
        where[i] = (h, len(buf))
        buf += elf[sh_off:sh_off + sh_size]

    # symbols
    symtab = next(s for s in secs if s[1] == SHT_SYMTAB)
    strtab = secs[symtab[6]]
    syms = []
    for o in range(symtab[4], symtab[4] + symtab[5], 16):
        st_name, st_value, st_size, st_info, st_other, st_shndx = \
            struct.unpack('>IIIBBH', elf[o:o + 16])
        syms.append((name(st_name, strtab), st_value, st_shndx))

    relocs = {CODE: {}, DATA: {}}
    for s in secs:
        if s[1] == SHT_REL:
            die('REL relocations are not supported')
        if s[1] != SHT_RELA or s[7] not in where:
            continue
        h, base = where[s[7]]
        buf = hunks[h]
        for o in range(s[4], s[4] + s[5], 12):
            r_off, r_info, r_add = struct.unpack('>IIi', elf[o:o + 12])
            sym, rtype = r_info >> 8, r_info & 0xff
            sname, sval, sndx = syms[sym]
            if sndx not in where:
                die(f'undefined or unplaced symbol {sname!r}')
            th, tbase = where[sndx]
            target = tbase + sval + r_add       # offset within hunk th
            pos = base + r_off
            if rtype == R_68K_32:
                struct.pack_into('>I', buf, pos, target & 0xffffffff)
                relocs[h].setdefault(th, []).append(pos)
            elif rtype in (R_68K_PC32, R_68K_PC16, R_68K_PC8):
                if th != h:
                    die(f'PC-relative reference to {sname!r} across hunks')
                v = target - pos
                fmt, lim = {R_68K_PC32: ('>i', 1 << 31),
                            R_68K_PC16: ('>h', 1 << 15),
                            R_68K_PC8: ('>b', 1 << 7)}[rtype]
                if not -lim <= v < lim:
                    die(f'PC-relative reference to {sname!r} out of range')
                struct.pack_into(fmt, buf, pos, v)
            else:
                die(f'relocation type {rtype} not supported')

    present = [h for h in (CODE, DATA, BSS)
               if (hunks[h] if h != BSS else hunks[BSS])]
    if CODE not in present:
        die('no code')
    num = {h: i for i, h in enumerate(present)}

    def longs(n):
        return (n + 3) // 4

    out = bytearray()
    w = lambda *v: out.extend(struct.pack('>' + 'I' * len(v), *v))
    w(0x3f3, 0, len(present), 0, len(present) - 1)
    for h in present:
        w(longs(hunks[h]) if h == BSS else longs(len(hunks[h])))
    for h in present:
        if h == BSS:
            w(0x3eb, longs(hunks[BSS]))
        else:
            data = bytes(hunks[h]) + b'\0' * (-len(hunks[h]) % 4)
            w(0x3e9 if h == CODE else 0x3ea, len(data) // 4)
            out.extend(data)
            if relocs[h]:
                w(0x3ec)
                for th, offs in sorted(relocs[h].items()):
                    w(len(offs), num[th], *sorted(offs))
                w(0)
        w(0x3f2)
    open(dst, 'wb').write(out)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        die('usage: mkhunk.py <in.o> <out>')
    main(*sys.argv[1:])
