#!/usr/bin/env python3
"""Exercise production launch ownership and rollback with host mount stubs."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from test_storage_paths import functions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/sm_shellcore_service.c").read_text(encoding="utf-8")
    types = source[source.index("typedef struct {"):
                   source.index("#define SHELLCORE_RELEASE_MAX_ATTEMPTS")]
    defines = re.findall(r"^#define SHELLCORE_RELEASE_.*$", source, re.M)
    types += "\n" + re.search(r"typedef struct \{[^}]+\} launch_mount_failure_t;", source)[0]
    names = [
        "launch_preparation_pending_locked", "mount_owner_matches",
        "find_mount_owner_locked", "clear_mount_owner", "set_mount_owner",
        "claim_prepared_title", "clear_owned_title", "rollback_prepared_title",
        "finish_prepared_title", "begin_runtime_release", "end_runtime_release",
        "release_title_runtime_expected", "release_title_runtime",
        "sm_shellcore_release_title_runtime", "cleanup_abandoned_launch",
        "mount_managed_title_runtime", "handle_launch_request",
        "sm_shellcore_service_bind_prepared_app", "sm_shellcore_service_note_game_exit",
        "reset_switch_title_mount", "handle_launch_failed_request",
        "sm_shellcore_try_begin_external_mutation", "sm_shellcore_end_external_mutation"]
    with tempfile.TemporaryDirectory(prefix="sm-launch-") as directory:
        build = Path(directory)
        (build / "launch_types.inc").write_text(types + "\n" + "\n".join(defines), encoding="utf-8")
        (build / "launch_helpers.inc").write_text(
            functions(root, "src/sm_shellcore_service.c", names), encoding="utf-8")
        kraken = (root / "src/sm_kraken.c").read_text(encoding="utf-8")
        marker_constants = re.findall(r"^#define KRAKEN_(?:MARKER|REVISION|INITIAL_READER|V\d+_READER) .*$", kraken, re.M)
        patch_record = re.search(r"typedef struct \{[^}]+\} patch_record_t;", kraken)[0]
        (build / "kraken_publish.inc").write_text("\n".join(marker_constants) + "\n" + patch_record + "\n" +
            functions(root, "src/sm_pkg.c", ["sm_pkg_source_matches"]) + "\n" +
            functions(root, "src/sm_kraken.c", [
                "join_path", "cancelled", "publish_cache", "signature_reader",
                "source_stat_matches",
                "package_signature_prefix", "write_reader_signature", "write_source_signature",
                "matches_source_signature", "package_unchanged", "open_cache_temp",
                "finish_cache_temp", "write_cache_marker", "same_package_after_rename",
                "cached_backport_fingerprint", "make_launch_source_readable",
                "write_image_pointer", "rebind_cache",
            ]), encoding="utf-8")
        (build / "scan_cycle.inc").write_text(
            functions(root, "src/sm_scanner.c", ["run_full_scan_cycle"]), encoding="utf-8")
        binary = build / "launch"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-I" + str(root / "include"), "-I" + str(build),
            str(root / "tests/test_shellcore_launch.c"), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary), str(build)], check=True, timeout=30)


if __name__ == "__main__":
    main()
