# Hardware test log

Everything below was run on one console on 2026-11-21. The two game titles used as test content are
referred to as Title A and Title B; their title ids are replaced in quoted log lines. Results are stated as observed; anything
not observed is listed under "Not validated".

## Environment

| Item | Value | How it was read |
| --- | --- | --- |
| Console | PlayStation 5, firmware 13.60 | `GET /api/v1/system` → `firmware.display` |
| Kernel patches | kstuff-lite v1.11, loaded | KEKCALL_CHECK; version from PLDMGR's slot metadata |
| Loader | PS5 Payload Manager 0.6.3 → elfldr on 127.0.0.1:9021 | PLDMGR `/version`, process list |
| Other payloads running | BFpilot 0.4.4, Orbit Store 1.9.2 | PLDMGR process list |
| ShellCore hooks | installed, registration path `shellcore_bridge` | `GET /api/v1/system` |
| Build under test | UniMount 0.1.0, clang 18. Tests A–F ran on commit `c65ba80` (engine with review fixes); A, G and H were repeated on commit `f1c69a3`; A, G and I ran on the released build, commit `1fd2b8f` (reported as `1.7beta12-59-g1fd2b8`) | `POST /api/v1/version` |
| Test content | Title A, a 34.1 GB plaintext debug `.pkg`; Title B, a 9.6 GB `.exfat` image; the Orbit Store app, a 217 MB `.ffpkg` (PPSA99177); all in `/data/homebrew` | `POST /api/v1/games`, `/packages` |

Launches were started and closed over the LAN, first with the test-only helper in `tools/hwtest/`
and later with UniMount's own `POST /api/v1/games/launch` and `/close`. A launch counts as
successful only when the game's process was seen alive past the confirmation window and the
console wrote no new entry to its error history (`/system_data/priv/error/history`, 41 entries
before and after every test). Nobody was watching the TV during these runs, so picture and sound
were not checked.

## Results

### A. Title A from a debug `.pkg` (Kraken path) — pass

1. With no mount payload running, UniMount started, found the `.pkg` after the stability wait and
   adopted the title that Kraken Mount had registered:
   `[AUTO] building <TITLE_A> …`, `[TAKEOVER] <TITLE_A>: adopting the title registered by Kraken Mount`,
   `[KRAKEN] cache ready … -> /data/shadowmount/kraken/<TITLE_A>` (5 s),
   `[SHELLCORE] AppInstallAll bridge consumed: <TITLE_A> result=0x00000000`.
   The source stayed a single 34.1 GB file; nothing was extracted beyond the small cache.
2. Launch: `launch mount ready`, `fakelib2 mounted … -> …/common/lib`,
   `/data/homebrew -> /mnt/sandbox/<TITLE_A>_000/app0/kraken`, `[GAME] started: <TITLE_A> pid=114`.
   The process held about 3.0 GB and stayed alive for 75 s; the kernel-log monitor reported no
   loader error or fatal signal in its 60 s window.
3. Launch record: `outcome: running` after 20 s, every stage `ok`, `required_firmware 9.00` against
   console `13.60`; after closing, `exited_normally`, `alive_seconds 75.5`.
4. Close: all eight sandbox layers unmounted, `runtime layers released`.
5. Repeated on the final engine build, after the takeover in test F, and on the released build:
   same result (`running`, then `exited_normally`; error history unchanged).

### B. Title B from an `.exfat` image (ShadowMountPlus path) — pass

Launch mounted the image through LVD on demand (`Attached as /dev/lvd1`, `nullfs mounted … ->
/system_ex/app/<TITLE_B>`), the process started (about 8 GB) and ran for 45 s; closing released the
layers and detached the image (`unmount complete … error=0`). Repeated through the API route and
while PKG Manager was running.

### C. Orbit Store — partly validated

- Cause of the original breakage, confirmed from the console's files: Orbit Store's Games-row app is
  `/data/homebrew/PPSA99177.ffpkg`, registered and mounted on launch by ShadowMountPlus only. With
  Kraken Mount running alone nothing mounts it.
- Under UniMount the app launched from the `.ffpkg` and ran (process alive, `outcome: running`),
  and closed cleanly. Orbit Store's own service (port 34177) answered throughout every test.
- UniMount's Kraken-compatible listener on port 8097 reported the Title A package in Kraken's
  `/api/state` schema with `status: "ready"` and the package path.
- Not validated: an actual Orbit Store download and hand-over. Its API requires device pairing, and
  its stored install record for Title A still read "Waiting for Kraken Mount" because it had
  stopped polling before UniMount started. Opening Orbit Store on the console should refresh it.

### D. PS5 Payload Manager — pass

- Upload and launch through PLDMGR's API (`/manage:upload`, `/loadpayload:local/unimount`).
- Custom repository: a repository built by `tools/make_pldmgr_manifest.py` was added as a source,
  listed UniMount with its version and SHA-256, installed it into `repo/unimount@…` with the
  checksum verified, and loaded it from that slot.
- Update: the repository was bumped to a newer version; PLDMGR replaced the slot in place while the
  old process kept running, and loading the slot handed over cleanly (`previous_unclean: false`,
  one `shadowmountplus.elf` process, library unchanged, no duplicate titles).
- Rest mode: the console entered rest mode after about an hour idle and came back with every
  payload, including UniMount, still running and its API answering.
- The repository used for this test was served from a PC on the LAN. The public repository on
  GitHub serves the same manifest format; the v0.1.0 release asset was downloaded from GitHub and
  its SHA-256 matched `payloads.json`. Installing from the public repository on the console has
  not been run yet.

### E. PKG Manager — coexistence validated, installation not testable

PKG Manager 1.5.1 was installed from PLDMGR's official repository and started beside UniMount. Its
helper went through elfldr and registered its home-screen tile, its API answered (version, storage,
empty package list), UniMount launched Title B while it ran, and UniMount's library and listeners
were unchanged after it was stopped. An actual package installation could not be tested: kstuff-lite
does not support installing debug packages above firmware 11.60, which is the reason `.pkg` titles
go through the Kraken path.

### F. Ownership and conflicts — pass

- A newer UniMount stops the running one and takes over (seen on every redeploy).
- Real Kraken Mount 0.3.1 started while UniMount ran: it logged
  `port 8097 busy … not started`; UniMount logged `[COMPAT] refused POST /api/quit from 127.0.0.1`
  and stayed the owner.
- UniMount started while real Kraken Mount ran: `[TAKEOVER] Kraken Mount 0.3.1 accepted quit (HTTP 204)`,
  `port 8097 released`, `released legacy Kraken mount: /system_ex/app/<TITLE_A>`,
  `legacy Kraken mounts: released=1 left=0`; Kraken's log shows `quit requested by a newer copy`.
  Title A then launched from UniMount's own mount.
- Clean stop (SIGTERM from PLDMGR) removed `owner.json`.

### G. Interface and browser

- The interface is served by the payload at `http://<console>:10101/` and was loaded against the
  live console (real library, key art through `GET /api/v1/games/art`, first conversion of a 4K
  PNG about 0.35 s).
- PS5 browser, measured with `tools/hwtest/probe.html`: WebKit 605.1.15 at Safari 17 level, viewport
  1540x700 at device pixel ratio 1, no `backdrop-filter`, no Gamepad API. D-pad arrives as arrow
  keys, cross as a pointer click, circle as Escape (after which the system closes the browser).
  Square, triangle, L1, R1 and Options were not measured.
- Layout on the console's own browser, released build: the page was opened there and reported its
  own geometry (the method is now `tools/hwtest/layout_probe.py`). For each of the three titles, including a 40-character title and with the control
  legend showing, the document height equalled the 700 px viewport, the top bar stayed at the top
  and nothing scrolled; no script error was raised. Nobody looked at the TV, so this is measured
  geometry, not a visual check.

### H. Settings — pass

On the release build, `POST /api/v1/settings` returned the eight UniMount keys and
`POST /api/v1/settings/update` applied them without a restart: `api_allow_launch: false` made the
launch route answer `403 launch_disabled` at once; changing `compat_kraken_api_port` to 18097 and
back rebound the listener within two seconds; `diagnostics_running_confirm_seconds: 45` was
reported by the diagnostics route; an out-of-range port and a wrong type were rejected with 400 and
left the file unchanged. `config.ini` kept all 82 comment lines and the unrelated settings.

### I. Launch records across a payload restart — pass

Run on the released build after an earlier build had left one launch record permanently "in
progress" (the payload had been replaced seconds after that launch).

- On start, that stale record was resolved to `observation_interrupted`; the interface shows it as
  "Not observed" instead of "Starting".
- Title B was launched and the payload replaced eight seconds later, while the game kept running.
  The new instance logged `launch 9 … re-attached after restart`, the record gained an
  `observation_resumed` entry and reached `running`; the game's mount stayed in place (`unmount
  deferred … Device busy`). After the game was closed the record ended `exited_normally` with
  `exit not observed directly`, and the layers were released.

## Repeating these checks

The commands used by hand in these runs are scripted in `tools/hwtest/`, for a PC on the same LAN:

- `console_check.py --console <host> status` prints the running version, ownership state, the
  running game, the error-history count and each title's last launch.
- `console_check.py --console <host> deploy build/unimount.elf` uploads the build into a PS5 Payload
  Manager slot, loads it and waits for it to answer.
- `console_check.py --console <host> launch <TITLEID>` runs the launch test from A and B: it passes
  only when the record went `running` then `exited_normally` and the error history did not grow.
- `layout_probe.py --console http://<host>:10101` measures the interface in the console's browser;
  open its address there with `console_check.py --console <host> helper browse http://<pc>:8766/`,
  which needs the `unimount-hwtest` helper payload loaded.

Both scripts were checked against the development mock server only; the commands they wrap are
the ones recorded above.

## Not validated

- Picture and sound of a running game (process-level evidence only).
- Folder games, `.ffpfs`, `.ffpfsc`, UFS game images, external USB and M.2 sources, SMB: no such
  content was on the console. These paths are ShadowMountPlus code that UniMount did not change,
  apart from the execute-bit repair for folder games (upstream PR #226).
- zstd- and LZ4-packed folders (no such game available); covered by host tests only.
- A failing launch on hardware: the classifier is exercised by host tests built from this console's
  real error-history entries (CE-107750-0, CE-105779-9, CE-105773-3, CE-108262-9), but no launch
  failed during these runs.
- Orbit Store downloads and hand-over, and package installation through PKG Manager (see C and E).
- Installing from the public GitHub repository through PS5 Payload Manager, and a cold-boot
  autoload of UniMount (the console was not rebooted during these runs).
- Auto-mount pausing when a game starts mid-build, user cancel of an automatic build, and a Kraken
  title whose `.pkg` is offline.
