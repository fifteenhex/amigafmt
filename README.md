# amigafmt

amigafmt runs AmigaOS executables (hunk format) directly on m68k Linux.
The CPU executes the 68k code natively; amigafmt loads the hunks and
provides the operating system: exec, dos, utility, intuition, graphics,
layers, gadtools, icon, iffparse, asl, diskfont, locale, keymap,
expansion, the math libraries, console, timer and input devices,
workbench and datatypes stand-ins, BOOPSI, custom screens, the blitter,
cooperative tasks, libraries loaded from disk, bsdsocket.library on
Linux sockets, and the AmigaOS shell with its scripts.
Registered with binfmt_misc, an Amiga program is started like any other
Linux program, each in its own Linux process.

GUI programs draw through wbde (a separate project), a desktop that
lends its clients windows and screens over a Unix socket.  Without wbde, CLI
programs run and requesters are asked on the terminal.

## Requirements

* An m68k Linux system with a 68040 or 68060 (see Building for other
  CPUs) and `CONFIG_BINFMT_MISC` for transparent execution, or
  `qemu-m68k` user-mode emulation on another host.  Under qemu-m68k
  everything works except `Supervisor()` code, because qemu aborts on
  privileged instructions instead of raising SIGILL.
* To build: meson and ninja, Python 3, and a C compiler for m68k Linux:
  either native, or Debian's `gcc-m68k-linux-gnu` cross toolchain with
  `qemu-m68k` to run the configure-time checks and the tests.
* To run GUI programs: wbde, listening on a Unix socket.
* AmigaOS programs to run.  Nothing from AmigaOS is in this repository;
  Workbench's `C:` commands, lha, unzip and the Preferences editors have
  been used for testing.

## Building

Cross-compiling on another host:

    meson setup --cross-file cross/m68k-linux-gnu.ini build
    ninja -C build              # -> build/amigafmt
    meson test -C build         # the test programs, under qemu-m68k

On m68k Linux itself, a native build needs no cross file:

    meson setup build && ninja -C build && meson test -C build

The binary is static.  It is compiled for the 68040 by default;
`-Dcpu=68060` builds for the 68060 (`meson configure build` lists the
options).  Both are supported and tested; a 68020 or 68030 with an FPU
should work with `-Dcpu=68020` but has not been tried, and there is no
software floating point, so a CPU without an FPU will not work.

At configure time `tools/floatcheck.c` is compiled and run (through the
exe wrapper when cross-compiling): Debian's gcc 16 for m68k miscompiles
a float kept live across a call at -O2, and when the check fails, or
cannot be run, the whole build gets `-ffloat-store`, which avoids it.

`tools/survey.sh <SYS: directory> build/amigafmt` runs the programs of
a real AmigaOS installation (a mounted Workbench partition, or a copy of
the Workbench disks; it is only read) and prints a table of how each one
fares: a list of command lines with the output and exit code AmigaOS
gives, the shell's own commands, then every other program once.
`-Dsys_dir=<directory>` makes it part of `meson test` (suite `survey`).

`meson test` compiles the programs in `tests/` into hunk files with
`tools/mkhunk.py` and runs each through amigafmt (`tests/check.sh`,
using `qemu-m68k` when cross-building), comparing the output with
`tests/expected/`.  `UPDATE=1 tests/check.sh ...` as meson invokes it
(see `meson test -v`) re-records a case.  GitHub Actions
(`.github/workflows/build.yml`) builds and tests both CPUs this way on
every push and pull request.

The library vector tables in `src/fdtab.c` are generated from the
AmigaOS `.fd` files and kept in the tree; `ninja -C build fdtab`
regenerates them with `-Dfd_dir=<directory>` pointing at amitools'
`data/fd`.  With `-Dndk_fd_dir=<directory>` (the NDK 3.1's `fd`
directory) `meson test` also runs `tools/fdcheck.py`, which checks
every generated and hand-written table against the NDK.

## Installing

Install the binary and register it with binfmt_misc (as root):

    ninja -C build install      # amigafmt and amigafmt-register in bindir,
                                # the example conf in datadir/amigafmt
    amigafmt-register /usr/local/bin/amigafmt

The kernel then runs any file starting with the hunk magic
`000003F3` as `amigafmt /path/to/program args...`, so `./List SYS:` or
`/mnt/sys/C/Dir` work from any shell.  The `F` flag in the registration
keeps the interpreter open, so it keeps working inside chroots; it also
means a new build must be registered again.  `amigafmt-register -u`
(`tools/register.sh` in the tree) removes the registration.

Without binfmt_misc, run programs as `amigafmt program args...`
(`qemu-m68k amigafmt program args...` on another host).

`tools/virt-binfmt-test.sh vmlinux build` boots an m68k "virt" kernel
under qemu-system-m68k with the built binary and test programs and
exercises the binfmt_misc path end to end.

## Configuration

Assigns map AmigaOS device and assign names to Linux directories.  The
defaults are `SYS:` and `ROOT:` = `/`, `C: S: L: LIBS: DEVS: FONTS:
LOCALE: ENVARC:` under `SYS:`, `T:` and `RAM:` = `/tmp`, `ENV:` =
`RAM:Env`, `HOME:` = `$HOME` and `PROGDIR:` = the program's directory.
They are overridden, in this order, by `/etc/amigafmt.conf`, then
`~/.amigafmtrc` (or the file named by `$AMIGAFMT_CONF`), then
`$AMIGAFMT_SYS` (the `SYS:` target), then `-a NAME=PATH` options.

A config file has one setting a line (`amigafmt.conf.example`):

    # an AmigaOS boot partition mounted read-only
    assign SYS: /mnt/sys
    assign Work: /mnt/lnx
    assign RAM: /tmp
    unixpaths yes

`assign NAME: target` (or just `NAME: target`) sets an assign; the
target is a Linux directory or an Amiga path built on another assign
(`C: SYS:C`).  `unixpaths yes` makes a leading `/` mean the Linux root
rather than AmigaOS's "parent directory", so `List /etc` does what a
Linux user expects; `-u` and `AMIGAFMT_UNIXPATHS=1` do the same per
run.  File names are looked up case-insensitively when the exact name
does not exist, as on an Amiga filesystem.

## Running programs

    amigafmt [options] program [arguments...]
    amigafmt [options] -c command-line [-c command-line...]

      -a NAME=PATH  assign NAME: to a Linux directory or Amiga path
      -c LINE       run LINE in the AmigaOS shell (repeatable)
      -s BYTES      stack size (default 262144)
      -t            trace every library call to stderr
      -u            a leading / is the Linux root, not the parent
      -n            do not map page 0 (emulate reads of address 4)
      -l            list the assigns and exit

Options come only before the program name, so a program's own arguments
are never mistaken for amigafmt's.  The arguments are joined into an
AmigaOS command line the way the shell would (words that contain spaces
or quotes are quoted), so `KEY=value` stays a keyword; AmigaOS quoting
has to survive the Linux shell, e.g. `List LFORMAT='"%s*N"'`.  Standard
input, output and error are the program's console; output to a terminal
has the Amiga console's control sequences translated, and ^C, ^\, and
`kill -USR1/-USR2` deliver the Ctrl-C/D/E/F break signals (a second ^C
while the first is still pending kills the program).  The program's
return code is the exit status.

### CLI programs

`amigafmt /mnt/sys/C/List SYS:Libs/m#?` or, registered, `./List`.

### The shell

`-c` runs lines through amigafmt's AmigaOS shell, one line each:

    amigafmt -c 'Set x world' -c 'Echo "hello $x"' -c 'Execute S:myscript'
    amigafmt -c NewShell        # an interactive Shell on the terminal

It parses lines as the 3.1 Shell does (`;` comments, `$var`, backquoted
commands, aliases, `<`, `>` and `>>` right after the command name), and
has the Shell's own commands: Alias, Ask, CD, Echo, Else, EndCLI, EndIf,
EndShell, EndSkip, FailAt, Fault, Get, Getenv, If, Lab, NewCLI, NewShell,
Path, Prompt, Quit, Resident, Run, Set, Setenv, Skip, Stack, Unalias,
Unset, Unsetenv and Why.  Commands are looked up in the resident list,
then as a path, in the current directory, along the path and in C:.
Scripts run the AmigaOS way, through `C:Execute` from SYS:, which
handles `.key` arguments; a command whose return code reaches the fail
level ends the script.  A command that is not an AmigaOS program is
handed to the Linux shell (`/bin/sh -c`).  Everything runs in the one
process, so assigns, variables and the current directory last for the
rest of the lines; Run forks a second process.

`System()` and `Execute()` from a program go through the same shell,
so `IconX` scripts started by wbde work too.

### GUI programs with wbde

Start wbde, then point amigafmt at its socket:

    WBDE_SOCKET=/tmp/wbde.sock amigafmt /mnt/sys/Utilities/Clock

`/tmp/wbde.sock` is the default.  The program's windows open on wbde's
Workbench screen; programs that open their own screens get wbde
screens, scaled to the monitor by the largest whole factor that fits
(`AMIGAFMT_SCREENSCALE=1` asks for 1:1, `=modeid` for the display
mode's own pixel sizes).  When wbde starts a program from an icon it
sets `WBDE_ICON` (the `.info` path) and optionally `WBDE_STACK`; the
program is then started the Workbench way, with a `WBStartup` message
and no CLI.

### Environment

| variable | effect |
| --- | --- |
| `AMIGAFMT_CONF` | config file instead of `~/.amigafmtrc` |
| `AMIGAFMT_SYS` | target of `SYS:` |
| `AMIGAFMT_UNIXPATHS` | as `-u` |
| `AMIGAFMT_TRACE` | as `-t` |
| `AMIGAFMT_NOPAGE0` | as `-n` |
| `AMIGAFMT_RAWARGS` | use this exact argument line instead of argv |
| `AMIGAFMT_WILDSTAR` | `*` is a wildcard in patterns (the WILDSTAR preference) |
| `AMIGAFMT_RAWCSI` | do not translate console control sequences |
| `AMIGAFMT_SCREENSCALE` | custom screen scaling: `1` or `modeid` |
| `AMIGAFMT_HANGDUMP` | seconds between reports of where the program is (for hangs) |
| `AMIGAFMT_ZORRO` | the expansion boards: `none`, a directory of ConfigDevs, or `manufacturer/product@address/size,...` |
| `WBDE_SOCKET` | wbde's socket (default `/tmp/wbde.sock`) |
| `WBDE_ICON`, `WBDE_STACK` | set by wbde: start from an icon, stack size |

## What works and what does not

`tools/survey.sh` gives the current answer for a given installation.

Works:

* OS-friendly CLI programs: the Workbench 3.1 and 3.1.4 `C:` commands
  (Version, Date, Avail, Eval, Dir, List, Info, Assign, Status, Which,
  CPU, Copy, Join, Type, Sort, Search, MakeDir, Rename, Protect,
  SetDate, Delete, MakeLink, Filenote, Wait, Break, ChangeTaskPri,
  Execute, IconX, SetPatch, IPrefs, ConClip, AddDataTypes, SetKeyboard,
  RequestChoice), Tools/ShowConfig, Utilities/More, LhA, UnZip.
* Scripts and the Shell's own commands (see The shell), and ARexx
  inside one shell: `amigafmt -c SYS:System/RexxMast -c 'RX script'`.
* ShowConfig's view of the machine: on an Amiga the Zorro boards Linux
  found (expansion.library's ConfigDevs), the chipset, PAL or NTSC and
  the memory regions from `/proc/hardware` and `/proc/iomem`; elsewhere
  an NTSC AGA machine with no boards (`AMIGAFMT_ZORRO` describes some).
* GUI programs on wbde: Workbench's Clock, Calculator, KeyShow,
  IconEdit, the Preferences editors (Serial, Font, IControl, Time,
  Workbench, Asl), programs with their own custom screens, HAM screens,
  BOOPSI classes, gadtools and asl requesters, fonts from disk,
  libraries loaded from `LIBS:` or `PROGDIR:libs`, programs that drive
  the blitter, and programs with several tasks.
* bsdsocket.library (the AmiTCP API) on Linux TCP/UDP sockets,
  including WaitSelect, the netdb calls and syslog.

Does not work:

* Anything that needs an AmigaOS filesystem or device handler: Lock,
  Relabel, AddBuffers, DiskChange, Mount, Format, Install, DiskCopy
  refuse cleanly; the clipboard; serial, parallel, printer and
  trackdisk devices.  Nothing touches the hardware: expansion.library
  configures no board, SetClock finds no battery-backed clock.
* Things that outlive a process: each program is its own Linux process,
  so assigns, variables, resident commands and the ARexx server made by
  one command are gone for the next unless both run in one shell (`-c`).
  Run forks.
* Datatypes (the library is present but knows no types), AmigaGuide,
  locale catalogs (everything is English), colorwheel and other disk
  BOOPSI classes, commodities.
* Preemptive multitasking: tasks are cooperative and switch only in
  `Wait()`, `WaitPort()`, `Delay()` and `WaitTOF()`.  A task that
  busy-loops starves the others.
* Hardware access other than the custom chip reads and blitter writes
  that are emulated; address 4 and the chip registers are read through
  `MOVE`, `AND`, `OR`, `CMP` and `TST` only.
* Console windows (`CON:` on a wbde window), input handlers (accepted,
  never called), the Roadshow extensions to bsdsocket and IPv6.
* Programs that need workbench.library 45 (3.1.4's LoadWB): amigafmt's
  libraries are version 40.

## Debugging

`-t` (or `AMIGAFMT_TRACE=1`) prints every library call with its
arguments and result, and says how address 4 is provided:

    [dos.library] Open(d1=$c0800124"SYS:C/List",d2=$3ed) = $c0802000

A call to a vector amigafmt does not implement stops the program with
the library, offset and name, which is where to start for an
unsupported program:

    amigafmt: prog: unimplemented dos.library -138 (CreateProc) called from $c0802036; stopping

A crash prints the signal, the PC as hunk + offset and the registers,
and exits with 20.  `AMIGAFMT_HANGDUMP=5` reports the PC and registers
every five seconds for a program that hangs.  Under qemu-m68k the same
binary and tests run on any host (`qemu-m68k build/amigafmt prog`).
