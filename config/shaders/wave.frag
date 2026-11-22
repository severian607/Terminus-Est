// label: Wave
// emoji: 🌊
// category: Distortion · Water
// tags: wave, ripple, wobble, water, sway
// description: Slow, gentle screen-wide ripple.
// source: original
// license: original
// order: 100
// animated: yes
// samples: yes
// raw: no

vec3 effect(vec3 c, vec2 uv) {
    vec2 p = uv;
    p.x += 0.004 * sin(p.y * 14.0 + time * 0.9);
    p.y += 0.003 * sin(p.x * 11.0 - time * 0.7);
    return texture(tex, clamp(p, 0.0, 1.0)).rgb;
}
