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
| Test content | Title A, a 34.1 GB plaintext debug `.pkg`; Title B, a 9.6 GB `.exfat` image; the Orbit Store app, a 217 MB `.ffpkg` (PPSA99177); from section N also Title C, a `.ffpfsc` image; all in `/data/homebrew` | `POST /api/v1/games`, `/packages` |

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

### J. Second session: the 0.1.1 candidate — one failure

Run later on 2026-11-21 with the scripts in `tools/hwtest/`, on the build of commit `115d549`
(reported as `0.1.1`, `v0.1.0-8-g115d54`). The error history held 43 files at the start: two
entries from the console's web browser had been added since the first session.

- Launch test (`console_check.py launch`): Title A, Title B and the Orbit Store app each went
  `running`, stayed up, closed, and ended `exited_normally`; the error history stayed at 43. Pass.
- Layout (`layout_probe.py`, the console's own browser, 1540x700): all three titles fit, with the
  stage line 28 px tall and the 40-character title at 56 px on two lines. Pass.
- Interrupted record: with Title B confirmed running, UniMount was stopped and the game closed
  while it was down. On the next start the record read `observation_interrupted` with one such
  timeline entry; after two more restarts the record was byte-identical. Pass.
- Quit, then stop: Title B was closed and UniMount stopped one second after the close returned. The
  record read `exited_normally` (the watcher had already finalised the exit). Pass.
- **Replacing UniMount while Title A (debug `.pkg`) was running: failure.** A second copy was
  loaded eight seconds after the launch. The game kept running and the new copy re-attached to it
  (`observation_resumed`, then `running`). The close request then failed (`close_failed`, the
  system refused to end the application), and within a minute the console stopped answering and
  left the network. The same sequence with Title B (an `.exfat` image) had passed in section I.
  The old copy's log confirmed the cause: eight seconds into the game its shutdown logged
  `[FAKELIB] directory unmounted` for the font, USB and `/data` layers and
  `[FAKELIB] fakelib2 unmounted: ... -> .../common/lib`, the library overlay a debug `.pkg` game
  runs on, while `app0/kraken` stayed mounted. The later close blocked for 15 s and returned
  `0x80020003`. After the restart the console's error history had one new entry, `CE-108262-9`.

### K. Third session: start, replacement and stop while a game runs

Run on 2026-11-21 after the console was restarted, on the builds of commits `2e3e1a6`, `920a1da`
and `8ceab2c` (the last reported as `0.1.1`, `v0.1.0-14-g8ceab2`). Since `2e3e1a6` a copy that
starts while one of UniMount's games is running waits until the game is closed before it does
anything, including replacing an older copy.

- Launch test on `8ceab2c`: Title B and the Orbit Store app; on `2e3e1a6` also Title A. Pass.
- Replacement with no game running: the old copy logged `stop request 1 received (signal)` and
  `cleanup complete for signal`, the new one `Previous instance stopped, continuing startup`. Pass.
- Replacement while Title B (image) ran: the new copy logged
  `[STARTUP] <TITLE_B> is running; waiting for it to close before starting`; the game kept running
  and closed normally; the old copy then stopped and the new one started
  (`started after waiting 24 s`). Pass.
- **Replacement while Title A (debug `.pkg`) ran, the case that failed in J:** same sequence, the
  game closed normally, the record ended `exited_normally`, no new error-history entry. Pass, on
  `920a1da` and again on `8ceab2c`.
- Found and fixed during these runs: a copy that had waited kept writing into the rotated log
  (`920a1da`), and the old copy announced a deferral for a game that had just closed (`8ceab2c`).
- **Stopping UniMount while Title A ran: failure.** One stop request was sent with PS5 Payload
  Manager's kill route. UniMount logged `stop request 1 received (signal)` and
  `deferred: <TITLE_A> is running` and left the game's layers alone, but Payload Manager sends
  `SIGKILL` 1.5 seconds after `SIGTERM`, so UniMount was killed with every layer still mounted.
  Closing the game afterwards returned `0x80020003` and the console went down again.

What these two failures establish: when a game started from a debug `.pkg` ends and no UniMount is
alive to remove its sandbox layers first, the console goes down, whether those layers were touched
(J) or left intact (K). Replacing UniMount no longer leads there. Stopping it with a payload
manager's kill during such a game still does, and cannot be prevented from inside a process that
is killed 1.5 seconds later. The same stop with Title B (image) running was run once on the earlier
build and was harmless (section J, "Interrupted record").

### L. Fourth session: the 0.1.1 release build — pass

Run on 2026-11-21 after the console was restarted, on the build of commit `2a44ad9` (reported as
`0.1.1`, `v0.1.0-16-g2a44ad`), the file published as `unimount_v0.1.1.elf`. Since `2a44ad9` a stop
request is handled by what the running game has in its sandbox: a game with only directory aliases
(image and folder titles) gets the normal teardown at once; a game with overlay layers (debug
`.pkg` titles) defers the stop until it is closed. The error history held 45 files before and
after every check.

- Launch test: Title A, Title B and the Orbit Store app each went `running`, stayed up, closed and
  ended `exited_normally`. Pass.
- Replacement with no game running. Pass.
- Replacement while Title B (image) ran: the new copy waited 27 s for the game to close. Pass.
- Replacement while Title A (debug `.pkg`) ran: the new copy waited 24 s; when the game was closed
  the old copy removed the `fakelib2` overlay and the alias layers, then stopped; the record ended
  `exited_normally`. Pass.
- Stop while Title B (image) ran, with PS5 Payload Manager's kill route: UniMount logged
  `stopping now while <TITLE_B> is running (signal): only directory aliases in its sandbox, normal
  teardown`, removed the three alias layers, left the busy `/system_ex/app` stack and the image
  attached, and logged `cleanup complete for signal`, all within the same second. The game kept
  running, was closed through the test helper while UniMount was down, and the console was still
  answering a minute later. On the next start the record read `observation_interrupted`. Pass.
- Layout (`layout_probe.py`, the console's own browser, 1540x700): all three titles fit. Pass.
- Install from the public repository: the `payloads.json` address was added as a source in PS5
  Payload Manager and UniMount v0.1.1 installed from it; the slot's checksum equalled the
  published one. Loading that slot replaced the test copy, and Title A then passed the launch
  test again. ShadowMountPlus and Kraken Mount were deleted from Payload Manager afterwards. Pass.

Not run: a stop while Title A runs (the known failure of K), a second stop request, and the STOP
file.

### M. Fifth session: session record and adoption (0.1.2) — pass

Run on 2026-11-21 in the same boot as L, on the build of commit `3648d86` (reported as `0.1.2`,
`v0.1.1-4-g3648d8`), the file published as `unimount_v0.1.2.elf`. Since `a6a9e68` the owning
instance records the mounts and directories it adds to a game's sandbox in
`/data/shadowmount/sandbox-session.json`, and a copy that starts while that game runs with no
UniMount alive adopts them. The error history held 45 files before and after every check.

1. Record, owner alive (first on `a6a9e68`, repeated on `3648d86`): during Title A the record
   held 8 entries (the `fakelib2` unionfs, four nullfs aliases including `app0/kraken`, three
   created directories), during Title B 6 (three aliases, three directories). After each close
   the record was gone and the launch record read `exited_normally`. Pass.
2. Dry run: a second copy loaded while each title ran logged `[ADOPT] would adopt 5 layer(s)` for
   Title A and `3 layer(s)` for Title B, then waited as before. Pass.
3. SIGKILL with no game running, then a new copy: `[OWNER] previous instance did not exit
   cleanly`, `[SHELLCORE] stale cave bridge detached`, hooks reinstalled; Title A then passed the
   launch test. Pass.
4. Title B running, UniMount ended with SIGKILL (test helper), new copy loaded:
   `[ADOPT] adopted 3 layer(s)`. The game was closed through the helper about 15 s later; the three
   aliases were unmounted in the same second and `[ADOPT] released ...: 3 of 3 layer(s) removed
   in 1024 ms` followed. The console kept answering and the copy went on to start normally. Pass.
5. Title A, the sequence that took the console down in K: `[ADOPT] adopted 5 layer(s)`; on close
   the four aliases and `fakelib2` were unmounted in the same second, `released ...: 5 of 5
   layer(s) removed in 1026 ms`, the console kept answering. Pass.
6. Title A again with PS5 Payload Manager's kill instead of the helper: the owner logged
   `deferred: ... (signal, overlay layers; a SIGKILL that follows leaves them orphaned)` and was
   killed; the new copy adopted 5 layers and released 5 of 5 in 1057 ms on close. Pass.
7. Afterwards Title A, Title B and the Orbit Store app each passed the launch test.

In 4 to 6 the launch record of the interrupted launch read `observation_interrupted`, as intended
for a launch whose end no UniMount watched as owner.

Not run: closing a game after a kill without loading UniMount again (the failure of K, which
adoption does not cover), a title with backport redirect sets or a writable `app0` cover, a
same-application replacement after adoption, a refused record, and a stop request during adoption.

### N. Sixth session: tile, waiting copies and a field failure (0.1.3) — pass

Run on 2026-11-22 in the same boot as L and M, on the build of commit `fa7f953` (reported as
`0.1.3`, `v0.1.2-1-gfa7f95`), the file published as `unimount_v0.1.3.elf`.

The failure that led to this build was found on 0.1.2 in normal use, from the console's log; it
was not staged. The Orbit Store app had been open in the background since the night before. A
second copy of UniMount was loaded (it waited, as designed), the running copy was then stopped
with a signal (it removed its three alias layers and exited, as designed), and two more copies
were loaded. Three passive copies named `payload.elf` waited for the app to close, no UniMount was
active, and every start from the home screen failed: the console recorded 16 `CE-105773-3` errors
(`0x80940033`), which took the error history from 45 to 61 files. Stopping the three copies,
closing the app through the helper and loading UniMount once restored it. Title C, a `.ffpfsc`
image installed the night before, then started on 0.1.2: process alive after 30 s,
`running_confirmed`, no new error.

Checks on `fa7f953`. The error history held 61 files before and after every check.

1. Update while a title runs (Title C under 0.1.2). First 0.1.3 copy: process
   `unimount-wait.elf`, `[ADOPT] would adopt 4 layer(s)`, `[STARTUP] a previous copy is alive and
   keeps working`, the existing notice. Second copy 7 s later: `[STARTUP] replaced waiting copy
   pid=...`; the first copy logged `stop requested while waiting` and was gone, one waiting
   process remained. After the title was closed through the API the waiting copy stopped 0.1.2
   and started as 0.1.3 (`started after waiting 33 s`). Pass.
2. Tile. On that start: `[SHELLICON] removed legacy tile: FAKE10101`, `AppInstallAll bridge
   consumed: UNMT00001 result=0x00000000`, `[SHELLICON] installed: UNMT00001`. `/user/app` then
   held `UNMT00001` with its `param.json` (name UniMount) and the 26,030-byte `icon0.png`, and no
   `FAKE10101`. Later starts logged `already installed: UNMT00001`. Pass for files and log; how
   the tile looks under Media was not observed from the PC.
3. The field failure repeated. Orbit Store app running, UniMount stopped with PS5 Payload
   Manager's kill (alias teardown, exit), 0.1.3 loaded: `[ADOPT] refused: no session record`,
   `[STARTUP] no live previous copy: UniMount is not active while ... (Orbit Store) is open` and
   the notice "UniMount is not active: Orbit Store is open. Close it to start UniMount. Until
   then games cannot be started." A second load replaced the first copy; one waiting process.
   When the app was closed through the helper the waiting copy started without another load
   (`started after waiting 21 s`) and the API answered 2 s later. Pass.
4. Adoption with a newer copy arriving (Title B). UniMount ended with SIGKILL; the first copy
   logged `adopted 3 layer(s)` (process `shadowmountplus.elf`), the second waited
   (`unimount-wait.elf`). On close: `released ...: 3 of 3 layer(s) removed in 1102 ms`,
   `[STARTUP] newer waiting copy pid=... takes over; exiting`, and the second copy started. Pass.
   The exiting copy also logs `stop requested while waiting for a game to close`, which is
   misleading in this case.
5. Adoption, Title A (debug `.pkg`). SIGKILL, new copy `adopted 5 layer(s)`; on close
   `released ...: 5 of 5 layer(s) removed in 1068 ms` and the console kept answering. Pass.
6. Title A, Title B, Title C and the Orbit Store app were each started through the API, reached
   `running_confirmed` (20 s) with every stage `ok`, and were closed through the API. Pass.

Not run: the reminder shown when the error history grows while a copy waits (nothing on the PC
side makes the console record an error; host tests only), removal of a Kraken Mount tile (none
was installed), a legacy tile that fails the check, a waiting copy of an older build when 0.1.3
is loaded (it is named `payload.elf` and is not replaced), and two loads at the same moment.

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
- `console_check.py --console <host> helper sigkill <pid>` sends SIGKILL to that process (the
  second half of PS5 Payload Manager's kill); the helper answers `ok` or `err <errno text>`.

Both scripts were checked against the development mock server only; the commands they wrap are
the ones recorded above.

## Not validated

- Picture and sound of a running game (process-level evidence only).
- Folder games, `.ffpfs`, UFS game images, external USB and M.2 sources, SMB: no such content was
  on the console. These paths are ShadowMountPlus code that UniMount did not change, apart from
  the execute-bit repair for folder games (upstream PR #226). One `.ffpfsc` image has
  process-level evidence since section N.
- zstd- and LZ4-packed folders (no such game available); covered by host tests only.
- A failing launch on hardware: the classifier is exercised by host tests built from this console's
  real error-history entries (CE-107750-0, CE-105779-9, CE-105773-3, CE-108262-9), but no launch
  failed while UniMount was running. The 16 failures in section N happened with no UniMount
  active, so no launch record was written for them.
- Orbit Store downloads and hand-over, and package installation through PKG Manager (see C and E).
- A cold-boot autoload of UniMount: it is in the autoload list after kstuff-lite, but no restart
  was run after that. After each of the two crashes Payload Manager came back with its autoload
  sequence aborted, so every payload was loaded by hand.
- Of the launch-record fixes of commit `20d3c44`: rebooting with a game running (the old record
  must read `observation_interrupted`, never resumed) and a same-application hand-off shortly after
  a launch (one record).
- Of the stop handling: a second stop request and the STOP file while a game runs.
- Of adoption: see the end of section M.
- Of the tile and the waiting copy (0.1.3): see the end of section N.
- Auto-mount pausing when a game starts mid-build, user cancel of an automatic build, and a Kraken
  title whose `.pkg` is offline.
