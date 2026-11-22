// label: VHS Dropout
// emoji: 📉
// category: Retro · Tape & Film
// tags: vhs, dropout, tape damage, white streaks, glitch
// description: Damaged tape lines shift sideways and drop to grey static.
// source: https://github.com/lostjared/shaders (vhs-code-dropout)
// license: MIT
// order: 50
// animated: yes
// samples: yes

float hash21(vec2 p) {
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}

vec3 effect(vec3 c, vec2 uv) {
    float f = floor(time * 24.0);
    float ln = floor(uv.y * SCREEN_SIZE.y);
    float blk = floor(uv.x * 28.0);
    float seg = hash21(vec2(blk + f * 0.17, ln));
    float bad = step(0.965, hash21(vec2(ln, floor(f * 0.5))));
    float drop = bad * step(0.42, seg);
    vec2 p = uv;
    p.x += (hash21(vec2(ln, f)) - 0.5) * bad * 0.075 + sin(uv.y * 190.0 + time * 2.7) * 0.0012;
    p = clamp(p, 0.001, 0.999);
    vec3 img = texture(tex, p).rgb;
    vec3 sep;
    sep.r = texture(tex, p + vec2(0.0025, 0.0)).r;
    sep.g = img.g;
    sep.b = texture(tex, p - vec2(0.003, 0.0)).b;
    img = mix(img, sep, 0.65);
    float grain = hash21(floor(uv * SCREEN_SIZE) + f * 13.0) - 0.5;
    img = mix(img, vec3(0.62 + grain * 0.7), drop * 0.78);
    img += grain * 0.035;
    return img * (0.96 + 0.04 * sin(ln * 3.14159265));
}
