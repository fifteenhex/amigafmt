# amigafmt

Run AmigaOS executables (hunk format) directly on m68k Linux.  The CPU runs
the 68k code natively; amigafmt loads the hunks and stands in for the
operating system.

    meson setup --cross-file cross/m68k-linux-gnu.ini build && ninja -C build
    meson test -C build     # test programs, under qemu-m68k when cross-building
    amigafmt [-s BYTES] [-t] [-n] program [args...]
    tools/register.sh /path/to/amigafmt     # binfmt_misc: then ./program args
