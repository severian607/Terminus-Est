// label: Film Grain
// emoji: 🌫️
// category: Retro · Tape & Film
// tags: grain, film, noise, texture, analog
// description: Fine animated luminance-weighted film grain.
// source: original
// license: original
// order: 110
// animated: yes
// samples: no

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec3 effect(vec3 c, vec2 uv) {
    vec2 px = floor(uv * SCREEN_SIZE);
    float f = floor(time * 24.0);
    float n = hash21(px + f * 13.0) + hash21(px.yx + f * 7.0) - 1.0;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    return c + n * 0.07 * (1.0 - abs(l * 2.0 - 1.0) * 0.5);
}
