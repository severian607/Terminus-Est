// label: Pixel Sort
// emoji: 🌫️
// category: Glitch
// tags: pixel sort, streak, glitch art, smear, bright
// description: Bright pixels smear rightward into long gradient streaks, with a threshold that drifts per row.
// source: original (single-pass approximation of the pixel-sorting glitch-art technique)
// license: original
// order: 120
// animated: yes
// samples: yes

float psHash(float n) { return fract(sin(n * 91.345) * 47453.5453); }
float psLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 effect(vec3 c, vec2 uv) {
    vec2 res = SCREEN_SIZE;
    float row = floor(uv.y * res.y / 6.0);
    float thr = 0.3 + 0.2 * sin(time * 0.6 + psHash(row) * 6.2831) + 0.1 * psHash(row + 3.0);
    float reach = 0.35 + 0.65 * psHash(row + floor(time * 0.5));
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int i = 1; i <= 28; i++) {
        float fi = float(i);
        float dx = fi * fi * 0.00045 * reach;
        vec3 s = texture(tex, vec2(clamp(uv.x - dx, 0.0, 1.0), uv.y)).rgb;
        float l = psLum(s);
        float w = exp(-fi * 0.11) * smoothstep(thr, thr + 0.1, l);
        acc += s * w;
        wsum += w;
    }
    vec3 streak = acc / max(wsum, 1e-4);
    float amt = clamp(wsum * 0.9, 0.0, 1.0);
    vec3 col = mix(c, streak, amt * (1.0 - smoothstep(thr, thr + 0.15, psLum(c)) * 0.6));
    return max(col, c * 0.9);
}
