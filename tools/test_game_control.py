#!/usr/bin/env python3
"""Run the key-art sizing/cache and launch/close guard tests on the host."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='sm-game-control-') as directory:
        binary = Path(directory) / 'game_control'
        command = shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wmissing-prototypes', '-Wstrict-prototypes',
            '-I' + str(root / 'include'),
            str(root / 'tests/test_game_control.c'),
            str(root / 'src/sm_game_control.c'), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)

        # The production key-art box filter, extracted from sm_icon_thumb.c.
        thumb = (root / 'src/sm_icon_thumb.c').read_text(encoding='utf-8')
        match = re.search(r'^static bool resize_rgb\([^;{]*\) \{.*?^\}',
                          thumb, re.MULTILINE | re.DOTALL)
        if not match:
            raise RuntimeError('Cannot find resize_rgb in src/sm_icon_thumb.c')
        build = Path(directory)
        (build / 'art_resize.inc').write_text(match.group(0) + '\n', encoding='utf-8')
        command[command.index(str(root / 'tests/test_game_control.c'))] = \
            str(root / 'tests/test_art_resize.c')
        command.insert(command.index('-I' + str(root / 'include')), '-I' + str(build))
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
