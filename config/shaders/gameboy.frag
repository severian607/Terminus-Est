// label: Game Boy
// emoji: 🎮
// category: Retro · Pixel & Palette
// tags: gameboy, 4 shades, palette, dither, handheld, retro
// description: Four-shade olive-green handheld palette with a 2x2 ordered dither.
// source: original
// license: original
// order: 110
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = floor(uv * SCREEN_SIZE);
    float bx = mod(px.x, 2.0);
    float by = mod(px.y, 2.0);
    float d = bx * 2.0 + by * 3.0 - bx * by * 4.0;
    float l = dot(c, vec3(0.299, 0.587, 0.114)) + (d / 4.0 - 0.375) * 0.2;
    float idx = clamp(floor(l * 4.0), 0.0, 3.0);
    vec3 col = vec3(0.059, 0.220, 0.059);
    col = mix(col, vec3(0.188, 0.384, 0.188), step(1.0, idx));
    col = mix(col, vec3(0.545, 0.675, 0.059), step(2.0, idx));
    col = mix(col, vec3(0.608, 0.737, 0.059), step(3.0, idx));
    return col;
}
