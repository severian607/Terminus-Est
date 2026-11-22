# Shader credits

Effects marked `source: original` were written for Terminus-Est. Everything else is
taken or adapted from the permissively licensed projects below; each file's
`// source:` / `// license:` header names its origin.

## rokokol/hyprland-screen-shader (MIT)
<https://github.com/rokokol/hyprland-screen-shader>
Also the origin of the composable `effect(vec3 c, vec2 uv)` file format.
Effects: `grayscale`, `sepia`, `cool`, `posterize`, `reading`, `warm`, `vignette`,
`sharpen`, `jpeg`.

## huangj1e/Hyprland-CRT-Shader (MIT)
<https://github.com/huangj1e/Hyprland-CRT-Shader>
Effects: `analog-tv` (verbatim, default constants).

## LordHerdier/hyprCRT-shader (Unlicense)
<https://github.com/LordHerdier/hyprCRT-shader>
Effects (verbatim): `tube-crt` (`crt.frag`), `crt-subtle`, `crt-trinitron`,
`crt-arcade`, `crt-green`, `crt-amber` (`presets/*.frag`).
These need `cursor:no_hardware_cursors = true` to look right; the green, amber and
arcade presets add halation and cost more GPU time.

## lostjared/shaders (MIT)
<https://github.com/lostjared/shaders>
Ported from a webcam-tool uniform layout to composable effects (audio, mouse and
history uniforms removed, strengths reduced where noted in the header):
`amber-mono` (game_amber_mono), `old-film` (old-film, vhs-code-worn-tape),
`glitch-blocks` (glitch1, idea), `vhs-tape` (vhs, idea), `vhs-tracking`
(vhs-code-tracking-roll), `vhs-dropout` (vhs-code-dropout), `vhs-head-switch`
(vhs-code-head-switch), `heat-haze` (heat-wave), `water-ripple` (ripple), `fisheye`,
`mirror` (mirror1), `thick-glass`, `neon-spiral` (neon), `kaleidoscope` (kale),
`pixelate` (pixels), `anime-cel` (game_anime_cel).

## sijan-dev/HyprShades (no licence)
<https://github.com/sijan-dev/HyprShades>
No licence is published, so no code was copied. `amoled-mono` and
`blue-light-filter` are independent reimplementations of the idea
(`source: original`).

## Original
`scanlines`, `phosphor-green`, `gameboy`, `datamosh`, `rgb-split`, `static-snow`,
`channel-tear`, `vibrant`, `cinematic`, `duotone-sakura`, `amoled-mono`,
`blue-light-filter`, `anti-flashbang`, `night-dim`, `calm-colors`, `amoled-black`,
`hue-cycle`, `solarize-rainbow`, `plasma-overlay`, `prism-orbit`, `halftone`,
`edge-outline`, `bloom-glow`, `sakura-bloom`, `night-vision`,
`chromatic-aberration`, `film-grain`, `thermal`. The original eight (`lsd`, `crt`,
`sakura`, `invert`, `glitch`, `matrix`, `wave`, `cyberpunk`) are also original.

## Shadertoy-derived / ported (Shadertoy default licence, personal use)
`ascii` (movAX13h, <https://www.shadertoy.com/view/lssGDj>), `oil-paint` (Kuwahara
filter), `stained-glass` (iq's Voronoi borders), `hex-mosaic`, `underwater` (caustic
loop after Dave_Hoskins / joltz0r), `chalkboard` (hatching idea after flockaroo).
`bayer-dither`, `blueprint` and `tilt-shift` are original.

## Second batch (30 effects)
Written for Terminus-Est from well-known techniques; `source:` headers name any
Shadertoy origin. `rain-on-glass` simplifies the drop layers of Heartfelt by BigWings
(<https://www.shadertoy.com/view/ltffzl>). All others are `source: original`.
Categories were regrouped into nested `Retro · …`, `Color · …`, `Distortion · …` and
`Art · …` sub-categories, matching the Super+A `Windows · …` scheme (`lib/screenfx.py`).
