---
name: UniMount
description: The focused game's own key art is the screen; the interface is a few white rectangles laid over it.
colors:
  ground: "#0b0b0c"
  ink: "#ffffff"
  ink-2: "rgba(255, 255, 255, .84)"
  ink-3: "rgba(255, 255, 255, .7)"
  line: "rgba(255, 255, 255, .9)"
  line-soft: "rgba(255, 255, 255, .34)"
  hairline: "rgba(255, 255, 255, .14)"
  fill: "rgba(12, 12, 14, .62)"
  fill-focus: "rgba(255, 255, 255, .16)"
  fill-solid: "#151517"
  sheet: "#101012"
  art-placeholder: "#1d1d20"
  on-ink: "#000000"
  ink-hover: "#e9e9e9"
  failure-red: "#c62828"
typography:
  display:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, 'Noto Sans', 'Noto Sans CJK JP', 'Noto Sans Thai', 'Noto Sans Arabic', sans-serif"
    fontSize: "5rem"
    fontWeight: 700
    lineHeight: 1.12
    letterSpacing: "-0.03em"
  headline:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, 'Noto Sans', 'Noto Sans CJK JP', 'Noto Sans Thai', 'Noto Sans Arabic', sans-serif"
    fontSize: "1.5rem"
    fontWeight: 700
    lineHeight: 1.3
  title:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, 'Noto Sans', 'Noto Sans CJK JP', 'Noto Sans Thai', 'Noto Sans Arabic', sans-serif"
    fontSize: "1.375rem"
    fontWeight: 700
    lineHeight: 1.3
  subtitle:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, 'Noto Sans', 'Noto Sans CJK JP', 'Noto Sans Thai', 'Noto Sans Arabic', sans-serif"
    fontSize: "1.125rem"
    fontWeight: 700
    lineHeight: 1.3
  body-lg:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, 'Noto Sans', 'Noto Sans CJK JP', 'Noto Sans Thai', 'Noto Sans Arabic', sans-serif"
    fontSize: "1.25rem"
    fontWeight: 400
    lineHeight: 1.3
  body:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, 'Noto Sans', 'Noto Sans CJK JP', 'Noto Sans Thai', 'Noto Sans Arabic', sans-serif"
    fontSize: "1.125rem"
    fontWeight: 400
    lineHeight: 1.3
  label:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, 'Noto Sans', 'Noto Sans CJK JP', 'Noto Sans Thai', 'Noto Sans Arabic', sans-serif"
    fontSize: "1rem"
    fontWeight: 400
    lineHeight: 1.4
  tag:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, 'Noto Sans', 'Noto Sans CJK JP', 'Noto Sans Thai', 'Noto Sans Arabic', sans-serif"
    fontSize: "0.9375rem"
    fontWeight: 700
    lineHeight: 1.4
    fontFeature: "'tnum'"
  path:
    fontFamily: "ui-monospace, 'SF Mono', Menlo, Consolas, 'Liberation Mono', monospace"
    fontSize: "0.9375rem"
    fontWeight: 400
    lineHeight: 1.4
rounded:
  control: "6px"
  mark: "4px"
  meter: "3px"
  nub: "2px"
spacing:
  gutter: "2.5rem"
  gutter-narrow: "1rem"
  xs: "0.375rem"
  sm: "0.625rem"
  md: "1rem"
  lg: "1.5rem"
components:
  button:
    backgroundColor: "{colors.fill}"
    textColor: "{colors.ink}"
    typography: "{typography.body}"
    rounded: "{rounded.control}"
    padding: "0 1.125rem"
    height: "2.75rem"
  button-focus:
    backgroundColor: "{colors.fill-focus}"
    textColor: "{colors.ink}"
  button-primary:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.on-ink}"
    rounded: "{rounded.control}"
    padding: "0 1.125rem"
    height: "2.75rem"
  button-primary-hover:
    backgroundColor: "{colors.ink-hover}"
    textColor: "{colors.on-ink}"
  button-action:
    backgroundColor: "{colors.fill}"
    textColor: "{colors.ink}"
    typography: "{typography.body-lg}"
    rounded: "{rounded.control}"
    height: "3rem"
  button-small:
    backgroundColor: "{colors.fill}"
    textColor: "{colors.ink}"
    typography: "{typography.label}"
    rounded: "{rounded.control}"
    padding: "0 0.75rem"
    height: "2.25rem"
  button-delete-confirm:
    backgroundColor: "{colors.failure-red}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
  tab:
    textColor: "{colors.ink-2}"
    typography: "{typography.body}"
    rounded: "{rounded.control}"
    padding: "0 1rem"
    height: "3rem"
  tab-selected:
    textColor: "{colors.ink}"
  input:
    backgroundColor: "{colors.fill-solid}"
    textColor: "{colors.ink}"
    typography: "{typography.body}"
    rounded: "{rounded.control}"
    padding: "0 0.875rem"
    height: "2.75rem"
  switch-track:
    backgroundColor: "{colors.fill-solid}"
    rounded: "{rounded.mark}"
    width: "3.25rem"
    height: "1.75rem"
  switch-track-on:
    backgroundColor: "{colors.ink}"
  shelf-card:
    backgroundColor: "{colors.fill-solid}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    width: "15rem"
  shelf-card-compact:
    backgroundColor: "{colors.fill-solid}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    width: "13rem"
  stage-mark:
    textColor: "{colors.ink}"
    width: "1.25rem"
    height: "1.25rem"
  stage-mark-failed:
    backgroundColor: "{colors.failure-red}"
    textColor: "{colors.ink}"
    rounded: "{rounded.meter}"
    width: "1.25rem"
    height: "1.25rem"
  failure-tag:
    backgroundColor: "{colors.failure-red}"
    textColor: "{colors.ink}"
    typography: "{typography.tag}"
    rounded: "{rounded.mark}"
    padding: "0.15rem 0.5rem"
  sheet:
    backgroundColor: "{colors.sheet}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    width: "56rem"
---

# Design System: UniMount

## Overview

**Creative North Star: "The Cover Is the Screen"**

UniMount is an Operate surface. Its primary device is the PS5's own browser on a TV, driven with a DualSense from the couch; the measured viewport there is 1540×700 at device pixel ratio 1. The focused game's key art fills that whole viewport, a dark scrim makes the left and bottom readable, and everything UniMount itself contributes is white: type, 1px outlines, and one solid white rectangle for the action that matters. Every other game is one step away on a cover shelf along the bottom.

The system is deliberately thin. There is no brand colour, no decorative surface, no illustration and no icon set beyond a handful of stroked line symbols. White is the only interface colour; one red marks failure. Controls are rectangles with small corners, never circles or pills. The game's own artwork is the only imagery, and where a game has none its initials stand in on a flat dark tile.

The technical frame is part of the design, not a footnote to it. The interface is a single self-contained file served by the console (no external fonts, images or requests) and must stay under 700 KB. The console browser is WebKit at Safari 17 level with no `backdrop-filter` and no Gamepad API: the D-pad arrives as arrow keys, cross as a pointer click, circle as Escape. So every state has to read from across the room, focus has to be unmistakable, and nothing may depend on blur, hover or a font download.

**Key Characteristics:**
- Full-bleed game art under a scrim; interface drawn in white on top of it.
- One solid white rectangle per screen for the primary action; everything else is outlined.
- A five-stage tracker of marks and words that states launch truth with glyph and word, not hue.
- Platform system sans in two weights, tabular numerals for facts and codes.
- Sized in rem against a root that scales with the viewport, so the TV layout holds at 1540×700, 1280×720 and 1920×1080.
- One 240ms motion, used for focus and the art cross-fade, and nothing else.

## Colors

Monochrome over photography: a near-black ground, white at a few opacities, and one red.

### Primary
- **Interface White** (`ink`): all text at full emphasis, the primary button fill, stage marks and the labels of done, in-progress and failed stages, focus outlines, the selected-tab underline, the checked switch, meter fill. Text on a white fill is pure black (`on-ink`).

### Secondary
- **Failure Red** (`failure-red`): the filled square behind a failed stage's cross, the console's error code inline in the state sentence, the error tag on a shelf cover, the error toast's border and marker. It always carries white text or a cross glyph. In the build it also fills the final "delete permanently" confirmation button and outlines the box that names what will be deleted; that is its only non-failure use.

### Neutral
- **Screen Ground** (`ground`): the page behind the art, and the solid left side of the scrim when a game only has portrait cover art.
- **Secondary White** (`ink-2`): supporting text: status line, stage labels not yet reached, list metadata, hints, the legend.
- **Tertiary White** (`ink-3`): placeholders, the off switch thumb, initials on an art-less cover.
- **Outline White** (`line`): the 1px border of buttons, inputs, sheets, meters.
- **Soft Outline** (`line-soft`): resting border of shelf covers, sub-navigation buttons and section dividers. **Hairline** (`hairline`) separates list rows.
- **Glass Fill** (`fill`): resting button fill, a translucent near-black that keeps a label readable over any art without blur. **Focus Fill** (`fill-focus`) replaces it on hover and focus.
- **Solid Fill** (`fill-solid`): inputs, selects, switch tracks and shelf-cover captions. **Sheet** (`sheet`) is the opaque dialog surface. **Art Placeholder** (`art-placeholder`) sits behind covers while art loads or is absent.
- **Pressed White** (`ink-hover`): the primary button under a pointer.

### Named Rules
**The One White Rule.** White is the only interface colour. Emphasis comes from opacity (100, 84, 70 percent), weight and solid-versus-outline, never from a second hue.

**The Failure Red Rule.** Red means something failed. It appears only where the console reported a failure, plus the single irreversible-delete confirmation. It is never a brand accent, a hover state or a decoration.

**The Never Colour Alone Rule.** Every state red or white carries is also carried by shape and words: a check, a play triangle, a progress glyph, a dash or a cross beside each stage's word, a dashed outline for pending meters, a bold status word with a cross on the cover.

## Typography

**Display Font:** the platform system sans (`system-ui`, with `-apple-system`, Segoe UI, Roboto, Helvetica Neue, Arial and Noto Sans families as fallback)
**Body Font:** the same family
**Label/Mono Font:** the platform monospace (`ui-monospace`, SF Mono, Menlo, Consolas), for filesystem paths and logs only

**Character:** One neo-grotesque voice in two weights, 400 and 700. It is the system face on purpose: the interface ships 31 languages offline, including CJK, Thai and Arabic, and a bundled webfont could neither cover them nor fit the file budget.

### Hierarchy
All sizes are rem. The root is `max(16px, min(1.25vw, 2.222vh))`: 16px at the primary 1540×700 viewport, 24px at 1920×1080.

- **Display** (700, 5rem, line-height 1.12 with a 0.04em bottom pad so descenders clear the clamp, -0.03em): the focused game's title, upper left of the home screen, two lines at most and always shown whole. On the TV layout the size is fitted, not fixed: it starts at 5rem (4rem for titles over 16 characters) and steps down to a 2rem floor until the title shows in full and the Library fits; titles over 44 characters track at -0.02em.
- **Headline** (700, 1.5rem, 1.3): section page titles; dialog titles run one step larger at 1.75rem. The wordmark is 1.5rem/700 at -0.01em.
- **Title** (700, 1.375rem, 1.3): the facts line under the game title, the shelf heading, drive names.
- **Subtitle** (700, 1.125rem, 1.3): block headings inside a page, list-row titles; shelf-cover names at 1.0625rem.
- **Body large** (400, 1.25rem, 1.3): the state sentence (two lines at most) and the labels of the hero action row.
- **Body** (400, 1.125rem, 1.3): buttons, tabs, inputs, lead paragraphs.
- **Label** (400, 1rem, 1.4): stage labels, status line, metadata, hints, legend, small buttons. Stage notes use it too, on the same line as the label.
- **Tag** (700, 0.9375rem, tabular): error codes on covers; inline tags drop to 0.8125rem.
- **Path** (monospace 400, 0.9375rem): filesystem paths, job routes; the log view is 0.875rem at 1.45.

### Named Rules
**The Two Weights Rule.** Only 400 and 700. No light, medium or black, no italics, no uppercase transforms, no letter-spaced labels. Negative tracking is reserved for the display title and wordmark.

**The Couch Floor Rule.** Anything the owner must read from the couch is 1rem or larger. Sizes below that are limited to tags, key caps and the log.

**The Tabular Rule.** Sizes, counts, timestamps, error codes and progress figures use tabular numerals so they do not jitter as they update.

## Layout

The TV layout is one fixed viewport with no page scroll: a 3.5rem top bar, a view that fills the rest, and a 2.25rem legend line. The horizontal gutter is 2.5rem everywhere.

- **Top bar:** wordmark, five section tabs, a right-aligned one-line status.
- **Home:** the hero stack sits upper left, capped near 46rem wide so the art stays visible on the right: title, facts line, stage tracker, one state sentence, action row. The shelf zone is pushed to the bottom: a heading with the position count ("All games · n of N"), the job strip while a storage job runs, filter, two selects and a rescan button on the same line, then a horizontally scrolling row of 15rem covers that bleeds through the gutter. Paging buttons overlay the row ends. A constant 0.5rem separates the shelf from the legend.
- **Fitting:** outside the reflow layout the Library is a fixed-height shell that never scrolls. When hero, shelf, job strip and legend do not fit, the title steps down one size at a time; at each size the covers are tried at 15rem first, then compacted to 13rem (the tracker and action row close up to 0.75rem and hero actions drop to 2.75rem). A larger title wins over full-size covers. The selected cover is kept scrolled into view.
- **Section pages:** a 15rem column of sub-navigation buttons, then a scrolling pane capped at 72rem. Blocks are separated by a soft 1px rule, not boxed. Settings pin a save bar to the bottom edge.
- **Dialogs:** a centred sheet (40, 56 or 72rem wide) over a 72 percent black veil, with head, scrolling body and right-aligned foot.
- **Rhythm:** 0.375rem and 0.625rem inside components, 1rem between siblings, 1.25 to 1.5rem between blocks.
- **Reflow:** at 900px wide or narrower, or any portrait aspect, the page scrolls; the gutter drops to 1rem; tabs become a fixed bottom bar; the art becomes a banner 60vw tall; action buttons wrap two per row at a 44px minimum height; the legend and paging buttons are removed; sub-navigation becomes a horizontal strip.
- **Direction:** every offset has a right-to-left mirror, including the scrim's reading side. The shelf paging buttons swap ends, so "previous" sits at the reading-direction start and "next" at its end, each chevron pointing outward.

**The One Viewport Rule.** On the TV the home screen fits 1540×700 with nothing below the fold. A new element has to displace something; it cannot extend the page. The title and covers give way first, through the fitting steps; nothing is cut off.

## Elevation & Depth

Flat. There are no drop shadows and no blur anywhere; the console browser has no `backdrop-filter`, and the system does not imitate one. Depth is three stacked layers: the art, a scrim of plain linear gradients (a short dark band under the top bar, a heavy bottom band under the shelf, a left-side band under the hero), and opaque or translucent near-black fills with white outlines. A faint noise tile (7 percent) dithers the gradients so they do not band on a TV panel. Away from the home screen the scrim becomes a uniform 90 percent black so pages read as text on dark, with the art only just present.

The one `box-shadow` in the build is a 2px spread ring with no offset and no blur, used as a second border on the selected shelf cover. It is a frame, not a shadow.

**The No Blur Rule.** Never rely on `backdrop-filter`, glass blur or soft shadows for legibility. If text must sit on art, the scrim or an opaque fill does the work.

## Shapes

Rectangles with small corners. Controls, covers, sheets, toasts and the log view take 6px. Marks inside or beside controls take 4px: tags, key caps, switch tracks, thumbnails. Meters and the failed stage mark take 3px and the switch thumb 2px. Borders are 1px white; 2px is used only to mark a current choice (the selected language, a pressed segment option, the current storage root). Dashed borders mean pending or not applicable.

Even the toggle is square: a rectangular track with a square thumb that jumps sides. Selection indicators are small filled squares, not dots.

**The Rectangle Rule.** No circles, no pills, no fully rounded ends. The largest radius in the system is 6px.

## Components

Controls are plain and confident: an outline, a label, an optional 1.25em stroked line icon to the left of the label.

### Buttons
- **Shape:** 6px corners, 1px white outline, 2.75rem minimum height, 1.125rem horizontal padding, label at Body size, never wrapping.
- **Default:** Glass Fill with white text. This is every button except the one primary.
- **Primary:** solid white with black 700 text and no visible outline. One per screen or dialog; on the home screen it is the action the focused game's state calls for.
- **Hover / Focus:** hover swaps to Focus Fill (primary to Pressed White) with no transition. Focus adds a 3px white outline offset 3px (4px on primary and on the delete confirmation) and the same Focus Fill.
- **Sizes:** hero actions are 3rem tall (2.75rem when the Library compacts) at Body large with an 8.5rem minimum width; small buttons are 2.25rem at Label size; icon-only buttons keep the height and square up.
- **Disabled:** 45 percent opacity.
- **Destructive:** an ordinary outlined button until the final step. Only the last, irreversible confirmation is filled red, and it follows a red-outlined box stating exactly what is deleted.

### Tabs and sub-navigation
- **Tabs:** text only at Body size in Secondary White; the selected tab is full white, 700, with a 3px white underline inset 1rem. Focus is a 3px white outline drawn inside the tab.
- **Sub-navigation:** full-width left-aligned buttons with a soft outline; the current one takes a full white outline and 700.

### Stage tracker (signature)
Symbols and text only, no boxes. Each of the five stages is a mark and a word on one line, about 1.75rem tall: a 1.25rem stroked glyph, the label at Label size, then an optional note after a middle-dot separator in Tertiary White. Stages sit in a wrapping row 1.5rem apart (mirrored for right-to-left). Done is a white check. Deferred ("on launch") is a white play triangle. In progress is a white progress glyph. Not reached, not needed and not observed are a dash in Tertiary White. Failed is the only filled mark: a 1.25rem Failure Red square with 3px corners carrying a white cross. Not observed (the last launch's observation was interrupted, its outcome is one the page does not know, or an in-progress record went stale) is the dim dash with the note "Not observed": never red, never a check. Labels of done, in-progress and failed stages are full white, the rest Secondary White; notes are full white. The tracker is re-stamped (a 240ms fade up from 35 percent) whenever the focused game changes. In the reflow layout stages wrap onto several lines 1rem apart, with label and note at 0.875rem. The same marks lead each 2.5rem row of the launch-record stage list in the diagnostics dialog.

### Cover shelf (signature)
- **Cover:** 15rem wide, 16:9 art above a 4.25rem caption on Solid Fill with a 700 name (two lines maximum) and a one-line status word. Soft outline at rest. Art that does not fit 16:9 is shown whole over a darkened, enlarged copy of itself.
- **Selected:** full white outline plus the 2px ring, scaled 1.06 from the bottom edge. Focus adds a 3px outline offset 5px.
- **Failed:** the status word turns full white 700 with a cross, and the error code sits top-right as a red tag.
- **Not observed:** the status word reads "Not observed" in Secondary White with no glyph and no tag; the state sentence says the launch was not observed to the end.
- **Compact:** when the Library compacts, covers are 13rem wide with 16:9 art; the caption is unchanged.
- **Behaviour:** moving along the shelf cross-fades the full-bleed art and re-stamps the tracker; this is the system's only motion.

### Inputs, selects and switches
- **Field:** Solid Fill, 1px white outline, 6px corners, 2.75rem tall; placeholder in Tertiary White. Focus is a 3px white outline offset 2px.
- **Select:** the same field with a small white caret drawn in CSS; no native chrome.
- **Switch:** a 3.25rem by 1.75rem rectangular track. Off: Solid Fill with a grey square thumb left. On: white track with a black square thumb right. No slide animation. The whole row is the hit target.

### Lists, meters and tags
- **List row:** 3.25rem minimum, hairline rule beneath, 700 title over Secondary White metadata, actions right-aligned.
- **Meter:** 0.75 to 1rem tall, 1px white outline, solid white fill; dashed outline when progress is unknown. A text figure always accompanies it.
- **Tag:** small outlined rectangle; solid white with black text for "active", solid red for failure.

### Sheets, toasts and the job strip
- **Sheet:** opaque Sheet fill, 1px white outline, 6px corners; head and foot divided by soft rules; a square close button top-right.
- **Toast:** bottom centre, opaque, outlined; an error toast takes a red outline and a small red square before the text.
- **Job strip:** progress for the single storage job sits in the shelf heading line (or docks under sub-navigation), with name, meter, figures and a cancel button. Its figure slot always shows a percentage, or the job's state word ("Measuring") while progress is unknown; remaining time appears only once it is known. At 1420px and narrower the meter shrinks to 3rem and the secondary figures drop; at 1300px and narrower the meter drops and the figure carries progress alone.

### Legend
A single line at the bottom of the TV layout pairing a line icon with a word for each input the console was measured to deliver (navigate, select, close). It lists only inputs that were measured.

## Do's and Don'ts

### Do:
- **Do** let the focused game's own artwork fill the screen, and use only that artwork as imagery.
- **Do** keep exactly one solid white primary button per screen or dialog; outline the rest.
- **Do** show focus as a 3px solid white outline with an offset, on every focusable element, with no dependence on hover.
- **Do** pair every red or white state with a glyph and a word.
- **Do** size in rem so the layout scales with the root, and check new work at 1540×700 first.
- **Do** keep everything inline: system fonts, inline SVG line symbols, data-URI textures. The file stays self-contained and under 700 KB.
- **Do** mirror every horizontal offset for right-to-left languages, and leave room for long translations (labels do not wrap; containers truncate with an ellipsis).
- **Do** make reduced motion instant: no cross-fade, no scale transition, no re-stamp.
- **Do** show a launch whose end was not observed, or whose outcome is unknown, as "Not observed": a dim dash and a neutral word, never red and never success.

### Don't:
- **Don't** introduce a second interface colour, a tinted surface, or red for anything other than failure and the final delete confirmation.
- **Don't** use circles, pills or radii above 6px, including for toggles, avatars, badges and progress.
- **Don't** use `backdrop-filter`, blur, glow or offset drop shadows; the console cannot render the first and the system does not want the others.
- **Don't** load a webfont, an external image, or any request beyond the payload's own API.
- **Don't** add a third weight, italics, uppercase transforms or letter-spaced labels.
- **Don't** animate anything except focus and the art cross-fade, and never longer than 240ms.
- **Don't** make a destructive action solid, red or first in a row before its final confirmation step.
- **Don't** depend on the Gamepad API or on buttons that were not measured on the console; arrow keys, a click and Escape are the whole input model.
