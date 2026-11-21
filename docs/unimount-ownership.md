# UniMount ownership

UniMount (ShadowMountPlus engine + Kraken Mount engine) must be the only
payload that mounts and registers games. Real ShadowMountPlus and real Kraken
Mount register the same titles and mount into the same sandboxes, so UniMount
takes over from them at startup and keeps them from coming back.

Compatibility anchors that are deliberately unchanged: process/thread name
`shadowmountplus.elf`, data directory `/data/shadowmount`, API port 10101 and
its routes, title `FAKE10101`, Kraken caches in `/data/shadowmount/kraken/<TID>`.
`/data/krakenmount` (real Kraken's data) is only read, never changed.

## Startup order

0. **A running managed game comes first.** Before anything else (no signal
   to an older instance, no takeover, mount, scan, hook or service), UniMount
   checks the running big app. If it is one of our titles (`mount.lnk`,
   `mount_img.lnk` or `mount.lnk.cleanup` in `/user/app/<TID>`, or a
   nullfs/unionfs on `/system_ex/app/<TID>`) or its title cannot be
   determined, UniMount waits: it logs and notifies once, checks every 3 s and
   exits at once on a signal (the STOP file too when no older instance runs).
   A retail game or a system app does not delay startup. This is the rule
   UniMount already follows for a game running under real Kraken Mount, now
   applied to every predecessor and to a start without one (recovery after a
   crash). Before it waits, UniMount evaluates the session record of the
   game's owner (see "Session record and adoption"): with a live predecessor
   it only logs whether it could adopt the session; without one it adopts a
   record that passes every check instead of waiting.
1. An older `shadowmountplus.elf` (real ShadowMountPlus or an older UniMount)
   is asked to exit (one SIGTERM per instance) and awaited for up to 60 s;
   then step 0 is checked again. An instance still alive after 60 s while a
   big app runs has deferred its stop for that game (see "Stopping with a
   game running"): UniMount waits for the app to close and for the instance
   to exit, without asking again (a second request would force it). Still
   alive with no app running: UniMount exits without starting, with a log
   line and a notification. Two instances never run together. The thread is
   then named `shadowmountplus.elf`; a real Kraken Mount started later sees
   that name and does not mount anything.
2. Configuration, log and notifications start; the log line reads
   `UniMount <ver> (ShadowMount+ <tag>, Kraken Mount <release> rev <commit>)`.
3. kstuff is awaited (unchanged).
4. **Kraken Mount takeover** (`src/sm_takeover.c`), before any scan, mount or
   registration work:
   - `GET http://127.0.0.1:8097/api/state` (2 s connect timeout, 4 s to read
     the reply). The peer is real Kraken Mount when the body is a JSON object
     with a string `version`, an array `games` and no `unimount` key; a body
     prefix that already holds both keys is enough (a large library or log
     may not arrive within the read cap or deadline).
   - Only "connection refused" means Kraken Mount is absent. A listener whose
     reply is not conclusive (no reply, reset, cut-off head, 5xx, empty body,
     body cut off before those keys) is **unknown**: state
     `waiting_for_reply`, the reason is logged and reported as `detail`, and
     the probe is repeated after 3 s, doubling to at most 30 s. Nothing is
     scanned or mounted, legacy mounts are not released and the compatible
     listener does not start meanwhile; the STOP file and signals still stop
     UniMount. A valid answer that is clearly not Kraken Mount (not HTTP, a
     non-5xx status other than 200, a body that is not a JSON object, or a
     complete JSON object without those keys) counts as "another service":
     state `absent` with that reason in `detail`.
   - `POST /api/quit` with an empty body from loopback - exactly what a newer
     Kraken copy sends (`web.rs take_port`). Kraken answers 204 and exits
     200 ms later, or 409 while a game runs.
   - 2xx: wait until nothing listens on 8097 (polled every 250 ms, logged
     after 5 s). If something still listens after 15 s, the state becomes
     `waiting_for_quit` and the probe starts again; UniMount never proceeds
     while the port answers.
   - 409: state `waiting_for_game`, one notification, ask again every 3 s.
     Any other answer (an old build without `/api/quit`): `waiting_for_quit`,
     same loop. Nothing is scanned or mounted meanwhile; the STOP file and
     signals still stop UniMount.
   - Then **legacy mounts are released**: every nullfs mount
     `/data/krakenmount/caches/<TID>` on `/system_ex/app/<TID>` is unmounted
     without `MNT_FORCE`, newest first, unless a game is running or a
     `/mnt/sandbox/<TID>_NNN` mount exists. Busy or protected mounts stay and
     are retried after later scan passes. Each action is logged with
     `[TAKEOVER]`.
5. **Ownership ledger**: `/data/shadowmount/owner.json`
   (`product`, `version`, `pid`, `started`, `boot_time`) is written. A ledger
   left by an earlier instance means it did not exit cleanly; this is logged
   (`[OWNER]`) and reported as `previous_unclean`. The existing startup
   recovery handles that instance's mounts. A clean shutdown removes the file.
6. The Kraken-compatible listener and the automatic PKG mount worker start.
7. ShellCore hooks, cleanup, startup scan and the API on 10101 follow as in
   ShadowMountPlus.

`GET /api/v1/ownership` reports all of the above (see `docs/api.md`).

## Stopping with a game running

The sandbox layers UniMount adds to a game, its NSFS backport redirects and
its `/system_ex/app/<TID>` stack are recorded only in the running instance.
Two kinds of game session are told apart (`sm_session_kind`):

- **Alias-only**: plain nullfs directory aliases at the sandbox root
  (`/data`, `/mnt/usbN`, `/mnt/extN`, `/mnt/disc`, fonts). Typical for image
  titles.
- **Overlay**: any unionfs layer (fakelib/fakelib2 on `common/lib`), anything
  mounted on or inside `app0` (the Kraken `app0` cover and `app0/kraken`), or
  NSFS backport redirects. Every Kraken-path title is an overlay session.

What a stop does (`sm_stop_decide`):

| Request | Alias-only or no layers | Overlay |
| --- | --- | --- |
| First signal (SIGTERM etc.) | Stop now, normal teardown | Defer until the game has exited |
| First STOP file | Defer until the game has exited | Defer until the game has exited |
| Second request, console shutdown, scanner failure | Stop now, normal teardown | Stop now, keep the game's layers |
| Any request, no active game | Stop now, normal teardown | Stop now, normal teardown |

- The normal teardown of an alias-only session is the one UniMount always
  did: a non-forced unmount of every alias that is not busy, under the live
  game. On hardware an image title kept running after that, closing it
  worked and the next start was clean.
- A signal is not deferred for an alias-only session because it may be
  followed by SIGKILL: PS5 Payload Manager's `process_kill` sends SIGTERM,
  waits at most 1.5 s and then sends SIGKILL.
- A deferred stop keeps UniMount working normally; it shuts down as soon as
  the game has exited and its on-exit cleanup has run.
- A kept overlay session is kept whole, without an unmount attempt, together
  with its `/system_ex/app/<TID>` stack (image teardown is then skipped, as
  for a busy stack).

**A payload manager's kill during a game with overlay layers cannot be made
safe.** UniMount defers on the SIGTERM, but the SIGKILL 1.5 s later ends it
with the game's layers still mounted and no owner left. Both alternatives end
the same way on hardware: tearing the overlay layers down under the live game
(the behaviour before this rule) and leaving them in place (a deferral cut
off by SIGKILL) were each followed, when the game was closed afterwards, by
`sceSystemServiceKillApp` blocking about 15 s, returning `0x80020003`, and
the console going down. Deferring keeps the game and its layers intact, so
that a copy started afterwards can adopt them (next paragraph); a game closed
while no UniMount is running still takes the console down. To stop UniMount
while such a game runs, close the game first, or use the STOP file (never
followed by a kill) and then close the game. After a kill, load UniMount
again before closing the game.

Kept or orphaned layers are listed in the session record. A later instance
that starts while the game still runs adopts them and removes them when the
game exits (see "Session record and adoption"); when the record is missing
or fails a check, it stays passive as before. The `/system_ex/app/<TID>`
stack is released by the scanner's normal cleanup once nothing uses it;
sandbox layers nobody adopted stay until ShellCore or a reboot removes the
sandbox.

Log lines (in `debug.log`; after the next start in `debug.log.1`):

| Line | Meaning |
| --- | --- |
| `[STARTUP] <TID> is running; waiting for it to close before starting` | Step 0 wait began (`a game` when the title is unknown). |
| `[STARTUP] stop requested while waiting for <TID> to close` | A signal or STOP file ended the wait; nothing was started. |
| `[STARTUP] <TID> closed; started after waiting <N> s` | First lines after the log rotation, once the wait ended. |
| `[RESTART] previous instance pid=<pid> deferred its stop for a running game; waiting for the game to close` | Step 1 timed out while a big app runs. |
| `[RESTART] previous instance pid=<pid> did not stop within 60 s; not starting alongside it` | Step 1 gave up; this copy exits. |
| `[SHUTDOWN] stop request <n> received (signal\|STOP file ...)` | Each new stop request. |
| `[SHUTDOWN] deferred: <TID> is running (signal, overlay layers; a SIGKILL that follows leaves them orphaned)` | First signal with an overlay session. |
| `[SHUTDOWN] deferred: <TID> is running (STOP file, <kind>)` | First STOP file with an active game; `<kind>` is `overlay layers`, `only directory aliases` or `no layers`. |
| `[SHUTDOWN] game closed; continuing the deferred shutdown` | The deferred stop proceeds. |
| `[SHUTDOWN] stopping now while <TID> is running (<reason>); its overlay layers are kept` | Second request or immediate stop with an overlay session. |
| `[SHUTDOWN] stopping now while <TID> is running (<reason>): <kind> in its sandbox, normal teardown` | Stop with a live alias-only (or layer-less) session: first signal, second request or immediate stop. |
| `  [GAME] stopping with a live game: <TID> pid=<pid>` | The lifecycle watcher stopped while the game process was alive. |
| `  [FAKELIB] shutdown kept <label> layer of live game <TID> pid=<pid>: <source> -> <target>` | One per kept sandbox layer. |
| `  [FAKELIB] shutdown removes the directory aliases of live game <TID> pid=<pid> (no overlay layers)` | Alias-only teardown under a live game. |
| `  [FAKELIB] shutdown retained <label> layer of <TID>: <source> -> <target> (<error>)` | Teardown of a stopped game's layer failed. |
| `  [BKP][NSFS] shutdown kept redirects of live game: title=<TID> pid=<pid> count=<n>` | Backport redirects kept. |
| `  [LINK] shutdown kept /system_ex/app/<TID> of live game` | Title stack kept; image teardown is skipped. |
| `[SHUTDOWN] cleanup complete for <signal\|STOP file ...\|reason>` | End of any shutdown that followed a stop request. |

## Session record and adoption

Run on hardware (test log, section M): the record for a debug `.pkg` title
and an image title, the dry run for both, and adoption after a SIGKILL for
both, including PS5 Payload Manager's kill during the debug `.pkg` title. Not
run on hardware: redirect sets, a writable `app0` cover, a same-application
replacement after adoption, any refusal, and a stop request during adoption.

### The record

The owning instance keeps `/data/shadowmount/sandbox-session.json`
(`src/sm_sandbox_record.c`, format in `src/sm_unimount_logic.c`). It lists
exactly what the owner removes when the game exits:

- every entry of the fakelib session that the owner's cleanup acts on, in
  session order:
  - each sandbox mount this instance made itself (`owned`): label (`game`,
    `global`, `fakelib2`, `directory`), file system type (`unionfs` or
    `nullfs`), source, mount path, `f_fsid` (both words), read-only flag and
    whether the owner created the mount directory;
  - each directory the owner created only as a parent of mount points (for
    example `preinst` and `preinst/common` above the font mount): label
    `parent`, type `none`, empty source, its path, `created` true. It is
    removed with `rmdir` after its children, as by the owner.

  Mounts that ShellCore made and UniMount only reused are not listed,
  because the owner does not unmount them either;
- every installed NSFS backport redirect set of the same title: backport
  root, the sandbox's NSFS `app0` path, its `st_dev` and `f_fsid`, and each
  redirect destination in creation order.

The top level holds `format` (1), `version`, `boot_time` (the
`kern.boottime` seconds that `owner.json` also stores), `owner_pid`,
`game_pid` (0 until the game process is known), `app_id` (AppInfo of
`game_pid`), `title_id`, `sandbox_app0`, `lib_mount` (the `common/lib` mount
path) and `complete` (false when a mount's `f_fsid` could not be read, or
the redirect destinations did not fit 64 KiB).

`sm_fakelib.c` and `sm_pkg_backport.c` hand a snapshot to the record module
whenever their session changes (on the same unlock that publishes the
session kind). That copy is the only work done under their locks. A writer
thread writes the file (temporary file, `fsync`, `rename`) and removes it
when nothing is left. A write error is logged once and otherwise ignored;
a launch never waits for the file. At startup the writer replaces or
removes a record left by an earlier instance. A clean shutdown removes the
record unless layers were kept for a live game; those stay recorded.

### Evaluation

Step 0 of the startup order evaluates the record when a managed game is
running. All of these must hold:

- the record parses (format 1, every field present and well formed, title
  id valid, every path inside `/mnt/sandbox/<TID>_NNN`, at most 24 mounts,
  2 redirect sets and 256 redirects per set, no string longer than
  `MAX_PATH`, a `parent` entry has no source and `created` true) and is
  `complete`;
- it lists at least one entry (a mount, a parent directory or a redirect
  set);
- `boot_time` equals this boot's;
- `game_pid` is set and alive, its AppInfo has the recorded `app_id` and
  title, and that `app_id` is the running big app (and its title, when the
  startup check could determine it);
- every recorded mount (not the `parent` entries, which are not mounts) is
  in the mount table exactly once by `f_fsid`, on
  the recorded path, with the recorded type and source (the owner's own
  source rule, allowing for `<above>:` and a source cut off at `MNAMELEN`),
  and is the topmost mount on that path;
- every redirect set's NSFS mount is present on its path with the recorded
  `f_fsid`.

With a live predecessor (today's wait) this is a dry run: one line,
nothing else changes. Without one, a failed check is logged and UniMount
waits passively as before. The record is evaluated on the first poll of
the wait, and once more if a predecessor that was alive disappears while
the game still runs.

### Adoption

When every check passes and no predecessor runs, the new copy adopts the
session and does nothing else (no takeover, no ShellCore hooks or service,
no scanner, no API):

1. It loads the recorded entries, in their order and with parent
   directories as entries without a mount, into its own fakelib session and the
   redirect sets into its backport sessions, takes the thread name
   `shadowmountplus.elf` (so a copy started after it sees a predecessor and
   does not adopt the same session again), restarts the record writer under
   its own pid, logs and notifies once.
2. It watches the game pid with `kqueue` (`EVFILT_PROC`, `NOTE_EXIT`) and a
   250 ms liveness poll in the main thread.
3. When the game exits it runs the owner's own `cleanup_fakelib_mount()`
   over the adopted entries: newest first, dependents (such as
   `app0/kraken`, or the font mount below `preinst/common`) before what
   they sit on, parent directories removed with `rmdir`, non-forced unmount after checking
   that the path's topmost mount still has the recorded `f_fsid` (a mount
   that no longer matches is never unmounted), each unmount logged. Busy
   mounts are retried every 250 ms for at most 10 s after the first busy
   pass, as the owner does.
4. Redirect sets are deleted with the owner's `cleanup_session_locked()`
   (which checks the NSFS namespace first) at the point where the owner
   finalizes a game exit: at least 1 s after the exit, and once the sandbox
   is gone or 10 s have passed. A process of the same application (same
   AppInfo `app_id` and title, another pid) found before then is a
   same-application replacement: it runs without the adopted mounts (they
   are not mounted again), and the redirect sets are left in place for it,
   as the owner would.
5. It logs the result, stops the writer (the record is removed when
   everything was removed; mounts that stayed busy remain recorded) and
   continues with the normal startup, which checks step 0 again from the
   top.

Stop requests while adopting follow the same rule as a running owner
(`sm_stop_decide`, with the adopted game active): a first signal or STOP
file defers the stop for an overlay session until the game has exited and
its layers are removed, then the copy exits instead of continuing. A signal
with an alias-only session removes the aliases at once (one non-forced pass)
and exits. A second request with an overlay session exits at once and keeps
the layers, which stay in the record for a later copy.

### Limits

- A game closed while no UniMount owns its layers (after the owner was
  killed and before a new copy started) still ends the way it did before
  this change: `sceSystemServiceKillApp` blocks about 15 s, returns
  `0x80020003` and the console goes down.
- Sessions of a version without the record (or with another record format)
  cannot be adopted: there is nothing to verify them against.
- A same-application replacement after adoption runs without the adopted
  mounts.
- A record written while the game was still starting (`game_pid` 0) or
  just before the owner was killed may lag behind the live state by the
  writer's delay. Such a record fails the checks and is refused; it never
  leads to a partial removal.

### Log lines

| Line | Meaning |
| --- | --- |
| `[ADOPT] session record written: <TID> pid=<pid> app_id=0x<id> layers=<n> redirect_sets=<m>` | The owner (or an adopting copy) wrote the record. `layers` counts every entry, parent directories included. |
| `[ADOPT] session record removed` | The record was removed because nothing is left to remove. |
| `[ADOPT] cannot write\|remove\|format /data/shadowmount/sandbox-session.json: <error>` | First record error of this process; later errors are not logged. |
| `[ADOPT] session record writer unavailable: <error>` | The writer thread could not start; no record is kept. |
| `[ADOPT] would adopt <n> layer(s) of <TID> pid=<pid>` | Dry run with a live predecessor: every check passed. `<n>` counts mounts plus one per redirect set; parent directories are not counted (a session of parent directories only shows 0). |
| `[ADOPT] refused: <reason>` | Evaluation failed (dry run or real); without a predecessor, the passive wait follows. |
| `[ADOPT] adopted <n> layer(s) of <TID> pid=<pid>` | Adoption started. |
| `[ADOPT] exit watch for pid=<pid> unavailable (<error>); polling` | `kqueue` could not watch the pid; only the 250 ms poll is used. |
| `[ADOPT] stop request <n> received (signal\|STOP file)` | Each new stop request while adopting. |
| `[ADOPT] stop deferred: <TID> is running (signal\|STOP file, <kind>)` | The stop waits for the game to exit and its layers to be removed. |
| `[ADOPT] stopping now while <TID> is running (<reason>); its overlay layers are kept in the session record` | Second request with an overlay session. |
| `[ADOPT] stopping now while <TID> is running (<reason>): <kind> in its sandbox, normal teardown` | Stop with an alias-only session. |
| `[ADOPT] teardown retained <k> layer(s) of <TID>: <error>` | That teardown left aliases behind. |
| `[ADOPT] <TID> pid=<pid> exited; removing the adopted layers` | The game exited. |
| `[ADOPT] busy sandbox cleanup scheduled: <TID>` | A mount was busy; retries follow. |
| `[ADOPT] sandbox cleanup wait ended; retaining <k> layer(s) of <TID>` | Mounts still busy after 10 s. |
| `[ADOPT] cleanup retained <k> layer(s) of <TID>: <error>` | A non-busy error stopped the retries. |
| `[ADOPT] busy redirect cleanup scheduled: <TID>` / `[ADOPT] redirect cleanup wait ended; retaining redirects: <TID>` / `[ADOPT] redirect cleanup retained for <TID>: <error>` | The same for redirect sets. |
| `[ADOPT] <TID> restarted as pid=<pid> (same-application replacement); it runs without the adopted layers` | A replacement process was found. |
| `[ADOPT] NSFS redirects of <TID> kept for pid=<pid>` | Its redirect sets are left in place. |
| `[ADOPT] released <TID>: <k> of <n> layer(s) removed in <ms> ms` | End of adoption; `<ms>` counts from the game's exit. |

The adopted cleanup also writes the owner's own lines:
`  [FAKELIB] <label> unmounted: <source> -> <target>` per mount,
`  [BKP][NSFS] redirects removed: title=<TID> count=<n> reason=adopted game exit`
(or `redirects released with namespace` when the sandbox is already gone).
The notification reads "UniMount: took over the layers of `<TID>`. They are
removed when the game is closed."

## Kraken-compatible listener (`[compat]`)

`kraken_api = on` (default), `kraken_api_port = 8097`. Bound only after the
takeover; if the port is taken the bind is retried every 10 s (logged once per
error). It closes during rest mode and reopens afterwards.

- `GET /api/state` - Kraken Mount's schema plus `"unimount": "<version>"`.
  `games` holds every library title backed by a Kraken PKG cache (`ready`,
  `path` = the `.pkg` file, which is what Orbit Store matches) and `.pkg` files
  the automatic mount has queued (`pending`), is building (`building`), skipped
  because another source provides the title (`duplicate`) or failed (`error`).
- `POST /api/rescan` - same full rescan as `/api/v1/scan`, 204.
- `POST /api/quit` - always 409 with a text body. A real Kraken Mount started
  later tries this, retries its bind for 5 s, logs "port 8097 busy" and stops.
- `GET /` - 302 to `http://<request host>:<api_port>/`.
- Everything else 404. Requests are capped at 16 KiB, one connection at a
  time on a non-blocking socket: the request must arrive within 2 s and the
  reply is abandoned after 2 s, and the stop flag is checked every 100 ms,
  so a slow client cannot hold the listener or delay shutdown. The scanner
  never waits on it.
- `paused` is true while a game runs or a background title blocks automatic
  work (the same rule that holds automatic builds back).

## Automatic mounting of debug PKGs (`[kraken] auto_mount`)

Default on. The scanner notes every regular `*.pkg` (any case) it meets in the
configured scan roots at the configured depth (not inside image mounts, not
SMB, not manual list entries). A worker thread, never the scan thread:

- waits for the stability wait (`[scan] stability_wait_seconds`);
- parses the header once per (path, size, mtime) and keeps only PS5 (`PPSA`)
  base PKGs that pass `sm_kraken_can_mount` (plaintext debug FIH). Retail,
  encrypted, PS4 and patch PKGs are ignored;
- skips titles another source already provides (library entry from a folder,
  image or another `.pkg`, or a registration it may not adopt);
- starts the same storage job as `POST /api/v1/packages/mount`
  (`run_kraken_job`: same guards, same progress in `/api/v1/games/storage/status`),
  one job at a time and never while a game runs: "a game is active" (a game
  process or a blocking background title) is checked again right before the
  job starts, and an automatic job is cancelled as soon as a game becomes
  active (retried later). Jobs requested through the API are not affected;
- remembers a failure keyed by (path, size, mtime). It is retried when the file
  changes, after an explicit `/api/v1/packages/mount` of the file, or after a
  restart. A storage-job cancel the user requested
  (`POST /api/v1/games/storage/cancel`) is remembered the same way
  (`error: cancelled_by_user`). Busy results and cancels caused by rest mode,
  shutdown or a game starting are retried on the next pass;
- tracks at most 64 `.pkg` files; further files are not mounted
  automatically, logged once and counted per scan pass in
  `auto_mount.overflow`.

Auto mount needs the API service (it owns the storage job); with
`[api] enabled = off` jobs are not accepted and nothing is mounted automatically.

### Adoption of titles real Kraken Mount registered

The job's `kraken_title_conflict` guard is relaxed for exactly this case:
`/user/app/<TID>/sce_sys/param.json` exists, `/user/app/<TID>/mount.lnk` does
not, `/user/app/<TID>/app.pkg` does not, no `app.pkg` for the title on
`/mnt/ext0|1/[ps5/]user/app`, and `/data/krakenmount/caches/<TID>/.done`
exists. UniMount then builds its own cache and registers the title again
(result 0x80990002 "already present" counts as success). While Kraken's own
`/system_ex/app/<TID>` mount is still present the job stops with
`kraken_legacy_mount_busy` and is retried. A genuinely installed copy is still
refused.

### Auto-remove

A Kraken-cache-backed title counts as missing only when its `.pkg` is
definitely absent (ENOENT/ENOTDIR), so `[library] remove_missing` treats a
removed `.pkg` like a removed image. An unreadable or corrupt cache marker, or
any other stat error, keeps the title (unknown is never "missing"); a cache
directory that is itself gone counts as missing.

## zstd-packed folder games (`[kraken] inject_ampr`)

Default on. A folder (or image) game whose root has `ampr_zstd.index` only runs
with AMPR-emu-NG. When its fakelib is mounted, UniMount builds
`/data/shadowmount/ampr-ng/<TID>/fakelib` from the fakelib it would have used
(or an empty one) and writes an AMPR-emu-NG `libSceAmpr.sprx` into it: a valid
shared one from `<emulators>/ampr-emu-ng/` or `<emulators>/` (same validation
as Kraken caches), else the bundled one. A fakelib that already ships
AMPR-emu-NG is used as is; folders with only `ampr_assets.index` are unchanged.
The overlay records what it was built from (`source.sig`: the fakelib tree
signature, the bundled revision and both shared AMPR candidates' size and
times) and is reused while that matches, so it is not recopied on every
launch. Overlays of titles that are no longer registered by UniMount (no
`mount.lnk`) are removed by the periodic fakelib cache cleanup unless mounted
or in use.

Emulator updates (`[emulators] update_enabled`) never replace an AMPR-emu-NG
`libSceAmpr.sprx` with an upstream ampr_emu file or the other way round
(ShadowMountPlus PR #200 rule; detected by the embedded `ampr-emu-ng ` string).
