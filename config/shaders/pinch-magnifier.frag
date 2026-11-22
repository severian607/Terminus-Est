// label: Magnifier
// emoji: 🔍
// category: Distortion · Lens
// tags: magnifier, bulge, lens, zoom, loupe
// description: A fixed round magnifying lens near the screen centre with a smooth bulge and soft rim highlight.
// source: original
// license: original
// order: 30
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    float asp = SCREEN_SIZE.x / SCREEN_SIZE.y;
    vec2 ctr = vec2(0.5, 0.48);
    vec2 p = uv - ctr;
    p.x *= asp;
    float R = 0.24;
    float d = length(p) / R;
    if (d >= 1.25) return c;
    float inside = 1.0 - smoothstep(0.97, 1.0, d);
    float k = 0.5 + 0.5 * d * d;               // 0.5x sample distance at centre = 2x zoom
    vec2 q = p * mix(1.0, k, inside);
    q.x /= asp;
    vec3 col = texture(tex, clamp(q + ctr, 0.0, 1.0)).rgb;
    col = mix(c, col, inside);
    float rim = smoothstep(0.93, 0.985, d) * (1.0 - smoothstep(0.985, 1.02, d));
    float lit = 0.5 + 0.5 * dot(normalize(p + 1e-5), normalize(vec2(-1.0, 1.0)));
    col = mix(col, vec3(0.75, 0.85, 1.0) * (0.35 + 0.65 * lit), rim * 0.6);
    float shadow = smoothstep(1.2, 1.0, d) * (1.0 - inside);
    col *= 1.0 - 0.35 * shadow;
    col += vec3(0.05, 0.07, 0.1) * inside * smoothstep(0.3, 0.9, d) * 0.5;
    return col;
}
