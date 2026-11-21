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
   - `GET http://127.0.0.1:8097/api/state` (2 s timeouts). The peer is real
     Kraken Mount when the body is a JSON object with a string `version`, an
     array `games` and no `unimount` key.
   - `POST /api/quit` with an empty body from loopback - exactly what a newer
     Kraken copy sends (`web.rs take_port`). Kraken answers 204 and exits
     200 ms later, or 409 while a game runs.
   - 2xx: wait until nothing listens on 8097 (polled every 250 ms, logged
     after 5 s, gives up waiting after 15 s and continues).
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
  time with 2 s socket timeouts and a 5 s request deadline; the scanner never
  waits on it.

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
  one job at a time and never while a game runs;
- remembers a failure keyed by (path, size, mtime). It is retried when the file
  changes, after an explicit `/api/v1/packages/mount` of the file, or after a
  restart. Busy/cancelled results are retried on the next pass.

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

A Kraken-cache-backed title counts as present only while its `.pkg` exists, so
`[library] remove_missing` treats a removed `.pkg` like a removed image.

## zstd-packed folder games (`[kraken] inject_ampr`)

Default on. A folder (or image) game whose root has `ampr_zstd.index` only runs
with AMPR-emu-NG. When its fakelib is mounted, UniMount builds
`/data/shadowmount/ampr-ng/<TID>/fakelib` from the fakelib it would have used
(or an empty one) and writes an AMPR-emu-NG `libSceAmpr.sprx` into it: a valid
shared one from `<emulators>/ampr-emu-ng/` or `<emulators>/` (same validation
as Kraken caches), else the bundled one. A fakelib that already ships
AMPR-emu-NG is used as is; folders with only `ampr_assets.index` are unchanged.

Emulator updates (`[emulators] update_enabled`) never replace an AMPR-emu-NG
`libSceAmpr.sprx` with an upstream ampr_emu file or the other way round
(ShadowMountPlus PR #200 rule; detected by the embedded `ampr-emu-ng ` string).
