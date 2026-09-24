"""Opt-in real Windows Nsight argv transport check. No tracing or game startup.

Pass --nsight installed/nsys.exe --openmw staged/openmw.exe.
Uses a temporary folder with spaces, records the old empty-argument behavior,
and requires the repaired argument vector to reach a real child unchanged.
The final direct OpenMW --version invocation separately checks its actual
command-line parser; Nsight does not reliably forward target stdout on Windows.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import diagnosticconfig


def main():
    if sys.argv[1:2] == ['--probe']:
        Path(sys.argv[2]).write_text(json.dumps(sys.argv[3:]), encoding='utf-8')
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nsight', required=True)
    parser.add_argument('--openmw', required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This integration check targets Windows process argument transport')
    tool = Path(args.nsight).resolve(strict=True)
    exe = Path(args.openmw).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='cp4f nsight argv ') as temporary:
        root = Path(temporary).resolve()
        script = root / 'no-op-startup.txt'
        script.touch()
        common = [str(tool), 'profile', '--trace=none', '--sample=none', '--cpuctxsw=none',
                  '--duration=1', '--kill=false', '--wait=all', '--force-overwrite=false',
                  '--discard-environment=true', '--export=none', '--show-output=true']

        def run(name, command):
            result = subprocess.run(common + ['--output=' + str(root / name)] + command,
                                    cwd=exe.parent, capture_output=True, text=True, timeout=45)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            return result.stdout

        def probe(name, arguments):
            observed = root / (name + '-argv.json')
            output = run(name, [sys.executable, str(Path(__file__).resolve()), '--probe',
                               str(observed)] + arguments)
            if not observed.is_file():
                raise AssertionError('Probe did not run: ' + output)
            return json.loads(observed.read_text(encoding='utf-8'))

        legacy = ['--script-run', '']
        received = probe('legacy', legacy)
        print('Legacy empty override delivered:', json.dumps(received))
        fixed = diagnosticconfig.build_command(exe, root / 'normal config', root, startup_script=script)
        received = probe('fixed', fixed[1:])
        if received != fixed[1:]:
            raise AssertionError((received, fixed[1:]))
        print('PASS: all repaired arguments, including paths with spaces, survived Nsight')
        result = subprocess.run(fixed + ['--version'], cwd=exe.parent,
                                capture_output=True, text=True, timeout=15)
        if result.returncode or 'OpenMW version' not in result.stdout:
            raise AssertionError(result.stdout + result.stderr)
        print('PASS: packaged OpenMW directly parsed the repaired command and returned --version')
        print('No game/config loaded; GPU tracing and elevated CPU sampling were not exercised.')


if __name__ == '__main__':
    main()
