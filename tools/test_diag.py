#!/usr/bin/env python3
"""Check launch diagnostics: outcomes, native error history, classification."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    ampr = root / "vendor/kraken/vendor/ampr-emu-ng"
    with tempfile.TemporaryDirectory(prefix="sm-diag-") as directory:
        binary = Path(directory) / "diag"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wstrict-prototypes", "-Wmissing-prototypes",
            "-I" + str(root / "include"),
            str(root / "tests/test_diag.c"), "-o", str(binary)]
        command += shlex.split(os.environ.get("JSON_C_CFLAGS") or
            subprocess.check_output(["pkg-config", "--cflags", "json-c"], text=True))
        command += shlex.split(os.environ.get("JSON_C_LIBS") or
            subprocess.check_output(["pkg-config", "--libs", "json-c"], text=True))
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        run = [str(binary)]
        if (ampr / "libSceAmpr.sprx").is_file() and (ampr / "VERSION").is_file():
            run += [str(ampr / "libSceAmpr.sprx"), str(ampr / "VERSION")]
        subprocess.run(run, check=True, timeout=60)


if __name__ == "__main__":
    main()
