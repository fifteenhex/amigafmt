#!/bin/sh
# register.sh [path to amigafmt] : make the kernel run AmigaOS hunk
# executables (magic 000003F3) through amigafmt.  Needs root and a kernel
# with CONFIG_BINFMT_MISC.  'F' opens the interpreter now, so it keeps
# working inside chroots and mount namespaces.
#   register.sh -u   removes the handler again
set -e
B=/proc/sys/fs/binfmt_misc
if [ ! -e $B/register ]; then
	modprobe binfmt_misc 2>/dev/null || true
	grep -q binfmt_misc /proc/filesystems || {
		echo "register.sh: this kernel has no binfmt_misc" \
		     "(CONFIG_BINFMT_MISC)" >&2
		exit 1
	}
	mount -t binfmt_misc binfmt_misc $B
fi
if [ "${1:-}" = -u ]; then
	[ -e $B/amigahunk ] && echo -1 > $B/amigahunk
	exit 0
fi
I=$(readlink -f "${1:-/usr/local/bin/amigafmt}")
[ -x "$I" ] || { echo "register.sh: $I is not executable" >&2; exit 1; }
[ -e $B/amigahunk ] && echo -1 > $B/amigahunk
printf '%s' ":amigahunk:M::\\x00\\x00\\x03\\xf3::$I:F" > $B/register
cat $B/amigahunk
