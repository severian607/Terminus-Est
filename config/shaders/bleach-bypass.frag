// label: Bleach Bypass
// emoji: 🧂
// category: Color · Grade
// tags: bleach bypass, desaturated, gritty, film, contrast
// description: Desaturated, high-contrast gritty film look.
// source: original
// license: original
// order: 50
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 base = vec3(l);
    vec3 mixed = mix(2.0 * base * c, 1.0 - 2.0 * (1.0 - base) * (1.0 - c), step(0.5, base));
    vec3 col = mix(c, mixed, 0.8);
    col = mix(vec3(l), col, 0.55);
    col = (col - 0.5) * 1.15 + 0.5;
    return clamp(col, 0.0, 1.0);
}
