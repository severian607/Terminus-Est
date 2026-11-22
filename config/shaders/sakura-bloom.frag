// label: Sakura Bloom
// emoji: 🌺
// category: Light & Finish
// tags: sakura, bloom, pink, glow, plum, dreamy, mochi
// description: Pink-tinted bloom with plum-lifted shadows, matching the Sakura Mochi theme.
// source: original
// license: original
// order: 50
// animated: no
// samples: yes

vec3 glow_sum(vec2 uv, float lo) {
    vec2 px = 1.0 / SCREEN_SIZE;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.785398;
        vec2 d = vec2(cos(a), sin(a));
        vec3 s1 = texture(tex, uv + d * px * 6.0).rgb;
        vec3 s2 = texture(tex, uv + d * px * 13.0).rgb;
        sum += s1 * smoothstep(lo, 0.95, dot(s1, vec3(0.2126, 0.7152, 0.0722)));
        sum += 0.6 * s2 * smoothstep(lo, 0.95, dot(s2, vec3(0.2126, 0.7152, 0.0722)));
    }
    return sum / 12.8;
}
vec3 effect(vec3 c, vec2 uv) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec3 col = c + vec3(0.03, 0.005, 0.04) * (1.0 - l);
    vec3 g = glow_sum(uv, 0.45);
    return col + vec3(dot(g, vec3(0.3333))) * vec3(1.0, 0.55, 0.75) * 1.3;
}
