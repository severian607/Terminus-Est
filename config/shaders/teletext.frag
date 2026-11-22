// label: Teletext
// emoji: 📺
// category: Retro · Pixel & Palette
// tags: teletext, ceefax, 8 colour, mosaic, blocks, 80s
// description: Eight pure teletext colours in chunky 2x3 sixel blocks on a 12x18px grid.
// source: original
// license: original
// order: 30
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 sub = vec2(6.0);
    vec2 p = (floor(uv * SCREEN_SIZE / sub) + 0.5) * sub / SCREEN_SIZE;
    vec3 s = texture(tex, clamp(p, vec2(0.0), vec2(1.0))).rgb;
    return step(vec3(0.38), s);
}
