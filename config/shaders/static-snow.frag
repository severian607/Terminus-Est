// label: Static Snow
// emoji: ❄️
// category: Retro · Tape & Film
// tags: static, snow, noise, tv, no signal, interference
// description: Light TV snow noise with sparse white specks (24 fps).
// source: original
// license: original
// order: 90
// animated: yes
// samples: no

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec3 effect(vec3 c, vec2 uv) {
    vec2 px = floor(uv * SCREEN_SIZE);
    float n = hash21(px + floor(time * 24.0) * 17.0);
    return c + (n - 0.5) * 0.06 + step(0.9985, n) * 0.5;
}
