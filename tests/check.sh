#!/bin/sh
# check.sh <expected file> <expected rc> <directory> <command...>: run the
# command in the directory and compare its stdout+stderr and exit code with
# the expected file.  Register dumps and hex addresses are normalised.
# Under meson, $MESON_EXE_WRAPPER (qemu-m68k when cross-building) runs the
# command.  UPDATE=1 rewrites the expected file instead of comparing.
set -u
exp=$1 rc=$2 dir=$3; shift 3
out=$(cd "$dir" && ${MESON_EXE_WRAPPER:-} "$@" 2>&1); got=$?
out=$(printf '%s\nrc=%s\n' "$out" "$got" | sed -e '/^  d[0-7]=/d' \
	-e 's/\$[0-9a-f]\{8\}/$X/g')
if [ "${UPDATE:-}" ]; then
	printf '%s\n' "$out" > "$exp"
	exit 0
fi
if [ "$out" = "$(cat "$exp" 2>/dev/null)" ] && [ "$got" = "$rc" ]; then
	exit 0
fi
printf '%s\n' "$out" | diff "$exp" -
echo "exit code $got, expected $rc"
exit 1
