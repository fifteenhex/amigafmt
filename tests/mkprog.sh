#!/bin/sh
# mkprog.sh <compiler> <mkhunk.py> <output> <source>...: build a test
# program into an AmigaOS hunk executable.  The sources are compiled
# freestanding (no libc: tests/lib.c is the runtime), linked relocatable
# with the start code first, and turned into hunks by tools/mkhunk.py.
# <compiler> may be several words ("ccache m68k-linux-gnu-gcc").
set -e
cc=$1 mkhunk=$2 out=$3; shift 3
objs=
for s in "$@"; do
	o="$out-$(basename "$s").o"
	$cc -m68020 -Os -ffreestanding -fno-builtin -fno-pic -nostdlib \
	    -fno-asynchronous-unwind-tables -fno-stack-protector \
	    -I"$(dirname "$s")" -c -o "$o" "$s"
	objs="$objs $o"
done
$cc -r -nostdlib -o "$out.o" $objs
python3 "$mkhunk" "$out.o" "$out"
