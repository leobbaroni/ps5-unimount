# UniMount architecture

This page records why UniMount is built the way it is, which module owns what, how a title moves from "found" to "running", and in what order the payload starts and stops. File names refer to `src/` and `include/` in this repository. Ownership rules and diagnostics have their own pages: [`unimount-ownership.md`](unimount-ownership.md), [`unimount-diagnostics.md`](unimount-diagnostics.md).

## Decision record

Goal: one payload that replaces running ShadowMountPlus and Kraken Mount separately, so that switching mount payloads no longer breaks other tools (Orbit Store in particular).

Candidates considered:

1. **ShadowKraken.** A C re-implementation of Kraken's package reader, recovered from a closed upstream pull request. It was never tested on a console by its author.
2. **DuoMount.** Embeds Kraken's whole daemon as a Rust `std` library, applies about 40 linker `--wrap`s to the whole binary, and runs a second, parallel launch watcher.
3. **Vov4k957's fork.** A thin `no_std` Rust parser with everything else in C, inside ShadowMountPlus's own synchronous launch hook. It has real shared git history with upstream.
4. **From scratch.**

Chosen: option 3, merged into this repository. Reasons: it keeps one launch path, it needs no syscall wrapping, and it stays mergeable with both upstreams (`tools/sync-upstream.sh smp` and `tools/sync-upstream.sh kraken <tag>`). The cost is that the Kraken reader is vendored and adapted by a patch (`tools/kraken-index.patch`) that has to be reapplied on each Kraken update.

UniMount's own additions (ownership, auto mount, diagnostics, key-art and launch routes, execute-bit repair, the web interface) sit beside that engine rather than inside Kraken.

## Component map

| Area | Files | Owns |
| --- | --- | --- |
| Entry and lifecycle | `src/main.c` | Startup and shutdown order, signal handling, STOP file, handoff from an older instance |
| Takeover and ownership | `src/sm_takeover.c`, `src/sm_unimount_logic.c` (pure decisions, host-tested) | Kraken Mount detection and quit request, legacy mount release, `owner.json` ledger |
| Kraken-compatible listener | `src/sm_kraken_compat.c` | Port 8097: `/api/state`, `/api/rescan`, `/api/quit` (always 409) |
| Debug `.pkg` backend | `src/sm_kraken.c`, `kraken-bridge/`, `vendor/kraken/`, `src/kraken_assets.S` | Package header check (`sm_kraken_can_mount`), cache and index building, embedded ampr-emu-ng and PlayGo stub |
| Automatic `.pkg` mount | `src/sm_kraken_auto.c` | Candidate tracking, stability wait, failure ledger; runs the storage job in `src/sm_api_service.c` (`run_kraken_job`) |
| Discovery | `src/sm_scanner.c`, `src/sm_scan.c`, `src/sm_scan_tree.c`, `src/sm_manual.c`, `src/sm_stability.c`, `src/sm_path_state.c` | Scan roots, `manual.lst`, stability of files being copied |
| Image mounting | `src/sm_image.c`, `src/sm_image_cache.c`, `src/sm_image_index.c`, `src/sm_mount_device.c`, `src/sm_filesystem.c` | Image detection, device attach (lvd/md), mount points |
| Registration | `src/sm_install.c`, `src/sm_appdb.c`, `src/sm_title_state.c`, `src/sm_game_cache.c`, `src/sm_gameinfo.c`, `src/sm_shell_icon.c` | `mount.lnk`, app.db access, library records, the launcher tile |
| ShellCore integration | `src/sm_shellcore_hooks.c`, `src/sm_shellcore_service.c`, `src/sm_shellcore_offsets.c` (+ `.inc`, generated), `src/sm_shellcore_bridge.S`, `src/sm_shellcore_remote.c`, `src/sm_shellcore_flags.c` | Hooks in SceShellCore, the Unix-socket service the hooks talk to, per-firmware offsets, launch-time mounting of a title's runtime layers |
| Game lifecycle | `src/sm_game_lifecycle.c`, `src/sm_game_control.c` | Detecting game start and exit; launch/close guards and key art helpers (`sm_game_control.c` is host-tested) |
| Fakelib and backports | `src/sm_fakelib.c`, `src/sm_pkg_backport.c`, `src/sm_ampr_updater.c` | Overlay libraries, backports, emulator-library updates |
| Execute-bit repair | `src/sm_exec_permissions.c`, `src/sm_self.c` | Missing execute bits on `eboot.bin` and modules of folder games |
| Diagnostics | `src/sm_diag.c`, `src/sm_diag_core.c` (pure, host-tested), `src/sm_diag_system.c`, `src/sm_mount_diag.c` | Launch records, error-history parsing, classification, `/api/v1/system` |
| HTTP API and UI | `src/sm_api_service.c`, `web/index.html`, `include/sm_api_protocol.h` | Port 10101, routes, storage jobs, web interface, key art (`src/sm_icon_thumb.c`) |
| Installer and PKG | `src/sm_install_queue.c`, `src/sm_pkg.c`, `src/sm_pkg_server.c`, `src/sm_pkg_stream.c`, `src/sm_pkg_ws.c` | PKG install queue and streaming |
| Storage and SMB | `src/sm_storage.c`, `src/sm_usb.c`, `src/sm_smb.c`, `src/sm_vfs.c` | Copy/move jobs, USB eject, SMB shares |
| Config and misc | `src/sm_config_file.c`, `src/sm_config_mount.c`, `src/sm_kstuff.c`, `src/sm_fan.c`, `src/sm_l10n.c`, `src/sm_log.c`, `src/sm_mdbg.c` | Config parsing and migration, kstuff probe, fan target, localization, log, crash/debug capture |

The module boundaries above come from file names and header comments, not from a design document; when in doubt, read the file.

## Lifecycle of one title

All sources end in a home-screen registration (the log shows it going through the ShellCore `AppInstallAll` bridge) and all launch through the same ShellCore hook. They differ in how the source is prepared.

### Folder game

1. Found: the scanner meets a directory with `sce_sys/param.json` (or `param.sfo`) in a scan root or `manual.lst`.
2. Prepared: execute bits are repaired on writable storage (`sm_exec_permissions.c`). Nothing is mounted yet.
3. Registered: the title is registered with the system. For library-style sources `sm_install.c` writes a `mount.lnk` under `/user/app/<TITLEID>` pointing at the source.
4. Launch requested: the user starts the title on the console; the ShellCore hook sends a `LAUNCH` request to `sm_shellcore_service.c`.
5. Mounted: the service builds the title's sandbox layers (nullfs mounts, plus fakelib/backport overlays) before the system launches the app.
6. Running: the lifecycle watcher confirms the process; diagnostics record the stages. On exit the layers are released.

### Disk image (`.exfat`, `.ffpkg`, `.ffpfs`, `.ffpfsc`)

1. Found: the scanner meets an image file; its type and the title it holds are read and indexed (`sm_image*.c`).
2. Prepared: nothing is attached yet.
3. Registered: as for folders, with the image as the source.
4. Launch requested: as above.
5. Mounted: at launch the image is attached on demand (an `lvd` or `md` device, `sm_mount_device.c`) and mounted, then the sandbox layers are built on top. The hardware test log shows this for an `.exfat` image.
6. Running and closed: on close the layers are unmounted and the image is detached.

### Debug `.pkg` (Kraken path)

1. Found: the scanner notes a `*.pkg` in a scan root; after the stability wait, `sm_kraken_auto.c` parses its header once per (path, size, mtime). Only PS5 base packages that pass `sm_kraken_can_mount` (plaintext debug) are kept. The same job can be started by hand through `POST /api/v1/packages/mount`.
2. Prepared: the storage job (`run_kraken_job`) builds a small cache in `/data/shadowmount/kraken/<TITLEID>` with the launch files and an asset index. The `.pkg` stays a single file and is not extracted. If real Kraken Mount had registered the title, the job adopts it (see the ownership page).
3. Registered: the title is registered through the AppInstallAll bridge (the hardware log shows `result=0x00000000`). How the registration records the source for Kraken titles was not traced for this page; see `sm_kraken.c` and `sm_game_cache.c`.
4. Launch requested: as above.
5. Mounted: the cache becomes `/app0` and ampr-emu-ng (the bundled `libSceAmpr.sprx`, or a valid shared one from the `[emulators]` path) serves reads from the `.pkg`.
6. Running and closed: as above.

## Startup and shutdown order

From `main()` in `src/main.c`.

Startup:

1. Initialise user service and app-install utilities; install signal handlers.
2. Find an older `shadowmountplus.elf`; if one exists, ask it to exit and wait. Set this process's thread name to `shadowmountplus.elf`.
3. Clear a stale STOP file; rotate the log; load or migrate the configuration; initialise the scanner service.
4. Wait for kstuff; stop a standalone BackPork if one runs.
5. Kraken Mount takeover (`sm_takeover_run`), then write the ownership ledger.
6. Start the compatible listener (8097) and the automatic `.pkg` mount.
7. Start mdbg, the ShellCore service and hooks, the launcher tile, the lifecycle watcher, the ShellCore flags monitor, the AMPR updater.
8. Remount `/system_ex` writable, notify, clean stale mount directories, wait for `/dev/lvd2` to be released, clean staged links and duplicate title mounts, run app.db maintenance.
9. Startup scan, then the HTTP API on 10101, then the steady-state scan loop.

Shutdown (also reached on a failed or interrupted startup step):

1. Stop the automatic mount, then the compatible listener, the AMPR updater and the API service.
2. Stop the ShellCore flags monitor and hooks, the lifecycle watcher and the ShellCore service.
3. Shut down the scanner; stop diagnostics (it reads mdbg and app.db, so it goes before them); shut down mdbg.
4. Release title layers, then image mounts. Only if both succeed is the kstuff no-automount sentinel removed.
5. Shut down app.db access, remove the ownership ledger, close the log.

## Paths and ports on the console

| Item | Value |
| --- | --- |
| Process name | `shadowmountplus.elf` |
| Data directory | `/data/shadowmount/` |
| Config, profiles, manual list, log | `config.ini`, `profiles.ini`, `manual.lst`, `debug.log` (previous run: `debug.log.1`) |
| Stop request file | `/data/shadowmount/STOP` |
| Ownership ledger | `/data/shadowmount/owner.json` |
| Kraken caches | `/data/shadowmount/kraken/<TITLEID>` |
| zstd-folder AMPR overlays | `/data/shadowmount/ampr-ng/<TITLEID>/fakelib` |
| Emulator libraries, global fakelib | `/data/shadowmount/emus`, `/data/shadowmount/fakelib` |
| Real Kraken Mount's data (read only) | `/data/krakenmount` |
| Launcher tile title | `FAKE10101` |
| HTTP API and web interface | TCP 10101 (`[api] port`) |
| Kraken-compatible listener | TCP 8097 (`[compat] kraken_api_port`) |
| Payload loader (elfldr) | TCP 9021 |

## Pulling upstream updates

Run from a clean checkout (the script refuses otherwise).

- `bash tools/sync-upstream.sh smp` merges ShadowMountPlus history.
- `bash tools/sync-upstream.sh kraken <release-tag>` imports that Kraken Mount release's package reader, index writer and matching ampr-emu-ng runtime into `vendor/kraken`, checks the runtime checksum, records the commit in `vendor/kraken/REVISION` and reapplies `tools/kraken-index.patch`.

Review any merge conflict or patch failure before continuing. Then build and run `tools/wsl-build.sh test` before committing or publishing. The PS5 build uses the vendored copy, so a release rebuild selects the same reader and runtime. Check [`THIRD_PARTY.md`](../THIRD_PARTY.md) after an update, since versions are recorded there.
