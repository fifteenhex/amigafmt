#!/bin/sh
# tests/csurvey.sh <C: directory> [build dir]: run every AmigaOS C:
# command we have an opinion about once, in a scratch directory, and show
# the first lines of output and the exit code.  The binaries are not in
# the repository (copyright); point it at a copy of an AmigaOS C: drawer.
C=$(cd "$1" && pwd)
O=${2:-/tmp/amigafmt-build}
case $(uname -m) in m68k) RUN="";; *) RUN="qemu-m68k";; esac
A="$RUN $O/amigafmt -u"
W=${TMPDIR:-/tmp}/afsurvey.$$
rm -rf $W; mkdir -p $W/d1; cd $W || exit 1
printf 'zeta\nalpha\nbeta\n' > f1.txt
printf 'hello world\nfoo bar\n' > f2.txt
cp f1.txt d1/
r() {
	n=$1; shift
	[ -f "$C/$n" ] || return
	echo "=== $n $*"
	$A $C/$n "$@" > $W/.out 2>&1 < /dev/null; rc=$?
	head -${L:-6} $W/.out; echo "rc=$rc"
}
r Version; r Version FULL; r Version $C/List; r Date; r Avail; r Eval 6 '*' 7
r Dir; r List; r List d1 ALL; r Info; r Assign; r Status
r Which List; r CPU
r Copy f1.txt f3.txt; r Copy d1 TO d3 ALL; r Join f1.txt f2.txt TO f4.txt
r Type f4.txt; r Type f2.txt HEX; r Sort f1.txt TO f5.txt; r Type f5.txt
r Search '#?.txt' foo; r Search ALL . alpha
r MakeDir d2; r Rename f3.txt d2/x.txt; r Protect f1.txt -w
r Filenote f2.txt hi; r SetDate f2.txt 01-Jan-90; r List f2.txt
r Delete d2 ALL; r Delete d3 ALL QUIET; r MakeLink lnk f2.txt; r Lock SYS:
r Wait 1; r Break 1 C; r ChangeTaskPri 0; r Execute nonesuch
r RequestChoice T B G; r AddBuffers DF0: 1; r Relabel x y; r SetClock LOAD
r lha; r unzip -v; r lspci
cd /; rm -rf $W
