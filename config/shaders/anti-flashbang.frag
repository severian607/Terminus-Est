// label: Anti-Flashbang
// emoji: 🕶️
// category: Comfort
// tags: flashbang, bright, white, tone map, dim whites, glare
// description: Compresses very bright areas so white pages stop blinding you.
// source: original
// license: original
// order: 40
// animated: no
// samples: no

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}
vec3 effect(vec3 c, vec2 uv) {
    float l = luma(c);
    float t = l < 0.6 ? l : 0.6 + (l - 0.6) * 0.35;
    return c * (t / max(l, 0.0001));
}
