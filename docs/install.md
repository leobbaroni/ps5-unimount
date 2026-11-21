# Installing and setting up UniMount

This guide takes a jailbroken PS5 from nothing to games on the home screen. It was written against
the one console UniMount has been tested on: firmware 13.60, kstuff-lite 1.11, PS5 Payload Manager
0.6.3. What was and was not verified is in [`hardware-test-log.md`](hardware-test-log.md).

## What you need

| Requirement | Notes |
| --- | --- |
| A jailbroken PS5 | Firmware 1.00 to 13.60 is covered by the ShellCore offsets inherited from ShadowMountPlus 1.7. Only 13.60 has been tested. |
| [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) 1.07 or newer | Must be running **before** UniMount starts. Without it games do not launch. Tested with 1.11. |
| A way to load an ELF | [PS5 Payload Manager](https://github.com/itsPLK/ps5-payload-manager) is recommended: it installs, updates and autoloads UniMount. Any sender that reaches the console's payload port 9021 also works. |
| Your games | In one of the supported forms below, on internal storage, M.2 or USB. |
| A home network (optional) | For the web interface from a phone or PC. The PS5's own browser works without one. |

## What you do not need

- **ShadowMountPlus.** UniMount is built on it and does everything it did.
- **Kraken Mount.** Its package reader is built in. Debug `.pkg` files are mounted automatically.
- **A separate AMPR emulation library.** AMPR-emu-ng is bundled for debug packages.

Remove ShadowMountPlus and Kraken Mount from autoload and do not start them next to UniMount. If
one of them is already running when UniMount starts, UniMount asks it to stop and takes over its
games. If Kraken Mount is started later, it finds its port taken and does not start.

Do not run the standalone BackPork payload while fakelib is enabled in UniMount.

## Install with PS5 Payload Manager

1. Open PS5 Payload Manager's settings and enable **Multiple sources**.
2. Add this repository address:

   ```
   https://raw.githubusercontent.com/leobbaroni/ps5-unimount/main/payloads.json
   ```

3. Refresh. **UniMount** appears under *Utilities & Tools* with its version. Install it. Payload
   Manager checks the download against the checksum in the repository.
4. Make sure kstuff-lite is running, then load UniMount.
5. Add it to autoload. Order matters:
   1. kstuff-lite
   2. UniMount
   3. anything that relies on mounted games, such as Orbit Store

   Remove ShadowMountPlus and Kraken Mount from the list.

To update later, install the new version from the same list and load it. The new copy stops the old
one and carries on with the same library.

If a game that UniMount mounted is running when you load the new version, the new copy waits,
shows a notification, and takes over once the game is closed.

> [!WARNING]
> Do not stop UniMount while a game started from a debug `.pkg` is running, for example with the
> kill button in PS5 Payload Manager. That game then has nobody to release its mounts, and closing
> it has taken the test console down (test log, sections J and K). Close the game first.

## Install by hand

Download `unimount_v<version>.elf` from the
[releases page](https://github.com/leobbaroni/ps5-unimount/releases) and send it to port 9021 after
kstuff-lite is running, for example:

```sh
socat -t 99999999 - TCP:<console-ip>:9021 < unimount_v<version>.elf
```

Nothing is installed permanently this way: send it again after every jailbreak.

## Put games where UniMount looks

UniMount scans these folders, one level deep, every 15 seconds:

```text
/data/homebrew/
/data/etaHEN/games/
/mnt/usb0/homebrew/          (usb0 to usb7)
/mnt/usb0/etaHEN/games/
/mnt/ext0/homebrew/          (ext0 and ext1)
/mnt/ext0/etaHEN/games/
```

| What you have | What to do |
| --- | --- |
| A plaintext debug PS5 `.pkg` | Copy the file as it is. It is not installed or unpacked; a small cache is built and the game reads from the file. |
| A disk image (`.ffpkg`, `.exfat`, `.ffpfs`, `.ffpfsc`) | Copy the file. The game's files must be at the root of the image. Create `.exfat` images with a 64 KB allocation unit. |
| An extracted game folder | Copy the folder, with `eboot.bin` and `sce_sys/` directly inside it. |

A new item is picked up once it has stopped changing for ten seconds, so a large copy is not
touched while it is still in progress. Retail, encrypted and PS4 `.pkg` files are ignored.

Other folders can be added in the web interface under **Sources**, or as `[scan] path` lines in
`/data/shadowmount/config.ini`.

## First start

1. UniMount stops an older copy of itself, ShadowMountPlus or Kraken Mount if one is running.
2. It scans the folders above and registers what it finds. A notification reports the number of
   games.
3. The games appear on the home screen. Start them there as usual. Images are attached when a game
   is launched and released when it is closed.

## The web interface

Open `http://<console-ip>:10101/` in a browser on the same network, or in the PS5's own browser.

- **Library** shows every game with its artwork. Under the title, one line shows how far the
  selected game has got: Found, Prepared, Mounted, Registered, Running.
- **Diagnostics** on a game shows its last launches stage by stage, with the console's own error
  code when one was recorded. A game with an icon is not reported as working until its process has
  been seen running.
- **Sources**, **Storage** and **Settings** cover scan folders, images and packages, drives, and
  the options in `config.ini`.

In the PS5 browser the D-pad moves, cross selects and circle closes the browser.

> [!CAUTION]
> The interface and its API have no password. Anyone on your network can use them, including the
> routes that start and close games. Keep the console on a trusted home network, or set
> `[api] bind_address = 127.0.0.1` to restrict it to the console itself.

## Working with other payloads

- **Orbit Store.** Its home-screen app is an image that UniMount mounts, and UniMount answers on
  the port Orbit Store uses to talk to Kraken Mount. Load UniMount before Orbit Store. A download
  handed over from Orbit Store has not been tested.
- **PKG Manager** and **BFpilot** ran beside UniMount without conflict.
- **Kraken Mount, ShadowMountPlus:** not needed; see above.

## Settings worth knowing

Settings live in `/data/shadowmount/config.ini` and can be changed from the web interface. All have
working defaults.

| Setting | Default | What it does |
| --- | --- | --- |
| `[kraken] auto_mount` | on | Mount debug `.pkg` files found in the scan folders without being asked |
| `[api] allow_launch` | on | Allow the web interface to start and close games |
| `[api] bind_address` | `0.0.0.0` | `127.0.0.1` restricts the interface to the console |
| `[compat] kraken_api` | on | Answer Orbit Store's Kraken requests on port 8097 |
| `[diagnostics] enabled` | on | Record launches; it only observes and never delays a launch |
| `[scan] path` | built-in folders | Repeat to replace the built-in scan folders |

The full list is in [`config.ini.example`](../config.ini.example).

## When something does not work

- **A game has an icon but does not start.** Open its **Diagnostics** in the web interface. It
  names the stage that failed and the console's error code. The full log is
  `/data/shadowmount/debug.log`.
- **Nothing appears on the home screen.** Check that kstuff-lite was loaded first, and that the
  game sits directly inside one of the scan folders, not one level deeper.
- **The web interface does not open.** Check the console's address and that `[api] enabled` is on
  and `bind_address` is not `127.0.0.1`.
- **Kraken Mount reports that its port is busy.** That is UniMount keeping it out. Remove Kraken
  Mount from autoload.
- **After a restart of the console** everything has to be loaded again, as with any payload. With
  autoload set up as above this happens by itself after the jailbreak. Rest mode does not stop
  UniMount.

## Removing it

Remove UniMount from autoload and delete it in PS5 Payload Manager. Its settings, cache and log stay
in `/data/shadowmount/`; delete that folder to remove them. Games it registered stay on the home
screen until they are removed there or from the web interface.
