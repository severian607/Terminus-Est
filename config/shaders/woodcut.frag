// label: Woodcut
// emoji: 🪵
// category: Art · Paint & Ink
// tags: woodcut, linocut, engraving, ink, lines, print
// description: Black ink carved lines on warm paper whose thickness follows brightness: bright content gets more ink, so light text on dark UIs prints bold.
// source: original
// license: original
// order: 40
// animated: no
// samples: yes

float wcut_lum(vec2 p) {
    return dot(texture(tex, clamp(p, vec2(0.0), vec2(1.0))).rgb, vec3(0.299, 0.587, 0.114));
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = uv * SCREEN_SIZE;
    float lum = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
            lum += wcut_lum(uv + vec2(float(i), float(j)) * 1.5 / SCREEN_SIZE);
    lum /= 9.0;
    lum = clamp(lum * 1.8, 0.0, 1.0);

    float period = 6.0;
    float s = px.y + 7.0 * sin(px.x * 0.035 + px.y * 0.01) + 4.0 * sin(px.x * 0.013 - 1.3);
    float d = abs(fract(s / period) - 0.5) * 2.0; // 1 at line centre gap, 0 at line centre
    float thick = 0.06 + 0.9 * smoothstep(0.02, 0.85, lum);
    float ink = 1.0 - smoothstep(thick - 0.12, thick + 0.04, d);
    ink = max(ink, step(0.97, lum) * 0.0);

    // carved contour where brightness changes sharply
    vec2 t = 1.2 / SCREEN_SIZE;
    float gx = wcut_lum(uv + vec2(t.x, 0.0)) - wcut_lum(uv - vec2(t.x, 0.0));
    float gy = wcut_lum(uv + vec2(0.0, t.y)) - wcut_lum(uv - vec2(0.0, t.y));
    float contour = smoothstep(0.15, 0.4, length(vec2(gx, gy)));
    ink = max(ink, contour);

    float fibre = fract(sin(dot(floor(px * vec2(0.5, 2.0)), vec2(12.9898, 78.233))) * 43758.5453);
    vec3 paper = vec3(0.93, 0.86, 0.72) * (0.96 + 0.06 * fibre);
    return mix(paper, vec3(0.05, 0.035, 0.03), ink);
}
