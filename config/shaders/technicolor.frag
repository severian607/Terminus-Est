// label: Technicolor
// emoji: 🎞️
// category: Color · Grade
// tags: technicolor, two-strip, film, saturated, vintage
// description: Two-strip Technicolor look with rich reds and cyans and warm whites.
// source: original
// license: original
// order: 20
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float rs = c.r * 1.0 + c.g * 0.15;
    float gs = c.g * 0.9 + c.b * 0.3;
    vec3 red = vec3(1.0, 0.1, 0.05) * rs;
    vec3 cyan = vec3(0.05, 0.95, 0.85) * gs;
    vec3 col = red + cyan;
    col = mix(vec3(dot(c, vec3(0.299, 0.587, 0.114))), col, 0.9);
    col = mix(col, c, 0.25);
    col *= vec3(1.04, 1.0, 0.92);
    return clamp(col, 0.0, 1.0);
}
