# The test programs are AmigaOS hunk executables built from tests/*.S and
# tests/*.c (mkprog.sh); check.sh runs each through amigafmt and compares
# the output and exit code with expected/<case>.out.
mkhunk = files('../tools/mkhunk.py')
mkprog = find_program('mkprog.sh')
check = find_program('check.sh')
cc_cmd = ' '.join(cc.cmd_array())

progs = {}
foreach p : ['hello', 'abs4', 'crash']
  progs += {p: custom_target(p, input: p + '.S', output: p, build_by_default: true,
                             command: [mkprog, cc_cmd, mkhunk, '@OUTPUT@', '@INPUT@'])}
endforeach
foreach p : ['targs', 'tfmt', 'tfiles', 'texec', 'tunimpl', 'tpat', 'tmath',
             'ttimer', 'tsystem', 'tscreen', 'tasl', 'tsock', 'tmachine']
  progs += {p: custom_target(p, input: ['start.S', 'lib.c', p + '.c'], output: p, build_by_default: true,
                             command: [mkprog, cc_cmd, mkhunk, '@OUTPUT@', '@INPUT@'])}
endforeach

# case name, expected exit code, amigafmt options, program, arguments
cases = [
  ['hello', '0', [], 'hello', []],
  ['hello-noprog', '20', [], 'nonexistent', []],
  ['abs4-emulated', '0', ['-n'], 'abs4', []],
  ['abs4', '0', [], 'abs4', []],
  ['crash', '20', [], 'crash', []],
  ['args1', '0', [], 'targs', ['a', 'b', 'c']],
  ['args2', '0', [], 'targs', ['TO', 'dest', 'QUIET', 'with space', 'NUM=42', 'TEXT', 'q"uote']],
  ['args3', '10', [], 'targs', []],
  ['args4', '10', [], 'targs', ['x', 'NUM', 'notanumber']],
  ['args5', '0', [], 'targs', ['one', 'TOG', 'two']],
  ['fmt', '0', [], 'tfmt', []],
  ['files', '0', [], 'tfiles', []],
  ['exec', '5', [], 'texec', []],
  ['unimpl', '20', [], 'tunimpl', []],
  ['pattern', '0', [], 'tpat', []],
  ['math', '0', [], 'tmath', []],
  ['timer', '0', [], 'ttimer', []],
  ['system', '0', [], 'tsystem', []],
  ['socket', '0', [], 'tsock', []],
  ['machine', '0', [], 'tmachine', []],
  ['machine-sysfs', '0', [], 'tmachine', []],
]
here = meson.current_build_dir()
# a board directory with the layout of /sys/bus/zorro, for machine-sysfs
run_command(find_program('mkzorro.py'), here / 'zorro', check: true)

# environment for a case (the default runs with no config file)
case_env = {
  'files': {'AMIGAFMT_CONF': '/dev/null'},
  'machine-sysfs': {'AMIGAFMT_ZORRO': here / 'zorro'},
  'machine': {'AMIGAFMT_ZORRO': '5080/148@0xe90000/0x10000,2206/33@0x40000000/0x1000000,2206/0xa1@0x60000000/0x20000000'},
}
foreach c : cases
  name = c[0]
  prog = c[3]
  test(name, check,
       args: [files('expected' / name + '.out'), c[1], here, amigafmt.full_path()] + c[2] + ['./' + prog] + c[4],
       env: case_env.get(name, {}),
       depends: progs.has_key(prog) ? [amigafmt, progs[prog]] : [amigafmt],
       suite: 'programs')
endforeach
