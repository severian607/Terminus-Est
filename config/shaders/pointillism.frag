// label: Pointillism
// emoji: 🖌️
// category: Art · Paint & Ink
// tags: pointillism, dots, seurat, impressionist, stipple, paint
// description: Scattered paint dots on a cream ground from a jittered grid; dark areas get larger dots so dark content stays readable.
// source: original
// license: original
// order: 30
// animated: no
// samples: yes

vec2 pt_hash2(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.xx + q.yz) * q.zy);
}

vec3 effect(vec3 c, vec2 uv) {
    float cell = 8.0;
    vec2 px = uv * SCREEN_SIZE;
    vec2 base = floor(px / cell);
    vec3 ground = vec3(0.95, 0.91, 0.80);
    vec3 col = ground;
    float bestScore = 1e9;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            vec2 id = base + vec2(float(i), float(j));
            vec2 h = pt_hash2(id);
            vec2 ctr = (id + 0.2 + 0.6 * h) * cell;
            vec3 s = texture(tex, clamp(ctr / SCREEN_SIZE, vec2(0.0), vec2(1.0))).rgb;
            float lum = dot(s, vec3(0.299, 0.587, 0.114));
            float r = cell * (0.80 - 0.40 * clamp(lum * 1.5, 0.0, 1.0) + 0.15 * h.x);
            float d = length(px - ctr);
            if (d < r) {
                float score = d / r;
                if (score < bestScore) {
                    bestScore = score;
                    vec3 sat = clamp(mix(vec3(lum), s, 1.3), 0.0, 1.0);
                    float shade = 1.0 - 0.2 * smoothstep(0.5, 1.0, score);
                    col = mix(ground, sat * shade, smoothstep(1.0, 0.85, score));
                }
            }
        }
    }
    return col;
}
