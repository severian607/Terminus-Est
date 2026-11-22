// label: LCD Subpixels
// emoji: 🔬
// category: Retro · CRT
// tags: lcd, subpixel, rgb, stripe, macro, pixel grid
// description: Close-up LCD panel: each 3px pixel is split into red, green and blue stripes with dark gaps.
// source: original
// license: original
// order: 100
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = uv * SCREEN_SIZE;
    vec2 cell = vec2(3.0, 3.0);
    vec2 id = floor(px / cell);
    vec2 p = (id + 0.5) * cell / SCREEN_SIZE;
    vec3 s = texture(tex, p).rgb;
    vec2 f = mod(px, cell);
    int sub = int(f.x);
    vec3 mask = vec3(sub == 0 ? 1.0 : 0.08, sub == 1 ? 1.0 : 0.08, sub == 2 ? 1.0 : 0.08);
    float row = f.y < 2.0 ? 1.0 : 0.15;
    return s * mask * row * 1.9;
}
