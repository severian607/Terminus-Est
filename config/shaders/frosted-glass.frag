// label: Frosted Glass
// emoji: 🪟
// category: Distortion · Water
// tags: frosted, bathroom window, blur, diffuse, glass, cool
// description: Noise-scattered blur like frosted bathroom glass with a mild cool tint.
// source: original (per-pixel noise-scattered blur)
// license: original
// order: 70
// animated: no
// samples: yes

float fgHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = 1.0 / SCREEN_SIZE;
    vec2 fc = floor(uv * SCREEN_SIZE);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 14; i++) {
        float fi = float(i);
        float a = fgHash(fc + fi * 7.31) * 6.2831853;
        float r = sqrt(fgHash(fc.yx + fi * 3.77 + 11.0)) * 9.0;
        acc += texture(tex, clamp(uv + vec2(cos(a), sin(a)) * r * px, 0.0, 1.0)).rgb;
    }
    vec3 col = acc / 14.0;
    col = mix(col, col * vec3(0.88, 0.97, 1.08) + vec3(0.0, 0.01, 0.025), 0.8);
    return col;
}
