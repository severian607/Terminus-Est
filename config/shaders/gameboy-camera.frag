// label: Game Boy Camera
// emoji: 📷
// category: Retro · Pixel & Palette
// tags: gameboy camera, dither, 4 shades, green, bayer, chunky
// description: Chunky 3px cells in four green shades with a coarse 2x2 Bayer dither.
// source: original
// license: original
// order: 20
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 cell = floor(uv * SCREEN_SIZE / 3.0);
    vec2 p = (cell * 3.0 + 1.5) / SCREEN_SIZE;
    vec3 s = texture(tex, clamp(p, vec2(0.0), vec2(1.0))).rgb;
    float l = dot(s, vec3(0.299, 0.587, 0.114));
    l = clamp((l - 0.5) * 1.3 + 0.5, 0.0, 1.0);
    vec2 m = mod(cell, 2.0);
    float bayer = (m.x * 2.0 + m.y * 3.0 - m.x * m.y * 4.0) / 4.0 - 0.375;
    float idx = clamp(floor((l + bayer * 0.33) * 4.0), 0.0, 3.0);
    vec3 col = vec3(0.04, 0.12, 0.06);
    col = mix(col, vec3(0.25, 0.42, 0.20), step(1.0, idx));
    col = mix(col, vec3(0.55, 0.74, 0.32), step(2.0, idx));
    col = mix(col, vec3(0.82, 0.93, 0.55), step(3.0, idx));
    return col;
}
