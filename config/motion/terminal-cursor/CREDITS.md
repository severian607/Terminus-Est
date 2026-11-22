# Terminal cursor shaders: credits

Ghostty cursor shaders, all by upstream authors. Changes made here: the header comment block, and where a shader
hardcoded its colour, the colour now comes from Ghostty's `iCurrentCursorColor` uniform so it follows the theme.

| Entry | Upstream | Licence | Changes |
| --- | --- | --- | --- |
| warp | https://github.com/sahaj-b/ghostty-cursor-shaders (ghostty/cursor_warp.glsl) | MIT, Sahaj Bhatt | header only |
| sweep | same repo, ghostty/cursor_sweep.glsl | MIT, Sahaj Bhatt | header only |
| tail | same repo, ghostty/cursor_tail.glsl | MIT, Sahaj Bhatt | header only |
| ripple | same repo, ghostty/ripple_cursor.glsl | MIT, Sahaj Bhatt | colour = cursor colour |
| sonic-boom | same repo, ghostty/sonic_boom_cursor.glsl | MIT, Sahaj Bhatt | colour = cursor colour |
| rectangle-boom | same repo, ghostty/rectangle_boom_cursor.glsl | MIT, Sahaj Bhatt | colour = cursor colour |
| blaze | https://github.com/KroneCorylus/ghostty-shader-playground (public/shaders/cursor_blaze.glsl) | MIT, Krone Corylus | trail colour = cursor colour, accent = cursor colour pushed toward red |
| bendy-boo | https://github.com/hced/ghostty-cursor-trails (boo-cursor.glsl) | MIT, H. Cederblad | header only |
| tinkle | same repo, tinkle-cursor.glsl | MIT, H. Cederblad | trail head colour = cursor colour (was fixed blue) |
| smear | https://github.com/thamarnan/ghostty-shaders (shaders/cursor_smear.glsl) | no licence file upstream | adapted earlier for Terminus-Est; already uses the cursor colour |

Ripple, sonic-boom and rectangle-boom only fire when the cursor width changes (for example block to bar), not on plain
cursor moves; that is how the upstream shaders work.

The MIT licence text of each repository: "Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files, to deal in the Software without restriction, subject to the copyright
notice and this permission notice being included in all copies or substantial portions of the Software. The software is
provided as is, without warranty of any kind."
