# UniMount

UniMount is one PS5 payload that does the work of ShadowMountPlus and Kraken Mount, so the two no longer have to be run, switched or autoloaded separately. It discovers games on the console, mounts them, registers them on the home screen and prepares them when launched.

> [!IMPORTANT]
> UniMount is an independent fork. It is not an official release of ShadowMountPlus or of Kraken Mount, and neither upstream project supports it. Report problems with it in this repository, not to the upstream authors.

It is built on:

- [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) 1.7 by drakmor: discovery, mounting of game folders and exFAT/UFS/PFS images, home-screen registration, the ShellCore launch hook, fakelib and backports, the PKG installer, SMB and the HTTP API on port 10101.
- The Kraken backend integration from [Vov4k957/shadowmountplus-kraken](https://github.com/Vov4k957/shadowmountplus-kraken): a small `no_std` Rust library (`kraken-bridge/`) around the package reader of quer3q's [Kraken Mount](https://github.com/quer3q/kraken-mount), vendored in `vendor/kraken` (Kraken Mount v0.3.1, ampr-emu-ng v0.5.0). It mounts plaintext debug `.pkg` files directly: a small cache per title under `/data/shadowmount/kraken/<TITLEID>` becomes `/app0` and the game reads its data from the `.pkg` through ampr-emu-ng. The package is not extracted or converted.

What UniMount adds on top:

- Ownership: a single instance; it takes over from a running Kraken Mount through that program's own quit request, releases its leftover mounts and adopts titles it had registered. It also answers on port 8097 in Kraken's API format, so Orbit Store's Kraken integration keeps working and a Kraken Mount started later backs off.
- Automatic mounting of debug `.pkg` files found in the scan roots.
- Launch diagnostics: stage timeline, outcome, the console's native error codes and a classification with evidence, plus a system report.
- Key-art, launch, close and running routes in the API.
- Execute-bit repair for folder games (upstream ShadowMountPlus pull request #226 by PhialsBasement).
- A new web interface (`web/index.html`).

Kept on purpose, for compatibility: the process name `shadowmountplus.elf`, the data directory `/data/shadowmount`, the config format and keys, API port 10101 and the existing API routes. Real Kraken Mount refuses to mount while a process named `shadowmountplus.elf` exists, which is what keeps it out of the way.

> [!WARNING]
> Mounting disk images can cause shutdown problems or data corruption, particularly on older firmware. Back up saves and important files before testing. A jailbroken PS5 is required.

## Status

Version 0.1.0 (`VERSION`). It was tested on one console: firmware 13.60, kstuff-lite 1.11, PS5 Payload Manager 0.6.3. The full record is in [`docs/hardware-test-log.md`](docs/hardware-test-log.md). In short:

- Verified: a plaintext debug `.pkg` title mounted through the Kraken path and launched, with its process alive past the confirmation window; a `.exfat` image title launched through the ShadowMountPlus path; takeover from a running Kraken Mount; refusal of a Kraken Mount started later; PS5 Payload Manager install, update and rest mode; PKG Manager running beside it; the web interface loaded from the console.
- Orbit Store: its Games-row app (`PPSA99177.ffpkg`) launched from UniMount and the compatible listener on port 8097 reported the mounted package. An actual Orbit Store download and hand-over was not tested.

Not validated (copied from the test log):

- Picture and sound of a running game (process-level evidence only).
- Folder games, `.ffpfs`, `.ffpfsc`, UFS game images, external USB and M.2 sources, SMB. These are ShadowMountPlus code that UniMount did not change, apart from the execute-bit repair for folder games.
- zstd- and LZ4-packed folders (host tests only).
- A failing launch on hardware: the classifier is tested on the host against real error-history entries, but no launch failed during the hardware runs.
- Orbit Store downloads and hand-over, and package installation through PKG Manager.
- Automatic-mount pausing when a game starts mid-build, user cancel of an automatic build, and a Kraken title whose `.pkg` is offline.

## Supported sources

| Source | How it is mounted | Verified on hardware |
| --- | --- | --- |
| Plaintext debug PS5 `.pkg` | Kraken path: cache in `/data/shadowmount/kraken/<TITLEID>`, data read from the `.pkg` | Yes, one title (about 34 GB) |
| `.exfat` image | ShadowMountPlus path: attached as a device and mounted at launch | Yes, one title (about 9.6 GB) |
| `.ffpkg` (UFS) image | ShadowMountPlus path | Only the Orbit Store app image; no UFS game image tested |
| `.ffpfs`, `.ffpfsc` image | ShadowMountPlus path | No |
| Game folder | ShadowMountPlus path, with execute-bit repair on writable storage | No |
| Folder or image with `ampr_zstd.index` | AMPR-emu-ng is added to its fakelib (`[kraken] inject_ampr`) | No (host tests only) |
| USB, M.2, SMB | Scan roots on USB/M.2; SMB is for file management and installation, not scanning | No |

Retail, encrypted and PS4 `.pkg` files are ignored by the automatic mount. Why debug packages go through the Kraken path on 13.60: kstuff-lite does not support installing debug packages above firmware 11.60. Layouts, scan locations and image creation are in [`docs/shadowmountplus-reference.md`](docs/shadowmountplus-reference.md).

## Requirements

- A jailbroken PS5 with [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) 1.07 or newer.
- Firmware: whatever ShadowMountPlus 1.7's ShellCore offset table covers (1.00 to 13.60). Only 13.60 has been tested.
- A way to load an ELF: PS5 Payload Manager, or any sender that can reach the console's payload port 9021.

## Install through PS5 Payload Manager

The step-by-step guide, with requirements, where to put games and troubleshooting, is [`docs/install.md`](docs/install.md). In short:

1. In PS5 Payload Manager's settings, enable "Multiple sources".
2. Add this custom repository URL:
   `https://raw.githubusercontent.com/leobbaroni/ps5-unimount/main/payloads.json`
3. UniMount appears in the list with its version. Install it, then load it.
4. To start it automatically, add the installed slot to the autoload list after kstuff-lite.

`payloads.json` and `icons.json` are generated at release time by `tools/make_pldmgr_manifest.py`; see [`docs/releasing.md`](docs/releasing.md). The release asset is named `unimount_v<version>.elf`.

## Manual loading

Download `unimount_v<version>.elf` from the releases page (the zip archive contains the same ELF as `shadowmountplus.elf`) and send it to the console's payload port 9021 after kstuff-lite is running. Place games in a scan location and wait for the background scanner.

## What happens on first start

- An older `shadowmountplus.elf` (real ShadowMountPlus or an older UniMount) is asked to exit and is awaited.
- If a real Kraken Mount is running, UniMount asks it to quit through its own quit request, waits until port 8097 is free, then releases Kraken's leftover mounts and adopts the titles it had registered. Nothing is scanned or mounted while that is pending. If a game is running under Kraken, UniMount waits.
- After takeover it writes the ownership ledger `/data/shadowmount/owner.json`, starts the compatible listener on port 8097, the automatic `.pkg` mount, the ShellCore hooks, the startup scan and the HTTP API.
- There is no need to run ShadowMountPlus or Kraken Mount, and they must not be autoloaded alongside UniMount. Stopping UniMount through the payload manager (SIGTERM) removes the ledger.

Details and the exact rules are in [`docs/unimount-ownership.md`](docs/unimount-ownership.md); the startup and shutdown order is in [`docs/unimount-architecture.md`](docs/unimount-architecture.md).

## Web interface

Open `http://<console-ip>:10101/` from a browser on the same network or from the PS5's own browser. The port is `[api] port` in `config.ini`.

PS5 browser, as measured on the test console (WebKit 605.1.15, Safari 17 level, viewport 1540x700, no `backdrop-filter`, no Gamepad API):

- The D-pad arrives as arrow keys and cross as a click.
- Circle arrives as Escape, after which the system closes the browser.
- Square, triangle, L1, R1 and Options were not measured.

> [!CAUTION]
> The API has no authentication. Use it on a trusted home network only and never expose it to the internet. `[api] allow_launch` gates the launch and close routes.

The routes are documented in [`docs/api.md`](docs/api.md) and [`docs/openapi.yaml`](docs/openapi.yaml).

## Compatibility notes

- Orbit Store: its Games-row app is `/data/homebrew/PPSA99177.ffpkg`, an image only ShadowMountPlus mounts. With Kraken Mount alone nothing mounts it, which is why switching mount payloads used to break Orbit Store. UniMount mounts it and also answers Orbit's Kraken polling on port 8097. A download and hand-over through Orbit Store has not been tested.
- PKG Manager 1.5.1 ran beside UniMount without conflicts. Installing a package through it could not be tested, because kstuff-lite does not support installing debug packages above firmware 11.60.
- PS5 Payload Manager: install from a custom repository, update in place and rest mode were verified (test log, section D). The repository used in that test was served from a PC on the LAN.
- Do not run the standalone BackPork payload while fakelib is enabled in UniMount.

## Configuration

Files live in `/data/shadowmount/`: `config.ini`, `profiles.ini`, `manual.lst`, `debug.log`. Accepted values are listed in [`config.ini.example`](config.ini.example) and [`profiles.ini.example`](profiles.ini.example). Sections that UniMount adds or extends:

| Section | Keys | Purpose |
| --- | --- | --- |
| `[api]` | `allow_launch` | Allow the launch and close routes (default on) |
| `[kraken]` | `auto_mount`, `inject_ampr` | Mount debug `.pkg` files found in scan roots; add AMPR-emu-ng to zstd-packed folder games |
| `[compat]` | `kraken_api`, `kraken_api_port` | Kraken-compatible listener (default on, port 8097) |
| `[diagnostics]` | `enabled`, `running_confirm_seconds`, `error_window_seconds` | Launch observation |

The remaining sections (`[general]`, `[scan]`, `[mount]`, `[library]`, `[fakelib]`, `[emulators]`, `[fan]`, `[sectors]`, SMB) are as in ShadowMountPlus; see the [reference](docs/shadowmountplus-reference.md). Do not remove the `[config]` `version` marker.

## Diagnostics

A game can be mounted and registered and still fail to start. UniMount records every observed launch as a stage timeline with an outcome (for example `running`, `exited_normally`), reads the console's own error history, and classifies a failure with the evidence behind it. Records are available from `GET /api/v1/diagnostics/launches` and in `last_launch` on library items.

`GET /api/v1/system` reports the firmware, kstuff, ShellCore hooks, mounting mechanisms and the limitations derived from them, each with a reason. Diagnostics only observe and never delay or change a launch. See [`docs/unimount-diagnostics.md`](docs/unimount-diagnostics.md).

## Building

On Linux or WSL: `tools/wsl-setup-toolchain.sh` installs the toolchain, `tools/wsl-build.sh` builds `build/unimount.elf`, and `tools/wsl-build.sh test` runs the cargo tests and every `tools/test_*.py`. clang 18 is used on purpose; the reason is in the comment at the top of `tools/wsl-build.sh`. CI is `.github/workflows/ps5.yml`; the host tests are listed in [`tests/README.md`](tests/README.md).

## Updating from upstream

```bash
bash tools/sync-upstream.sh smp
bash tools/sync-upstream.sh kraken <release-tag>
```

Run from a clean checkout. Build and test before committing. See [`docs/unimount-architecture.md`](docs/unimount-architecture.md#pulling-upstream-updates).

## Documentation

- [`docs/install.md`](docs/install.md): installation and setup, step by step.
- [`docs/unimount-architecture.md`](docs/unimount-architecture.md): design decision, components, lifecycles, ports and paths.
- [`docs/unimount-ownership.md`](docs/unimount-ownership.md), [`docs/unimount-diagnostics.md`](docs/unimount-diagnostics.md), [`docs/api.md`](docs/api.md)
- [`docs/shadowmountplus-reference.md`](docs/shadowmountplus-reference.md): formats, scanning, backports, image creation (upstream reference).
- [`docs/releasing.md`](docs/releasing.md), [`THIRD_PARTY.md`](THIRD_PARTY.md)

## Credits and licence

UniMount is GPL-3.0 ([`LICENSE`](LICENSE)). Components and their licences are in [`THIRD_PARTY.md`](THIRD_PARTY.md).

- ShadowMountPlus by drakmor and contributors; the original ShadowMount by VoidWhisper.
- Kraken Mount and ampr-emu-ng by quer3q.
- The Kraken backend integration by Vov4k957.
- dlc_emu and pgo_stub by drakmor.
- BackPork by BestPig.
- ps5-payload-sdk by John Törnblom.
- PS5 Payload Manager by itsPLK.
- Upstream pull request #226 (execute-bit repair) by PhialsBasement.
- kstuff-lite by EchoStretch; also Gezine, earthonion, LightningMods and RenanGBarreto (MkPFS), and the PS5 research community.

To support the engines UniMount is built on, use the donation and support pages of the upstream projects ([ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), [Kraken Mount](https://github.com/quer3q/kraken-mount)) directly.
