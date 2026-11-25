# The test programs are AmigaOS hunk executables built from tests/*.S and
# tests/*.c (mkprog.sh); check.sh runs each through amigafmt and compares
# the output and exit code with expected/<case>.out.
mkhunk = files('../tools/mkhunk.py')
mkprog = find_program('mkprog.sh')
check = find_program('check.sh')
cc_cmd = ' '.join(cc.cmd_array())

progs = {}
foreach p : ['crash']
  progs += {p: custom_target(p, input: p + '.S', output: p, build_by_default: true,
                             command: [mkprog, cc_cmd, mkhunk, '@OUTPUT@', '@INPUT@'])}
endforeach

# case name, expected exit code, amigafmt options, program, arguments
cases = [
  ['crash', '20', [], 'crash', []],
  ['hello-noprog', '20', [], 'nonexistent', []],
]
here = meson.current_build_dir()
foreach c : cases
  name = c[0]
  prog = c[3]
  test(name, check,
       args: [files('expected' / name + '.out'), c[1], here, amigafmt.full_path()] + c[2] + ['./' + prog] + c[4],
       env: {},
       depends: progs.has_key(prog) ? [amigafmt, progs[prog]] : [amigafmt],
       suite: 'programs')
endforeach
