// label: Glitch
// emoji: 📡
// category: Glitch
// tags: glitch, datamosh, rgb split, tearing, corruption, static, macroblock
// description: Calm screen with short bursts of slice shifts, blocks, RGB split and static.
// source: original
// license: original
// order: 100
// animated: yes
// samples: yes
// raw: no
// Pattern re-rolls at 20 Hz inside a burst; ~45% of 0.33 s slots glitch.
//   - horizontal slice shifts (variable band height),
//   - displaced / mosaic / inverted macroblocks (datamosh-style),
//   - per-channel RGB split that only hits the corrupted regions,
//   - bright static lines.

float h1(float n) { return fract(sin(n * 12.9898) * 43758.5453); }
float h2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

vec3 effect(vec3 c, vec2 uv) {
    vec2 p = uv;
    float slot = floor(time * 3.0);                       // 0.33 s slots
    float on = step(0.55, h1(slot));                      // ~45% of slots glitch
    float I = on * (0.5 + 0.5 * h1(slot + 9.0));          // burst intensity
    if (I == 0.0) return texture(tex, p).rgb;

    float tick = floor(time * 20.0);
    float hit = 0.0;                                      // 1 where the pixel is corrupted
    float mode = 0.0;                                     // block corruption kind

    // Slice shifts.
    float rows = mix(8.0, 60.0, h1(slot + 3.0));
    float band = floor(p.y * rows);
    float hb = h1(band * 7.1 + tick);
    if (hb > 0.72) {
        p.x += (h1(band + tick * 3.3) - 0.5) * 0.5 * I;
        hit = 1.0;
    }

    // Macroblocks: some jump to another place, some turn into mosaic, a few invert.
    vec2 bs = vec2(mix(0.04, 0.22, h1(tick + 1.0)), mix(0.02, 0.12, h1(tick + 2.0)));
    vec2 cell = floor(p / bs);
    float hc = h2(cell + tick);
    if (hc > 1.0 - 0.12 * I) {
        p += (vec2(h2(cell + 1.7), h2(cell + 4.1)) - 0.5) * vec2(0.6, 0.25);
        hit = 1.0;
        mode = hc > 1.0 - 0.025 * I ? 2.0 : (hc > 1.0 - 0.06 * I ? 1.0 : 0.0);
    }
    if (mode == 1.0) p = (floor(p * 48.0) + 0.5) / 48.0;  // mosaic

    p = clamp(p, 0.0, 1.0);
    float split = hit * 0.02 * I + I * 0.002;
    c = vec3(texture(tex, p + vec2(split, 0.0)).r,
             texture(tex, p).g,
             texture(tex, p - vec2(split, split * 0.5)).b);
    if (mode == 2.0) c = 1.0 - c.gbr;                     // inverted, channel-rotated block

    // Static lines.
    float line = step(0.992, h1(floor(uv.y * 540.0) + tick * 1.7));
    return mix(c, vec3(1.0), line * 0.7 * I);
}
