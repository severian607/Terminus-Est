// label: Gradient Map
// emoji: 🔥
// category: Color · Mono & Tint
// tags: gradient map, palette, magma, luminance, ramp
// description: Maps brightness onto a black, purple, orange, cream magma-like ramp.
// source: original
// license: original
// order: 20
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float l = clamp(dot(c, vec3(0.299, 0.587, 0.114)) * 1.15, 0.0, 1.0);
    vec3 a = vec3(0.00, 0.00, 0.02);
    vec3 b = vec3(0.32, 0.07, 0.43);
    vec3 d = vec3(0.86, 0.26, 0.28);
    vec3 e = vec3(0.99, 0.62, 0.25);
    vec3 f = vec3(0.99, 0.94, 0.75);
    vec3 col = mix(a, b, smoothstep(0.0, 0.25, l));
    col = mix(col, d, smoothstep(0.25, 0.5, l));
    col = mix(col, e, smoothstep(0.5, 0.75, l));
    col = mix(col, f, smoothstep(0.75, 1.0, l));
    return col;
}
