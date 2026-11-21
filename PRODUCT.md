# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users

The owner of a jailbroken PlayStation 5 (firmware 13.60 with kstuff-lite) who keeps games on the
console's drives as extracted folders, disk images and debug packages.

Primary scene, confirmed by the owner: the PS5's own browser on the TV, opened from a home-screen
tile, operated with the DualSense from the couch. Other devices on the LAN can reach the same page,
but the TV is the surface the design answers to.

The job: see which games the console can run right now, get a new one onto the home screen, and
find out why a game that should launch does not.

## Product Purpose

UniMount is one PS5 payload that discovers games on internal and external storage, mounts them,
registers them on the home screen and prepares each launch. It replaces running ShadowMountPlus and
Kraken Mount separately, which cannot run at the same time.

Success means a game launches from the home screen, and when it does not, the interface names the
stage that failed and the console's own error code.

## Positioning

The only payload on the console that owns mounting and registration for every source type at once:
folders, exFAT/UFS/PFS images and plaintext debug `.pkg` files, without converting or expanding
them. It reports launch truth, not mount truth: a mounted game with a home-screen icon is not
reported as working until its process is seen running.

## Operating Context

- Loaded and auto-started by PS5 Payload Manager (PLDMGR); distributed through a PLDMGR custom
  repository.
- Runs beside Orbit Store, PKG Manager and BFpilot and must not disturb them. Orbit Store's own
  home-screen app is an `.ffpkg` image that UniMount mounts on launch.
- The interface is a single HTML file embedded in the payload and served by the console itself.
  The console may have no internet access.
- A game's path through the system has ordered stages: found, prepared, mounted, registered, launch
  requested, running or failed.

## Capabilities and Constraints

- Functions the interface covers: game library, automatic and manual scanning, mount and unmount,
  registration, package and image management, backports, fakelib settings, emulator settings,
  storage, per-game configuration, launch diagnostics.
- One self-contained file: no external fonts, scripts, images or network requests beyond the
  payload's own API.
- Must work in the PS5 browser. Its engine level and input model (pointer versus D-pad focus) are
  to be verified on the console; until then the interface must be fully usable by sequential focus
  alone and by pointer alone.
- The existing ShadowMountPlus interface ships translations for 31 languages; the redesign keeps
  the translation mechanism and existing strings. New strings fall back to English until
  translated.
- The management API has no authentication. It is for a trusted home network.
- Destructive actions (delete a source, uninstall a title) exist and must never be the easy mistake.

## Brand Commitments

- Name: UniMount.
- An independent fork. It is not an official release of ShadowMountPlus or Kraken Mount and must
  not read as one.
- GPL-3.0. Credits to ShadowMountPlus (drakmor and contributors), Kraken Mount (quer3q) and the
  bundled emulator libraries stay visible in the interface.

## Evidence on Hand

- The owner's console: LEGO Batman as a 34 GB debug `.pkg`, Hades II as an exFAT image, and Orbit
  Store's `.ffpkg` app. Real titles, real sizes, real error-history entries (for example
  CE-107750-0, CE-105779-9).
- The incumbent ShadowMountPlus interface at `web/index.html` and its API description in
  `docs/api.md` and `docs/openapi.yaml`.
- No user research, usage numbers or testimonials exist. None may be invented.

## Product Principles

1. Report what is true. Never show success for a stage that was not observed.
2. One owner. Every mount and registration on the console is visible here and attributable.
3. Leave things as found. Existing installations, game data and other payloads keep working;
   changes are reversible by default.
4. Readable from the couch. If a state cannot be understood from across the room, it is not shown
   clearly enough.

## Accessibility & Inclusion

Derived from the confirmed TV scene: text and state must hold up at viewing distance, focus must
always be visible, and state is never carried by colour alone. No formal standard was specified.
