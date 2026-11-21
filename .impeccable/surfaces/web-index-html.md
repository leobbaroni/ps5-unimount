---
version: 1
slug: "web-index-html"
primary_target: "web/index.html"
related_targets: []
---

## Scope

`web/index.html`: the single-file interface embedded in the UniMount payload. Visitor mode: Operate.
Primary device: the PS5 browser on a TV, driven with a DualSense from the couch. Other LAN devices
are secondary.

## Task

See which games the console can run now, act on one (mount, unmount, configure, diagnose, remove),
add sources, and find out why a launch failed. Every function of the incumbent ShadowMountPlus
interface stays reachable, and its translation mechanism and strings are kept.

## Chosen direction

One Game, Whole Screen, in the owner's approved "cover shelf" composition
(`.impeccable/mocks/feed-shelf.png`, approved 2026-11-21). The build is code-led; the approved
mock-up is the composition the build matches and the critique reference at finish.

Owner's steer, binding: rectangular controls instead of circles; use the games' own artwork; the
whole library must be browsable from the home screen.

## Direction contract

THESIS: The focused game's own key art is the screen. UniMount is a few white rectangles laid over
it, and every other game is one step away on a cover shelf. It refuses the dark card-grid dashboard
where games are rows of badges.

OWN-WORLD: Full-bleed game art under a left and bottom scrim. White is the only interface colour;
one red marks failure. Rectangles with 6px corners, 1px white borders, solid white for focus. One
neo-grotesque system sans in two weights with tabular numerals. A five-segment stage tracker.

STORY: The owner reads the focused game's truth from found to running at a glance, moves along the
shelf to any other game, and acts without leaving the screen. A failed launch shows the console's
own code on the cover.

FIRST VIEWPORT: Top bar with wordmark, five section tabs and a right-aligned status line. Upper
left: title at display scale, facts line, stage tracker, one state sentence, then the action row
with the primary action solid white. Bottom fifth: "All games · N" and the cover shelf, focused
cover framed and enlarged. Legend line beneath.

SIGNATURE: Moving focus along the shelf cross-fades the full-bleed art and re-stamps the stage
tracker for the newly focused game. Motion grammar: one 240ms ease-out for focus and cross-fade;
nothing else moves; reduced motion makes it instant.

FORM: Full-bleed media surface with a cover shelf; third of the dealt challengers, steered by the
owner. seed: none (unavailable)

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance

## Unresolved

- The PS5 browser's engine level and controller input model are unverified. Until checked on the
  console, the interface must work by pointer alone and by sequential/arrow-key focus alone, and
  controller hints appear only once a controller input has actually been observed.
- A key-art endpoint does not exist yet; covers come from the existing icon endpoint until it does.
