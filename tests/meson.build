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
             'ttimer', 'tsystem', 'tscreen', 'tasl', 'tsock', 'tmachine', 'tcli']
  progs += {p: custom_target(p, input: ['start.S', 'lib.c', p + '.c'], output: p, build_by_default: true,
                             command: [mkprog, cc_cmd, mkhunk, '@OUTPUT@', '@INPUT@'])}
endforeach

# a script for the shell test, next to the programs
configure_file(input: 'script1', output: 'script1', copy: true)

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
  # the shell: amigafmt -c lines, scripts, the shell's own commands
  ['shell', '3', ['-c', 'Echo "hello world"', '-c', 'Set x b', '-c', 'Echo $x ${x}c *$x $nonesuch ; a comment',
                  '-c', 'Alias say Echo said [] !', '-c', 'say it', '-c', 'Echo `Echo inner` outer',
                  '-c', 'FailAt', '-c', 'FailAt 11', '-c', 'targs',
                  '-c', 'If ERROR', '-c', 'If WARN', '-c', 'Echo nested', '-c', 'Else', '-c', 'Echo bad',
                  '-c', 'EndIf', '-c', 'Else', '-c', 'Echo bad', '-c', 'EndIf', '-c', 'Echo RC=$RC',
                  '-c', 'If $x EQ B', '-c', 'Echo equal', '-c', 'EndIf', '-c', 'If 10 GT 9 VAL', '-c', 'Echo greater', '-c', 'EndIf',
                  '-c', 'Skip over', '-c', 'Echo bad', '-c', 'Lab over', '-c', 'Fault 205 103', '-c', 'Stack 9000', '-c', 'Stack',
                  '-c', 'Echo >af-shell.tmp redirected', '-c', 'Echo >>af-shell.tmp appended',
                  '-c', 'tcli <af-shell.tmp cat', '-c', 'targs a TO b',
                  '-c', 'tcli exit 4', '-c', 'Echo Exit gave $RC', '-c', 'tcli internal', '-c', 'Resident hello',
                  '-c', 'Resident tcli tcli', '-c', 'Resident REMOVE tcli',
                  '-c', 'Quit 3', '-c', 'Echo bad'], '', []],
  ['shell-script', '0', ['-c', 'Echo before', '-c', 'tcli script script1', '-c', 'Echo lost: the script replaced the input'], '', []],
  # CD in a nested shell, in the board directory made for machine-sysfs
  ['shell-cd', '0', ['-c', 'CD zorro', '-c', '/tcli cd', '-c', '/tcli cd', '-c', '/tcli cd'], '', []],
  ['shell-tmpdir', '20', ['-c', 'Echo bad'], '', []],
  ['shell-fail', '10', ['-c', 'Echo one', '-c', 'targs', '-c', 'Echo bad'], '', []],
  ['vars', '0', ['-a', 'ENV=T:'], 'tcli', ['vars']],
  ['names', '0', [], 'tcli', ['names']],
  ['keymap', '0', [], 'tcli', ['keymap']],
  ['packets', '0', [], 'tcli', ['pkt']],
]
here = meson.current_build_dir()
# a board directory with the layout of /sys/bus/zorro, for machine-sysfs
run_command(find_program('mkzorro.py'), here / 'zorro', check: true)

# environment for a case (the default runs with no config file)
case_env = {
  'files': {'AMIGAFMT_CONF': '/dev/null'},
  'machine-sysfs': {'AMIGAFMT_ZORRO': here / 'zorro'},
  'shell-tmpdir': {'TMPDIR': '/nonexistent-afshell'},
  'machine': {'AMIGAFMT_ZORRO': '5080/148@0xe90000/0x10000,2206/33@0x40000000/0x1000000,2206/0xa1@0x60000000/0x20000000'},
}
foreach c : cases
  name = c[0]
  prog = c[3]
  test(name, check,
       args: [files('expected' / name + '.out'), c[1], here, amigafmt.full_path()] + c[2] +
             (prog != '' ? ['./' + prog] : []) + c[4],
       env: case_env.get(name, {}),
       depends: prog == '' ? [amigafmt] + progs.values() : [amigafmt] + progs.get(prog, []),
       suite: 'programs')
endforeach
