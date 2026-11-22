// label: Teal & Orange
// emoji: 🎬
// category: Color · Grade
// tags: hollywood, cinematic, teal, orange, blockbuster, grade
// description: Blockbuster grade with teal shadows, orange highlights and gentle contrast.
// source: original
// license: original
// order: 10
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 teal = vec3(0.0, 0.50, 0.58);
    vec3 orange = vec3(1.0, 0.55, 0.15);
    vec3 tint = mix(teal, orange, smoothstep(0.1, 0.9, l));
    vec3 col = c * mix(vec3(1.0), tint * 1.6, 0.45);
    col += tint * 0.04 * (1.0 - l);
    col = mix(col, col * col * (3.0 - 2.0 * col), 0.3);
    return clamp(col, 0.0, 1.0);
}
