// label: Console CRT
// emoji: 🎮
// category: Retro · CRT
// tags: crt, console, snes, genesis, pixel, scanlines, game, retro, 240p, aperture
// description: A readable CRT with a slight console-pixel look: 4px blocks, scanlines per pixel row, a soft RGB mask, light glow and gently curved glass.
// source: original
// license: original
// order: 160
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    // Gently curved glass.
    vec2 d = uv - 0.5;
    vec2 p = 0.5 + d * (1.0 + 0.06 * dot(d, d) * 4.0);
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return vec3(0.0);

    // Slight pixel effect: 4 px blocks, blended 65/35 with the real pixels so text stays readable.
    vec2 size = SCREEN_SIZE;
    vec2 block = vec2(4.0);
    vec2 cell = (floor(p * size / block) + 0.5) * block / size;
    vec3 pix = texture(tex, cell).rgb;
    vec3 raw = texture(tex, p).rgb;
    vec3 col = mix(raw, pix, 0.65);

    // Light horizontal bleed, like a composite or RGB console signal.
    vec3 right = texture(tex, cell + vec2(block.x / size.x, 0.0)).rgb;
    col = mix(col, 0.5 * (col + right), 0.25);

    // One scanline per pixel row.
    float row = fract(p.y * size.y / block.y);
    col *= 0.78 + 0.22 * smoothstep(0.0, 0.35, row) * smoothstep(1.0, 0.65, row);

    // Soft RGB aperture mask on single screen pixels.
    float tri = fract(p.x * size.x / 3.0);
    vec3 mask = vec3(0.92) + 0.08 * vec3(step(tri, 0.34), step(0.34, tri) * step(tri, 0.67), step(0.67, tri)) * 3.0;
    col *= mask;

    // Glow on highlights, vignette and a mild punch in colour.
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    col += 0.04 * smoothstep(0.6, 1.0, luma);
    col *= 1.0 - 0.35 * dot(d, d) * 2.4;
    col = mix(vec3(luma), col, 1.08);
    return clamp(col, 0.0, 1.0);
}
