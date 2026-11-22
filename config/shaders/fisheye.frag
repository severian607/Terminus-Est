// label: Fisheye
// emoji: 🐟
// category: Distortion · Lens
// tags: fisheye, lens, barrel, bulge, wide angle
// description: Static barrel/fisheye lens warp, slightly zoomed to hide the edges.
// source: https://github.com/lostjared/shaders (fisheye, reworked)
// license: MIT
// order: 30
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 d = (uv - 0.5) * 0.85;
    vec2 p = 0.5 + d * (1.0 + 0.35 * dot(d, d));
    return texture(tex, clamp(p, 0.0, 1.0)).rgb;
}
