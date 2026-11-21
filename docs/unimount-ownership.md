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

1. An older `shadowmountplus.elf` (real ShadowMountPlus or an older UniMount)
   is asked to exit (SIGTERM) and awaited, as before. The thread is then named
   `shadowmountplus.elf`; a real Kraken Mount started later sees that name and
   does not mount anything.
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
