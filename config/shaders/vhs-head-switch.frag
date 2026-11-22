// label: VHS Head Switch
// emoji: 🧲
// category: Retro · Tape & Film
// tags: vhs, head switching, noise, tear, bottom edge
// description: Noisy head-switching tear band hugging one screen edge, VHS style.
// source: https://github.com/lostjared/shaders (vhs-code-head-switch)
// license: MIT
// order: 60
// animated: yes
// samples: yes

float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(41.23, 289.17))) * 45758.5453);
}

vec3 effect(vec3 c, vec2 uv) {
    float f = floor(time * 29.97);
    float ln = floor(uv.y * SCREEN_SIZE.y);
    float top = 0.075 + 0.012 * sin(time * 2.0);
    float zone = 1.0 - smoothstep(top, top + 0.045, uv.y);
    float saw = fract(uv.y * 95.0 + time * 5.0) - 0.5;
    vec2 p = uv;
    p.x += zone * (saw * 0.035 + (hash21(vec2(ln, f)) - 0.5) * 0.09);
    p.x += sin(uv.y * 160.0 + time * 4.0) * 0.0015;
    p = clamp(p, 0.001, 0.999);
    vec3 col;
    col.r = texture(tex, p + vec2(0.002 + zone * 0.006, 0.0)).r;
    col.g = texture(tex, p).g;
    col.b = texture(tex, p - vec2(0.003 + zone * 0.008, 0.0)).b;
    float n = hash21(floor(uv * SCREEN_SIZE) + f * 31.0) - 0.5;
    float tear = step(0.93, hash21(vec2(ln, f + 7.0))) * zone;
    col += n * (0.025 + zone * 0.2);
    col = mix(col, vec3(0.7 + n * 0.5), tear * 0.4);
    return col * (0.95 + 0.05 * sin(ln * 3.14159265));
}
