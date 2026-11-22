// label: CRT
// emoji: 📺
// category: Retro · CRT
// tags: crt, scanlines, curvature, phosphor, vignette, retro, tube
// description: Barrel curvature, scanlines, RGB phosphor mask and vignette.
// source: original
// license: original
// order: 100
// animated: no
// samples: yes
// raw: no

vec3 effect(vec3 c, vec2 uv) {
    // Barrel distortion. Past the curved edge, smear the border pixels and dim them
    // so the corners read as a dark phosphor glow instead of a black void.
    vec2 q = (uv * 2.0 - 1.0) * 0.94;
    q *= 1.0 + 0.06 * dot(q, q);
    vec2 over = max(abs(q) - 1.0, 0.0);
    vec2 p = clamp(q * 0.5 + 0.5, 0.0, 1.0);
    c = texture(tex, p).rgb * exp(-14.0 * dot(over, over) - 3.0 * (over.x + over.y));

    // Scanlines and RGB phosphor triads, tied to the real pixel grid.
    vec2 px = uv * SCREEN_SIZE;
    c *= 0.86 + 0.14 * sin(px.y * 3.14159265);
    vec3 mask = vec3(0.92);
    mask[int(mod(floor(px.x), 3.0))] = 1.08;
    c *= mask;

    // Brightness lift and gentle vignette.
    vec2 v = uv * 2.0 - 1.0;
    return c * 1.08 * (1.0 - 0.2 * dot(v, v) * 0.5);
}
