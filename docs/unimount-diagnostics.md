# UniMount launch diagnostics

A game can be mounted and have a home-screen icon and still not start. The
diagnostics subsystem records what actually happened to every observed launch,
using the console's own error codes, and exposes it through the HTTP API
([`api.md`](api.md#launch-diagnostics)):

- `GET /api/v1/system` - what this console and this UniMount run can do.
- `GET /api/v1/diagnostics/launches[?title_id=&limit=]` and
  `GET /api/v1/diagnostics/launches/<id>` - launch records.
- `GET /api/v1/diagnostics/errors[?limit=]` - the console's error history.
- `last_launch` in every `/api/v1/games` and `/api/v1/games/info` item.

Diagnostics only observe. The hooks in the launch path copy an event into a
bounded in-memory queue and return; they never wait, read files or change the
response sent to ShellCore. All probing (kernel log, error-history files,
mount table, app.db, `param.json`) runs on the diagnostics thread. If the queue
is full the event is dropped and `[DIAG] dropped N launch event(s)` is logged.

At shutdown `sm_diag_stop()` runs after the ShellCore hooks, the ShellCore
service and the lifecycle watcher have stopped and before mdbg, app.db and the
log are shut down: new events are dropped, the thread processes what is
queued, writes `launches.json` once more and is joined. If the thread cannot
be created at all, the store is reported unavailable at once (library items
get `last_launch: null` without waiting).

## Where events come from

| Event | Source in UniMount |
| --- | --- |
| Launch requested + launch-mount result | ShellCore `LAUNCH` request, `handle_launch_request()` in `sm_shellcore_service.c` |
| `LAUNCH_FAILED` | ShellCore `LAUNCH_FAILED` request, `handle_client()` |
| Sandbox ready (+ overlay result) | ShellCore `SANDBOX_READY`, `handle_sandbox_ready_request()` |
| Process started / handoff / exit | Lifecycle watcher (`sm_game_lifecycle.c`): `dispatch_game_launch()`, `try_game_process_handoff()`, `handle_game_exit()`, `finalize_game_exit()` |
| rtld error, fatal signal, crash flag | `sm_mdbg.c` `handle_game_failure()` (first finding of the 60 s monitor) |
| Kernel-log lines | `sm_mdbg_get_log_tail()`, read on the diagnostics thread |
| Native errors | `/system_data/priv/error/history/*.json` |

Only valid game title IDs are recorded (`NPXS` system apps are not). Titles
that are not managed by UniMount (installed PKGs) are recorded too, with
`managed=false` and `mounted.state=not_observed`. "Not managed" is decided
only from the launch mount's mount-link lookup (layer `mount_link`, ENOENT) or
an unsupported title id (no layer, EINVAL). ENOENT from any other layer - for
example `kraken_source` when a Kraken title's `.pkg` is offline - is a managed
`mount_failed` with its diagnosis.

A title that was only mounted or registered has no record: nothing launched
it. Its library item reports `last_launch: null`.

## Stages

Every record has the same seven stages, each
`{state, at, at_unix_ms, mono_us, detail}` with `state` one of `ok`, `failed`,
`pending` (expected but not seen yet) or `not_observed`.

| Stage | `ok` means | `failed` means |
| --- | --- | --- |
| `mounted` | The launch mount was ready when ShellCore asked | The launch mount was refused; `mount.layer` and `mount.errno` say where and why |
| `registered` | The title was in app.db when the launch was processed | It was not in app.db |
| `launch_requested` | ShellCore's launchApp reached UniMount (`launch_attempts` counts retries) | - |
| `sandbox_ready` | UniMount's sandbox overlays were prepared before spawn | Overlay preparation reported an error (ShellCore still continues) |
| `process_started` | The lifecycle watcher saw the game process (pid, app id) | No process within 30 s of the request, or `LAUNCH_FAILED` without a retry within 10 s |
| `running_confirmed` | The process stayed alive for `running_confirm_seconds` with no fatal finding | It exited or crashed before that |
| `exited` | The process exit was final (after the same-app handoff grace) | - |

`mount.layer` values: `sleep`, `mount_link`, `read_only_override`,
`source_resume`, `kraken_source`, `claim` (another launch/release/mutation was
in progress), `image`, `title_nullfs`, `backport_overlay`.

## Outcomes

| Outcome | Rule |
| --- | --- |
| `launch_in_progress` | Requested or process started, no verdict yet |
| `running` | `running_confirmed` is `ok` and no exit was seen |
| `exited_normally` | Confirmed running, then exited |
| `mount_failed` | `mounted` failed |
| `spawn_failed` | Requested/sandbox ready but no process in time, or `LAUNCH_FAILED` |
| `exited_early` | The process exited before confirmation |
| `crashed` | A fatal signal, exception report or mdbg crash flag was observed |
| `launch_not_observed` | Record without a launch request or process (should be rare) |
| `observation_interrupted` | UniMount restarted or stopped, or lost the process, before a verdict; the unobserved stages are `not_observed` |

Running is confirmed only by observation: the diagnostics thread probes the
recorded PID (`kill(pid, 0)` plus `sceKernelGetAppInfo()` title and app id,
like the lifecycle watcher) about once a second until confirmation and every
5 s afterwards, and `running_confirmed` becomes `ok` only after a positive
probe at or after `running_confirm_seconds`. The same probes bound missed
exits: when the PID is gone (or now belongs to another application) on two
consecutive probes without an exit event, the exit is concluded with the detail
"exit not observed directly" - `exited_normally` when running was confirmed,
`exited_early` when the process vanished before the confirmation time, and
`observation_interrupted` when it vanished later without a probe proving it
lived that long. A `running_confirmed` already decided (`ok`, or `failed` by a
fatal finding) is never changed by this. A `NOTE_EXIT` whose final exit is
never reported is concluded 60 s later with the observed exit time. When the
PID exists but its application identity cannot be read, the probe counts
neither as alive (no confirmation) nor as gone; after 30 such probes in a row
the launch is interrupted (`observation_interrupted`). If a new process is
reported on the PID of a launch whose exit was missed, that launch's exit is
concluded the same way and the new process gets its own record. A same-app
handoff never revives a launch whose exit was already concluded.

A mount or registration success never makes a launch `running`: only the
lifecycle watcher's process plus the confirmation time does. A same-app
process handoff (ExitSpawn/LoadExec) keeps the launch alive. A late process
appearing after a `spawn_failed` verdict revises that record and adds a
`verdict_revised` timeline entry.

## Native error capture

The console writes one JSON file per user-visible error into
`/system_data/priv/error/history/` (UTF-8 with a BOM). After every launch
request the diagnostics thread polls this directory every 2 s for
`error_window_seconds` (default 120). A new file is attached to the launch:

- `attribution: "title_id"` when its `titleId` equals the launch's title;
- `attribution: "time_window"` when it is filed under a system title (`NPXS…`)
  inside the window. Launch failures are often filed under `NPXS40087`.
  This attribution is circumstantial: any system error in the window matches.

Errors filed under another game's title are not attached. The file's mtime
must not be older than the launch. `shortError`, `internalErrorCode` (signed,
plus `0x%08X`), `messageBuffer`, `titleId`, `displayVersion`, `tick`,
`__appId`, the file name and mtime are kept as written. `tick` is reported raw
and not converted.

`GET /api/v1/diagnostics/errors` lists the directory newest first by file
mtime (the newest 512 files whatever the directory order; `total_files` is
capped at 512); unreadable or garbled files appear with `parsed=false`.

## Kernel log

When a launch is processed, the diagnostics thread remembers the last 512
bytes of the kernel log (the SDK log text that `sm_mdbg` and
`/api/v1/kernel-log` read). After `LAUNCH_FAILED`, an exit, a finding, an
outcome change and once when the error window closes, it reads the log again
and keeps only lines after that anchor:

- `processSpawn() error: …` / `An error occured while creating new process …`
  (`spawn_error`), attributed when the preceding `createApp <TITLE>` /
  `titleId = <TITLE>` line names this launch; spawn errors of another title
  are skipped;
- `[rtld] <pid> ERROR …` for this launch's PID (`rtld`);
- `sceSblAuthMgrAuthHeader …` lines while a process exists (`self_auth`, not
  attributed: the line carries no PID);
- `# exception: …` followed by `# proc ID: <pid>` of this launch
  (`exception`).

If the anchor has rolled out of the ring buffer, everything still in the log
is newer than the launch and is scanned. If the kernel log cannot be read (no
coredump privileges), these lines are simply missing.

## Overlay layers

On `SANDBOX_READY` the diagnostics thread reads the mount table and records
every mount inside `/mnt/sandbox/<TITLE>_NNN/` with a label: `app0`,
`kraken_pkg` (`app0/kraken`), `fakelib` (`…/common/lib`), `data`, `usb`, `ext`,
`disc`, `fonts`, `other`. For the fakelib layer the source directory and its
file list are kept; they are used by the `fakelib_conflict` rule. This is a
snapshot taken a few milliseconds after the overlays were mounted; a process
that dies immediately can leave `overlay.captured=true` with fewer layers.

## Classification

Launches with outcome `mount_failed`, `spawn_failed`, `exited_early` or
`crashed` get `diagnosis = {category, confidence, rule, evidence, advice}`.
Running, successful and pending launches have `diagnosis: null`. The rules are
a table in `src/sm_diag_core.c` (`k_classify_rules`); the first match wins and
every rule cites its source next to its code. `confidence` is `observed` when
the evidence names this launch (title-attributed error, PID-tagged rtld line,
UniMount's own mount result) and `inferred` otherwise.

| Category | Rule | Source |
| --- | --- | --- |
| `mount_failed` | UniMount's launch mount failed; errno and layer quoted | this repo, `mount_managed_title_runtime()` |
| `permission` | Internal code `0x80AA001A` (CE-107750-0), kernel log `processSpawn() error: 0x80aa001a`, or rtld `mount flag / attribute error`; advice names the file when the log does | upstream ShadowMountPlus PR #226, issues #189/#219 |
| `fakelib_conflict` | The module named by an rtld error is one of the files of this launch's fakelib overlay; `sceSblAuthMgrAuthHeader` / `0xa0020104` lines are quoted | upstream issue #227 |
| `firmware_incompatible` | `PRX_NOT_RESOLVED_FUNCTION` / `0xa0020101` in rtld or a fatal report (observed), or `requiredSystemSoftwareVersion` in `sce_sys/param.json` above the console firmware (inferred) | upstream issue #197 |
| `missing_library` | rtld errors naming `.prx`/`.sprx` modules, listed in the advice | upstream issue #227 rtld format |
| `mount_busy` | Internal code `0x80940033` (CE-105773-3) | upstream issues #196/#216 |
| `corrupt_data` | Internal code `0x80940039` (CE-105779-9) | the console's own message for that code |
| `kernel_patch` | kstuff did not answer KEKCALL_CHECK, or the ShellCore hooks were not installed, when the launch was observed | this repo (`main.c`, hooks) |
| `unknown` | Nothing above matched; all raw evidence is kept | - |

Internal codes outside this table (for example `0x80BA0013`, CE-108262-9) stay
`unknown`. Error meanings are never guessed.

### Firmware requirement

`requiredSystemSoftwareVersion` is a 64-bit hex string whose top 16 bits are
the BCD major.minor firmware: `"0x1360000000000000"` is 13.60 (value from a
PPSA24913 patch in upstream issue #197; issue #227 shows `0x1120…` = 11.20).
`kernel_get_fw_version()` uses the same BCD encoding in bits 31..16 (13.60 is
`0x1360xxxx`, see `get_firmware_version_string()` in `main.c`). Packed BCD
compares in decimal order as an integer, so `0x1360 > 0x1340` means "needs
13.60, console has 13.40". Only major.minor is compared. The file is read
right after a successful launch mount from the title's source
(`<mount.lnk target>/sce_sys/param.json`), never through the live
`/system_ex/app/<TITLE>` mount, so an open file cannot make a non-forced
unmount fail with EBUSY during the launch window. Titles without a
`mount.lnk` (not managed) fall back to `/system_ex/app/<TITLE>/sce_sys/param.json`.

## System report

`GET /api/v1/system` measures where it can and labels what it infers:

- `firmware` from `kernel_get_fw_version()`.
- `shellcore`: offset-table entry for this firmware, hook state from the hook
  module itself, and the registration path the install code uses:
  `batch_install_all` when `[library] install_all` is on,
  `shellcore_bridge` on firmware 12.00+ (available only with the AppInstallAll
  hook installed), otherwise `direct_appinstutil` (symbol lookup).
- `kstuff.loaded` is measured with KEKCALL_CHECK. `implementation`/`version`
  are inferred: kstuff has no version query. UniMount looks for processes named
  `kstuff*` (the 19-character process name is truncated, for example
  `kstuff-lite_v1.11.e`) and for the first `kstuff` entry of PS5 Payload
  Manager's `/data/pldmgr/autoload.txt`; for a `repo/…` or `local/…` slot it
  reads `/data/pldmgr/payloads/<slot>/meta.json` and reports its `version`
  with `version_source: "pldmgr_meta"`. An autoload entry shows what PLDMGR is
  configured to load, not what is running.
- `mechanisms`: `/dev/lvdctl` and `/dev/mdctl` are opened read/write and closed.
  nullfs/unionfs/pfs use `vfs.conflist` when it exists; on the PS5 it does not
  (ENOENT), so availability comes from evidence (`sm_diag_fs_availability()`):
  a mount of that type in the mount table now is `available: true`
  ("N <fs> mount(s) present"); otherwise a mount of that type UniMount
  completed earlier in this process (a fakelib unionfs overlay, a PFS image)
  is `true` ("mounted successfully this session"); otherwise `null` ("not yet
  observed on this boot"). Without the kernel list it is never `false`. PFS
  images also need LVD. Kraken direct PKG mounting needs nullfs and reports
  which AMPR runtime a launch would install, with the same selection as the
  launch path: `shared` `<emulators path>/ampr-emu-ng/libSceAmpr.sprx`, else
  `shared` `<emulators path>/libSceAmpr.sprx` (both must pass the same
  validation), else `bundled`, and the version string the runtime embeds
  (`ampr-emu-ng 0.4.1`).
- `limitations`, for example `native_debug_pkg_unavailable` above 11.60:
  kstuff-lite's `ps5-kstuff/uelf/fpkg.c` returns early with
  `if(FWVER > 0x1160) return NULL;`, so plaintext debug packages cannot be
  installed natively there. That is why `.pkg` titles go through the Kraken
  backend.

## Configuration

```ini
[diagnostics]
enabled = on                  # off: events are discarded, API still answers
running_confirm_seconds = 20  # 5..600
error_window_seconds = 120    # 10..3600
```

## Persistence

The last 64 records are kept in memory and saved to
`/data/shadowmount/launches.json` (unique `mkstemp` temporary file, fsync,
rename; the file is read with `O_NOFOLLOW`) at most every 2 s while something
changed, from the diagnostics thread, and once more at shutdown. They are restored on
the first diagnostics use after start. No record stays "in progress" across a
restart:

- At shutdown, an unfinished launch whose process is already gone is finalised:
  with an observed `NOTE_EXIT` its exit is concluded at that time (so a
  confirmed game is `exited_normally`), otherwise it becomes
  `observation_interrupted`. A game that is still running is saved in
  progress.
- On restore, an unfinished launch whose PID still runs the same application
  (same title and app id; a reused PID or an unknown app id does not count) is
  re-attached: its monotonic times are rebased from the wall clock, an
  `observation_resumed` timeline entry is added, and it keeps being observed
  until `running_confirmed` and `exited` are reached. A PID that exists but
  whose identity cannot be read is re-attached the same way and left to the
  probes above. A process that started before the current boot (its start is
  older than the uptime) is never re-attached.
- Any other unfinished launch becomes `observation_interrupted` with
  `observation_interrupted: true`; its pending or unreached stages
  (`running_confirmed`, `exited`, ...) become `not_observed` with the detail
  "UniMount restarted before this was observed". Stages already observed keep
  their state. A diagnosis is kept only when evidence (native error, game
  error, kernel-log line) was attached before the interruption; an observed
  verdict such as `crashed` is kept.

Finished records, including interrupted ones that kept an observed verdict
such as `crashed`, are restored as they were and never re-evaluated or absorb
a process of the current run; restoring them again changes nothing. Monotonic timestamps (`mono_us`) only compare
within one UniMount run.

## Honest limits

- `time_window` attribution is circumstantial.
- `sm_mdbg` stops after its first finding and monitors for 60 s only; later
  crashes are seen as exits.
- A spawn error line without a `createApp`/`titleId` line before it is kept
  but not attributed.
- The kernel log ring is small (about 128 KiB); a busy log can lose lines
  before the scan.
- Without the ShellCore hooks no launch request is seen; launches started by
  other means appear when their process starts (`launch_requested`
  `not_observed`).
- `spawn_failed` after the 30 s timeout means "no game process was observed",
  which includes a watcher that could not track the process.
- kstuff identification is inferred from process names and PLDMGR files.
