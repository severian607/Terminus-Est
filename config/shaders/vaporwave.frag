// label: Vaporwave
// emoji: 🌴
// category: Color · Grade
// tags: vaporwave, synthwave, pink, cyan, split-tone, aesthetic
// description: Pink and cyan split-tone with a soft pastel glow.
// source: original
// license: original
// order: 30
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 cyan = vec3(0.10, 0.85, 0.95);
    vec3 pink = vec3(1.00, 0.30, 0.75);
    vec3 tone = mix(cyan, pink, smoothstep(0.0, 0.8, l));
    vec3 col = mix(c, tone * (0.3 + 0.9 * l), 0.45);
    col = mix(vec3(l), col, 1.25);
    col += vec3(0.05, 0.0, 0.08) * (1.0 - l);
    col += pink * 0.08 * smoothstep(0.5, 1.0, l);
    return clamp(col, 0.0, 1.0);
}
