// label: Cinematic
// emoji: 🎬
// category: Color · Grade
// tags: cinematic, teal orange, film grade, contrast, movie
// description: Teal shadows, orange highlights and a soft S-curve contrast.
// source: original
// license: original
// order: 60
// animated: no
// samples: no

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}
vec3 effect(vec3 c, vec2 uv) {
    float l = luma(c);
    vec3 g = mix(c * vec3(0.88, 1.0, 1.12), c * vec3(1.12, 1.0, 0.86), smoothstep(0.2, 0.8, l));
    g = clamp(g, 0.0, 1.0);
    return mix(g, g * g * (3.0 - 2.0 * g), 0.5);
}
