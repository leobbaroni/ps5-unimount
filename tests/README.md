Host regression tests (Linux/WSL; no console connection):

```sh
python3 tools/test_runtime_config.py --sanitize
python3 tools/test_shellcore.py --sanitize
python3 tools/test_scan_permissions.py
python3 tools/test_exec_permissions.py
python3 tools/test_shellcore_launch.py --sanitize
python3 tools/test_sandbox_cleanup.py
python3 tools/test_unimount_logic.py --sanitize
python3 tools/test_diag.py --sanitize
```

The UniMount logic test compiles `src/sm_unimount_logic.c` with the host
json-c (`libjson-c-dev`, found through `pkg-config`). It covers Kraken Mount
detection (the full reply classification matrix: complete, body-prefix,
truncated, empty, 5xx, non-HTTP and other-server replies), quit outcomes, legacy-mount matching, adoption rules, the ownership
ledger, the compatible listener's request parsing, routing and redirect, the
automatic-mount failure ledger and decisions (a user cancel is remembered,
other cancels are retried), and AMPR-emu-NG selection and
replacement rules. The runtime config test also checks the `[compat]` and
`[kraken]` settings.

The config test compiles the production implementation with host stubs for
logging, localization and backend names. It checks concurrent initialization,
snapshot lifetime, coherent reads during repeated file reloads, path/rule
getters, unchanged settings and restoration of defaults after file removal.
The ShellCore tests cover actual ptrace results, credential restoration, hook
repair, partial writes, failed rollback, detach failures and shutdown ownership.
If ASan cannot initialize under WSL, prefix either Python command with
`setarch x86_64 -R` to disable ASLR for that process.

The scan permission test checks inaccessible roots and descendants in watcher
discovery while keeping content scans and real I/O failures strict.

The exec permission test checks that game executables gain only their missing
execute bits, that directories and symlink targets are left unchanged, and
that a failed chmod is remembered per file state (dev, inode, mode, ctime).

The diagnostics test compiles `src/sm_diag_core.c` with json-c. It covers the
launch timeline and outcomes (no `running` without a confirmed process, early
exit, `LAUNCH_FAILED`, handoff, crash, persistence round trip, restored
records never absorbing a new process, unmanaged vs. managed mount failures),
the console error-history parser on real samples (BOM, signed codes to hex),
newest-512 selection independent of directory order, filesystem availability
without `vfs.conflist`, every
classification rule plus an unknown code, `param.json` firmware comparison,
PS5 Payload Manager autoload/meta parsing, kernel-log line selection and the
AMPR runtime version marker of the bundled runtime.
