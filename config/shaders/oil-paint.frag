// label: Oil Paint
// emoji: 🎨
// category: Art · Paint & Ink
// tags: kuwahara, painting, oil, smooth, painterly
// description: Kuwahara filter: flattens detail into painterly blobs while keeping edges sharp.
// source: https://www.shadertoy.com/view/MsXSz4 (Kuwahara filter, rewritten with 4 quadrants, radius 3)
// license: original
// order: 70
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = 1.0 / SCREEN_SIZE;
    const int R = 3;
    vec3 mean[4];
    float var[4];
    for (int k = 0; k < 4; k++) {
        vec2 o = vec2(k == 0 || k == 3 ? -float(R) : 0.0, k < 2 ? -float(R) : 0.0);
        vec3 s = vec3(0.0), s2 = vec3(0.0);
        for (int j = 0; j <= R; j++) {
            for (int i = 0; i <= R; i++) {
                vec3 t = texture(tex, uv + (o + vec2(i, j)) * px).rgb;
                s += t;
                s2 += t * t;
            }
        }
        float n = float((R + 1) * (R + 1));
        mean[k] = s / n;
        vec3 v = s2 / n - mean[k] * mean[k];
        var[k] = v.r + v.g + v.b;
    }
    vec3 best = mean[0];
    float bv = var[0];
    for (int k = 1; k < 4; k++) {
        if (var[k] < bv) { bv = var[k]; best = mean[k]; }
    }
    return best;
}
