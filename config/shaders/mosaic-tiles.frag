// label: Mosaic Tiles
// emoji: 🧩
// category: Art · Tiles & Shapes
// tags: mosaic, tiles, grout, ceramic, bevel, square
// description: Square 14px ceramic tiles with grout lines, per-tile colour variation and a bevelled highlight.
// source: original
// license: original
// order: 110
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    float cell = 14.0;
    vec2 px = uv * SCREEN_SIZE;
    vec2 id = floor(px / cell);
    vec2 f = fract(px / cell);
    vec3 s = texture(tex, clamp((id + 0.5) * cell / SCREEN_SIZE, vec2(0.0), vec2(1.0))).rgb;
    vec3 h = fract(sin(vec3(dot(id, vec2(12.9898, 78.233)), dot(id, vec2(39.346, 11.135)), dot(id, vec2(73.156, 52.235)))) * 43758.5453);
    s *= 0.9 + 0.2 * h.x;
    s += (h.yzx - 0.5) * 0.06 * (0.3 + dot(s, vec3(0.33)));
    s = max(s, vec3(0.012));
    float g = 1.2 / cell;
    vec2 e = min(f, 1.0 - f);
    float edge = min(e.x, e.y);
    float tile = smoothstep(g, g + 0.05, edge);
    // bevel: light on top-left rim, shade on bottom-right rim
    float rim = 1.0 - smoothstep(g, g + 0.16, edge);
    float lit = (f.x + f.y < 1.0) ? 1.0 : -1.0;
    vec3 col = s * (1.0 + rim * lit * 0.45) + rim * max(lit, 0.0) * 0.05;
    vec3 grout = vec3(0.03, 0.03, 0.035);
    return mix(grout, col, tile);
}
