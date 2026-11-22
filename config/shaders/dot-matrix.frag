// label: Dot Matrix
// emoji: 🔴
// category: Retro · CRT
// tags: led, dot matrix, sign, board, round dots, glow
// description: LED sign board of round 6px dots with dark gaps and a soft glow on bright dots.
// source: original
// license: original
// order: 110
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    float cell = 6.0;
    vec2 px = uv * SCREEN_SIZE;
    vec2 id = floor(px / cell);
    vec2 f = (px / cell - id) - 0.5;
    vec3 s = texture(tex, (id + 0.5) * cell / SCREEN_SIZE).rgb;
    float lum = dot(s, vec3(0.299, 0.587, 0.114));
    float d = length(f);
    float r = 0.30 + 0.12 * lum;
    float dotm = 1.0 - smoothstep(r - 0.08, r + 0.04, d);
    float glow = exp(-d * d * 9.0) * lum * 0.45;
    vec3 col = s * (dotm * 1.25) + s * glow;
    return col + vec3(0.012);
}
