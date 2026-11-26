#!/bin/sh
# fdtab.sh <fd2c.py> <output> <fd dir> <library>...: regenerate the vector
# tables from the .fd files; the meson `fdtab` run target.
set -e
fd2c=$1 out=$2 dir=$3; shift 3
[ -n "$dir" ] || { echo "fdtab: configure with -Dfd_dir=<directory of .fd files>" >&2; exit 1; }
fds=
for l in "$@"; do fds="$fds $dir/${l}_lib.fd"; done
python3 "$fd2c" $fds > "$out.tmp" && mv "$out.tmp" "$out"
echo "wrote $out"
