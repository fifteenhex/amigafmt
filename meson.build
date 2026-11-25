project('amigafmt', 'c',
  version: '0.1',
  meson_version: '>= 1.1',
  default_options: ['buildtype=debugoptimized', 'c_std=gnu11', 'warning_level=2'])

cc = meson.get_compiler('c')
python = find_program('python3')

cflags = ['-m' + get_option('cpu'), '-Wno-unused-parameter', '-Wno-sign-compare',
          '-fno-strict-aliasing']
add_project_arguments(cflags, language: 'c')
add_project_link_arguments('-static', language: 'c')

# Debian's gcc 16 for m68k spills a float that is live across a call as
# 80-bit extended and reloads it as a 4-byte single (wrong code at -O2).
# tools/floatcheck.c reproduces it; when it fails, or cannot be run,
# -ffloat-store avoids it.
if meson.can_run_host_binaries()
  r = cc.run(files('tools/floatcheck.c'), args: cflags + ['-O2', '-lm'],
             name: 'floats survive a call without -ffloat-store')
  need_float_store = not r.compiled() or r.returncode() != 0
else
  need_float_store = true
  message('cannot run host binaries: assuming -ffloat-store is needed')
endif
if need_float_store
  add_project_arguments('-ffloat-store', language: 'c')
endif

libm = cc.find_library('m', required: false)

srcs = files(
  'src/main.c', 'src/lib.c', 'src/hunk.c', 'src/amem.c', 'src/path.c',
  'src/exec.c', 'src/fmt.c', 'src/dos.c', 'src/lock.c', 'src/readargs.c',
  'src/pattern.c', 'src/date.c', 'src/utility.c', 'src/intuition.c',
  'src/gfx.c', 'src/gui.c', 'src/gadtools.c', 'src/icon.c', 'src/boopsi.c',
  'src/asl.c', 'src/math.c', 'src/timer.c', 'src/fdtab.c', 'src/glue.S')
amigafmt = executable('amigafmt', srcs,
  dependencies: [libm],
  install: true)

install_data('tools/register.sh', install_dir: get_option('bindir'),
             rename: 'amigafmt-register', install_mode: 'rwxr-xr-x')
install_data('amigafmt.conf.example', install_dir: get_option('datadir') / 'amigafmt')
install_data('README.md', install_dir: get_option('datadir') / 'doc' / 'amigafmt')

# the vector tables, regenerated from the .fd files into the source tree
fds = ['exec', 'dos', 'utility', 'intuition', 'graphics', 'locale', 'icon',
       'mathieeedoubbas', 'mathieeedoubtrans', 'mathieeesingbas',
       'mathieeesingtrans', 'mathffp', 'mathtrans']
run_target('fdtab',
  command: [files('tools/fdtab.sh'), files('tools/fd2c.py'),
            meson.current_source_dir() / 'src' / 'fdtab.c', get_option('fd_dir')] + fds)

subdir('tests')

if get_option('ndk_fd_dir') != ''
  test('fdcheck', python, args: [files('tools/fdcheck.py'), get_option('ndk_fd_dir')],
       suite: 'tables')
else
  message('ndk_fd_dir not set: the fdcheck test is not run')
endif
