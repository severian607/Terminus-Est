// label: Bayer Dither
// emoji: 🔳
// category: Retro · Pixel & Palette
// tags: dither, bayer, ordered, 1-bit, low colour, mac, ega
// description: Ordered 8x8 Bayer dithering down to 4 levels per channel, like an early 16-colour display.
// source: https://en.wikipedia.org/wiki/Ordered_dithering (Bayer matrix, generated bit-interleave form)
// license: original
// order: 55
// animated: no
// samples: no

float bayer8(vec2 p) {
    ivec2 q = ivec2(mod(p, 8.0));
    int x = q.x, y = q.y ^ q.x;
    int v = 0;
    v |= ((y >> 2) & 1) << 0 | ((x >> 2) & 1) << 1;
    v |= ((y >> 1) & 1) << 2 | ((x >> 1) & 1) << 3;
    v |= ((y >> 0) & 1) << 4 | ((x >> 0) & 1) << 5;
    return (float(v) + 0.5) / 64.0;
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = floor(uv * SCREEN_SIZE / 2.0);   // 2px dither cells
    float t = bayer8(px) - 0.5;
    float levels = 3.0;                         // 4 levels per channel
    return floor(c * levels + 0.5 + t) / levels;
}
