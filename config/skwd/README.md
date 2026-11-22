# Skwd wallpaper picker look

Config lives in `~/.config/skwd-wall-v2/config.json` (not tracked; it holds an Unsplash key). Straight, square, unskewed grid. `theme.*` is untouched, so the accent still follows the wallpaper.

Changed in `components.wallpaperSelector` (and mirrored in `pickerLayouts.outputs.<DP-2 monitor>.values`, which overrides top-level values):

| key | old | new |
|---|---|---|
| displayMode | slices (override: slices) | wall |
| cornerRadius / gridCornerRadius | unset | 0 |
| gridRoundCorners | unset | false |
| gridBorderWidth | unset | 3 (selected thumbnail gets a 3px pale accent frame; 1px was barely visible) |
| gridStageRotation / ShearX / ShearY | unset | 0 |
| gridLayout | unset | grid |
| gridColumns / gridRows | unset | 5 / 3 |
| gridGapX / gridGapY | unset | 12 / 12 |
| gridThumbWidth / Height | unset | 320 / 180 |
| sliceParallax | true | false |
| showTypeBadges | unset | false (PIC/VID badges hidden) |
| gridSelectedScale | unset | 1.0 (no zoom on selection) |
| override sliceHeight / depthSpacingPx | 520 / 400 | unchanged (inert in wall mode) |
| savedLayouts "My layout" displayMode | depth | wall (parallax off, depth spacing removed) |

Other: `general.uiScale` 1.2 (unchanged), `filterBar.show.colors/arrange/swatch/tagcloud` unset -> false (rainbow strip, "arrange on screen", theme swatch, tag cloud hidden); `filterBar.visualStyle` stays "match". `filterBar.offsetX/offsetY` unset -> -268 / 0 (bar left-aligned to the grid edge; the bar keeps its own narrower width).

Not configurable (compiled in): font (no key; stays the built-in condensed sans, not monospace), filter bar button shapes and icons, type-badge style, colour of chrome (comes from the wallpaper scheme; no neutral-pinning without touching theme policy), selection highlight style, translucent grid backdrop.

## Tried, no use
- `frameEnabled/frameX/frameY/frameWidth/frameHeight`: only resize the thumbnail stage (the grid grew to 7x5 filling the frame). They draw no panel or outline, so they were reset to false/unset.
- `gridFilterBarOffsetX/Y`: no effect in wall mode; the top-level `filterBar.offsetX` is what moves the bar.
- No config key exists for a backdrop, dim, scrim or panel behind the picker, nor for the greenish halo in the gaps between thumbnails (wallpaper-tinted, compiled in). A black panel with outline is out of reach.
- `mode wall` logs "layout edit rejected: unsupported layout setting" but still switches; harmless.
