// label: Channel Tear
// emoji: 🪟
// category: Glitch
// tags: glitch, channel swap, bands, colour tear, stripes
// description: Thin horizontal bands briefly swap their colour channels.
// source: original
// license: original
// order: 100
// animated: yes
// samples: no

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec3 effect(vec3 c, vec2 uv) {
    float t = floor(time * 8.0);
    float b = floor(uv.y * SCREEN_SIZE.y / 18.0);
    float r = hash21(vec2(b, t));
    if (r > 0.965) return c.bgr;
    if (r > 0.935) return c.gbr;
    return c;
}
