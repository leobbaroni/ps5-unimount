#!/usr/bin/env python3
"""Run the UniMount takeover/compat/auto-mount/AMPR decision tests on the host."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='sm-unimount-') as directory:
        binary = Path(directory) / 'unimount_logic'
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wmissing-prototypes', '-Wstrict-prototypes',
            '-I' + str(root / 'include'),
            str(root / 'tests/test_unimount_logic.c'),
            str(root / 'src/sm_unimount_logic.c'), '-o', str(binary)]
        command += shlex.split(os.environ.get('JSON_C_CFLAGS') or
            subprocess.check_output(['pkg-config', '--cflags', 'json-c'], text=True))
        command += shlex.split(os.environ.get('JSON_C_LIBS') or
            subprocess.check_output(['pkg-config', '--libs', 'json-c'], text=True))
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()
