#!/bin/sh
# survey.sh [-v] <SYS: directory> [amigafmt binary]: run the programs of an
# AmigaOS installation through amigafmt and tabulate how each one fares.
#
# The installation is only read: SYS: is assigned to it, and everything the
# commands write goes to a scratch directory (the current directory, T:,
# RAM:, ENV: and ENVARC: are all there).  Nothing from AmigaOS is in this
# repository; point it at a mounted Workbench partition or a copy of the
# Workbench disks.
#
# Each case is a command line, the exit code it should give and a pattern
# its output should match.  The outcome is one of
#   works     the exit code and the output are what AmigaOS gives
#   wrong     it ran to the end but the output or the exit code is wrong
#   unimpl    it called a library function amigafmt does not have
#   crash     it crashed (signal, address error)
#   hang      it did not finish in $SURVEY_TIMEOUT seconds (default 30)
#   gui       it needs wbde (a window, a screen or the topaz font), which
#             the survey does not start
#   missing   the program is not in this installation
# After the cases every other executable in the root of SYS:, C:, System,
# Tools, Tools/Commodities, Utilities, Prefs, Rexxc and WBStartup is run
# once without arguments, with a ^C after $SURVEY_BREAK seconds (10) for
# those that run until they get one, and classified the same way, "ran"
# meaning it finished without one of the failures above.
#
# Output: a Markdown table on stdout, then the counts.  -v also prints the
# first lines of each command's output.  The exit status is 1 when one of
# the cases crashed, hung or met an unimplemented call, so the survey can
# serve as a test; 0 otherwise.
#
# It runs under qemu-m68k on other hosts, and on m68k Linux itself with a
# busybox userland (an Amiga running Linux, with SYS: mounted).
verbose=0
[ "$1" = -v ] && { verbose=1; shift; }
[ -d "${1:-}" ] || { echo "usage: $0 [-v] <SYS: directory> [amigafmt]" >&2; exit 2; }
SYS=$(cd "$1" && pwd)
AF=${2:-amigafmt}
case $AF in */*) AF=$(cd "$(dirname "$AF")" && pwd)/$(basename "$AF");; esac
case $(uname -m) in m68k) RUN=;; *) RUN=qemu-m68k;; esac
T=${SURVEY_TIMEOUT:-30}

# af <seconds> <signal> <arguments...>: amigafmt with the arguments, its
# output in $W/out.  After the seconds it gets the signal: TERM (when the
# signal is empty) and the exit code is 124, or INT, a ^C, which a
# program that runs until it is stopped (a commodity) takes as its cue
# to finish, so its own exit code counts; one that ignores the ^C is
# killed five seconds later and gives 124.  $TENV is added to the
# environment and $TINPUT is the standard input.
# Programs run in a session of their own where setsid is there, so that
# CON: (which is the controlling terminal) falls back to standard output.
# (sh has no local variables: these names are af's own)
SETSID=
command -v setsid > /dev/null && SETSID=setsid
if command -v timeout > /dev/null; then
	af() {
		_secs=$1 _sig=$2; shift 2
		[ -n "$SETSID" ] && set -- $SETSID "$@"
		if [ "$_sig" = INT ]; then
			set -- -k 5 -s INT --preserve-status "$_secs" "$@"
		else
			set -- "$_secs" "$@"
		fi
		env $TENV timeout "$@" > "$W/out" 2>&1 < "${TINPUT:-/dev/null}"
		_rc=$?
		[ $_rc = 137 ] && _rc=124
		return $_rc
	}
else
	# busybox without timeout
	af() {
		_secs=$1 _sig=${2:-TERM}; shift 2
		env $TENV $SETSID "$@" > "$W/out" 2>&1 < "${TINPUT:-/dev/null}" &
		_pid=$!
		( sleep "$_secs"; kill -s "$_sig" $_pid 2>/dev/null
		  [ "$_sig" = INT ] && sleep 5 && kill -s KILL $_pid 2>/dev/null ) &
		_w=$!
		wait $_pid
		_rc=$?
		kill $_w 2>/dev/null
		case "$_sig$_rc" in TERM143|INT137) _rc=124;; esac
		return $_rc
	}
fi
W=$(mktemp -d "${TMPDIR:-/tmp}/afsurvey.XXXXXX") || exit 1
trap 'cd /; rm -rf "$W"' EXIT INT TERM
mkdir -p "$W/run/d1" "$W/t" "$W/ram/Env" "$W/envarc"
cd "$W/run" || exit 1
printf 'zeta\nalpha\nbeta\n' > f1.txt
printf 'echo from iconx\n' > iscript
printf ' q' > keys
printf 'Echo interactive\nEndShell\n' > shellin
printf '/* */\nsay "hello from arexx" 6*7\n' > t.rexx
printf 'hello world\nfoo bar\n' > f2.txt
cp f1.txt d1/
printf '.key name/a\necho "hi <name>"\n' > script1
printf 'echo one\nif warn\n  echo bad\nelse\n  echo two\nendif\nfailat 21\nsearch f1.txt nothing\nif warn\n  echo three\nendif\nskip done\necho bad\nlab done\necho four\n' > script2
export AMIGAFMT_CONF=/dev/null
unset WBDE_SOCKET WBDE_ICON
export WBDE_SOCKET="$W/nowbde"
AFOPT="-a SYS=$SYS -a T=$W/t -a RAM=$W/ram -a ENV=$W/ram/Env -a ENVARC=$W/envarc"

n_works=0 n_wrong=0 n_unimpl=0 n_crash=0 n_hang=0 n_gui=0 n_missing=0 n_ran=0
failed=0
seen=" "
echo "| command | outcome | detail |"
echo "| --- | --- | --- |"

# classify <output file> <rc>: the failures every run can show
classify() {
	if [ "$2" = 124 ]; then echo hang
	elif grep -aq '^amigafmt: .*: unimplemented ' "$1"; then echo unimpl
	elif grep -aq '^amigafmt: .*: no wbde ' "$1"; then echo gui
	elif grep -aEq '^amigafmt: .*: (Segmentation fault|Bus error|Illegal instruction|Floating point|Address error|Wait\(.*nothing can send)' "$1" ||
	     grep -aEq '^amigafmt: .* at \$[0-9a-f]+, address' "$1"; then echo crash
	else echo ok; fi
}

detail() {
	d=$(grep -a -m1 '^amigafmt: .*: \(unimplemented\|no wbde\)' "$1" |
	    sed -e 's/^amigafmt: [^:]*: //' -e 's/ called from.*//')
	[ -n "$d" ] || d=$(LC_ALL=C sed -e "s/$(printf '\033')\\[[0-9;?]*[ ]*[@-~]//g" \
			   -e 's/[^ -~]/ /g' "$1" |
			   grep -av '^[[:space:]]*$' | head -1 | cut -c1-60)
	printf '%s' "$d" | tr '|\t' '/ '
}

row() {
	echo "| \`$1\` | $2 | $3 |"
	eval "n_$2=\$((n_$2 + 1))"
	case "$sweep$2" in crash|hang|unimpl) failed=1;; esac
}

# t <exit code> <pattern> <program> [args...]: one case; the program is a
# path under SYS:, the exit code - for any (where versions of a command
# differ), the pattern an extended regular expression that some line of
# the output must match ('' for any output).  $TENV adds to the
# environment and $TINPUT is the standard input (/dev/null); $TBREAK is
# a number of seconds after which the program gets a ^C (SIGINT), for
# those that run until they get one.
t() {
	rc=$1 pat=$2 prog=$3; shift 3
	label=$(printf '%s ' "$prog" "$@" | sed 's/ $//')
	f=$(ls -d "$SYS"/$prog 2>/dev/null | head -1)
	if [ ! -f "$f" ]; then
		f=$(find "$SYS/$(dirname "$prog")" -maxdepth 1 -iname "$(basename "$prog")" 2>/dev/null | head -1)
	fi
	if [ -z "$f" ]; then
		row "$label" missing ""
		return
	fi
	seen="$seen$f "
	af "${TBREAK:-$T}" "${TBREAK:+INT}" $RUN "$AF" $AFOPT "$f" "$@"
	got=$?
	c=$(classify "$W/out" "$got")
	if [ "$c" = ok ]; then
		if { [ "$rc" = - ] || [ "$got" = "$rc" ]; } &&
		   { [ -z "$pat" ] || grep -aEq -- "$pat" "$W/out"; }; then
			c=works
		else
			c=wrong
		fi
	fi
	row "$label" "$c" "rc $got: $(detail "$W/out")"
	if [ $verbose = 1 ]; then
		head -${SURVEY_LINES:-8} "$W/out" | sed 's/^/    /' >&2
	fi
}

# s <exit code> <pattern> <line>...: lines through amigafmt's shell (-c)
s() {
	rc=$1 pat=$2; shift 2
	label=$(printf '%s / ' "$@" | sed 's| / $||')
	n=$#
	for l; do set -- "$@" -c "$l"; done
	shift $n
	af "$T" "" $RUN "$AF" $AFOPT "$@"
	got=$?
	c=$(classify "$W/out" "$got")
	if [ "$c" = ok ]; then
		if { [ "$rc" = - ] || [ "$got" = "$rc" ]; } &&
		   { [ -z "$pat" ] || grep -aEq -- "$pat" "$W/out"; }; then
			c=works
		else
			c=wrong
		fi
	fi
	row "-c $label" "$c" "rc $got: $(detail "$W/out")"
	[ $verbose = 1 ] && head -${SURVEY_LINES:-8} "$W/out" | sed 's/^/    /' >&2
}

# --- C: ---
t 0 '^Kickstart [0-9]+\.[0-9]+, Workbench [0-9]+\.[0-9]+' C/Version
t 0 '^list [0-9]+\.[0-9]+' C/Version C:List
t 0 '^exec\.library [0-9]+\.[0-9]+' C/Version exec.library
t 0 '[0-9][0-9]-[A-Z][a-z][a-z]-[0-9][0-9]' C/Date
t 0 '^chip +[0-9]+ +[0-9]+' C/Avail
t 0 '^42$' C/Eval 6 '*' 7
t 0 'f1\.txt' C/Dir
t 0 '^f1\.txt +16 ' C/List
t 0 '^1 file' C/List d1 ALL
t 0 'Read/Write' C/Info
t 0 '^SYS \[Mounted\]' C/Assign
t 0 '^Process +1: Loaded as command: Status' C/Status
t 0 'C[:/]List$' C/Which List
t 0 'INTERNAL Echo' C/Which Echo
t - '^System: 680[2346]0' C/CPU
t 0 '' C/Copy f1.txt f3.txt
t 0 'f1\.txt\.\.copied' C/Copy d1 TO d3 ALL
t 0 '' C/Join f1.txt f2.txt TO f4.txt
t 0 '^foo bar$' C/Type f4.txt
t 0 '^0000: 68656C6C' C/Type f2.txt HEX
t 0 '' C/Sort f1.txt TO f5.txt
t 0 '^ +[0-9]+ foo bar' C/Search '#?.txt' foo
t 0 '' C/MakeDir d2
t 0 '' C/Rename f3.txt d2/x.txt
t 0 '' C/Protect f1.txt -w
t 0 '' C/Filenote f2.txt hi
t 0 '' C/SetDate f2.txt 01-Jan-90
t 0 'Deleted' C/Delete d2 ALL
t 0 '' C/MakeLink lnk f2.txt
t 0 '' C/Wait 1
t 0 '' C/Break 1 C
t 0 '' C/ChangeTaskPri 0
t 0 '^hi bob$' C/Execute script1 bob
t 0 '^four$' C/Execute script2
t 10 'Can.t open' C/Execute nonesuch
t 0 '' C/Assign FOO: d1
t 0 '' C/LoadResource
t 20 'failed' C/Lock SYS:
t 20 '' C/Relabel f1.txt x
t 20 'battery' C/SetClock LOAD
t 20 'battery' C/SetClock SAVE
t 0 'not implemented' C/AddBuffers SYS: 10
t 20 '' C/DiskChange DF0:
t 0 '' C/BindDrivers
t 0 '' C/Mount
t 20 '' C/Mount DF9:
t 20 '' C/Install DF0:
t 0 '' C/SetPatch QUIET
t 0 '' C/AddDataTypes
t 0 '' C/AddDataTypes REFRESH
t 10 'required' C/Ed
t - 'Bad args|required' C/Edit
t 5 '' C/RequestChoice T B G
t 20 '' C/IconX
TENV="WBDE_ICON=$W/run/iscript.info" t 10 'from iconx' C/IconX "$W/run/iscript"
t 0 '' C/IPrefs
t 0 '' C/ConClip
t 5 '' C/LoadWB
t 0 '' C/SetFont topaz 8
t 0 '' C/SetKeyboard usa
t 10 'Recoverable' C/RemRAD
t 20 '' C/MagTape
# --- Tools:, System:, Utilities:, Rexxc: ---
t 0 'Kickstart version' Tools/ShowConfig
t - 'cmd|CMD' Tools/CMD
t - 'Usage|required' Tools/PrintFiles
t 10 '' Tools/PrepCard
TINPUT=keys t 0 'foo bar' Utilities/More f2.txt
TBREAK=3 t 0 '' System/NoFastMem
t 20 'required' System/Format
t 20 'required' System/DiskCopy
t 0 'ARexx' System/RexxMast
t - 'Usage|required' Rexxc/RX
t 10 'not active' Rexxc/HI
t 5 '' Rexxc/WaitForPort nonesuch
# --- the shell (amigafmt -c) and its own commands ---
s 0 '^hello world$' 'Echo "hello world"'
s 0 '^b$' 'Set x b' 'Echo $x'
s 0 '^5$' 'Search f1.txt nothing' 'Echo $RC'
s 5 '' 'Search f1.txt nothing'
s 0 '^hi bob$' 'Execute script1 bob'
s 0 '^two$' 'If EXISTS f1.txt' 'Echo two' 'Else' 'Echo bad' 'EndIf'
s 0 '^f1\.txt' 'CD d1' 'List'
s 0 '^Current directory' 'Path'
s 0 'stack size is [0-9]+' 'Stack'
s 0 'alpha' 'Type >out.txt f1.txt' 'Type out.txt'
s 0 'object not found' 'Fault 205'
s 0 '^amigafmt$' 'Setenv Shell amigafmt' 'Getenv Shell'
s 0 'INTERNAL' 'Resident'
s 0 '^f1\.txt' 'Alias ll List []' 'll d1'
s 0 'not set a return code|failed' 'Why'
s 0 '^42$' 'Echo `Eval 6 * 7`'
TINPUT=shellin s 0 'Process 1 ending' 'NewShell FROM NIL:'
s 0 '^d1:|f1\.txt' 'Assign FOO: d1' 'Dir FOO:'
s 0 '^hello from arexx 42' 'SYS:System/RexxMast' 'SYS:Rexxc/RX t.rexx'

# --- everything else, once without arguments ---
sweep=1
for d in . C System Tools Tools/Commodities Utilities Prefs Rexxc WBStartup; do
	for f in "$SYS"/$d/*; do
		[ -f "$f" ] || continue
		case "$f" in *.info) continue;; esac
		case "$seen" in *" $f "*) continue;; esac
		[ "$(od -An -tx1 -N4 "$f" | tr -d ' ')" = 000003f3 ] || continue
		af "${SURVEY_BREAK:-10}" INT $RUN "$AF" $AFOPT "$f"
		got=$?
		c=$(classify "$W/out" "$got")
		[ "$c" = ok ] && c=ran
		f=${f#$SYS/}
		row "${f#./}" "$c" "rc $got: $(detail "$W/out")"
		[ $verbose = 1 ] && head -${SURVEY_LINES:-8} "$W/out" | sed 's/^/    /' >&2
	done
done

echo
echo "works $n_works, wrong $n_wrong, unimpl $n_unimpl, crash $n_crash, hang $n_hang, gui $n_gui, ran $n_ran, missing $n_missing"
exit $failed
