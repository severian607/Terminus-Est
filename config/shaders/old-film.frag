// label: Old Film
// emoji: 🎞️
// category: Retro · Tape & Film
// tags: film, grain, scratches, flicker, sepia, vintage
// description: Flickering sepia film with grain and vertical scratches.
// source: https://github.com/lostjared/shaders (old-film, vhs-code-worn-tape)
// license: MIT
// order: 120
// animated: yes
// samples: no

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec3 effect(vec3 c, vec2 uv) {
    float f = floor(time * 18.0);
    vec2 px = floor(uv * SCREEN_SIZE);
    float grain = hash21(px + f * 29.0) - 0.5;
    float scratch = step(0.993, hash21(vec2(floor(px.x * 0.25), f)));
    float flicker = 0.92 + 0.08 * hash21(vec2(f, 2.0));
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 col = mix(vec3(l), c, 0.4) * vec3(1.06, 0.97, 0.82);
    col = col * flicker + grain * 0.09 + scratch * (0.12 + grain * 0.2);
    vec2 d = uv - 0.5;
    return col * (1.0 - 0.5 * dot(d, d));
}
