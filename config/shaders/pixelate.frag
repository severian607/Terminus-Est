// label: Pixelate
// emoji: 🟦
// category: Retro · Pixel & Palette
// tags: pixelate, mosaic, blocks, low res, 8-bit
// description: Chunky 5px mosaic blocks.
// source: https://github.com/lostjared/shaders (pixels, pixel-based size)
// license: MIT
// order: 10
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 cell = vec2(5.0);
    vec2 p = (floor(uv * SCREEN_SIZE / cell) + 0.5) * cell / SCREEN_SIZE;
    return texture(tex, p).rgb;
}
