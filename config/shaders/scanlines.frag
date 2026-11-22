// label: Scanlines
// emoji: 📺
// category: Retro · CRT
// tags: scanlines, crt, lines, retro
// description: Faint horizontal scanlines on the real pixel grid; text stays readable.
// source: original
// license: original
// order: 80
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float y = uv.y * SCREEN_SIZE.y;
    float s = 0.5 + 0.5 * cos(y * 1.5707963);
    return c * (0.82 + 0.18 * s) * 1.06;
}
