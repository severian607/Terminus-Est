// label: Matrix
// emoji: 🟩
// category: Overlay
// tags: matrix, green, terminal, phosphor, monochrome, hacker
// description: Monochrome green phosphor with contrast and glow.
// source: original
// license: original
// order: 100
// animated: no
// samples: no
// raw: no
// The glow reads one neighbouring pixel, so it is treated as colour-only (damage tracking stays on).

vec3 effect(vec3 c, vec2 uv) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    l = clamp((l - 0.5) * 1.4 + 0.5, 0.0, 1.0);
    float g = dot(texture(tex, uv + vec2(0.002, 0.0)).rgb, vec3(0.333));
    l = min(l * 1.15 + 0.1 * g * g, 1.0);
    return vec3(0.15, 1.0, 0.35) * l;
}
