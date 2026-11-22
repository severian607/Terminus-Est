// label: LSD
// emoji: 🌈
// category: Psychedelic
// tags: lsd, acid, trippy, psychedelic, hue, wobble, neon, weird colours
// description: Wobble, glitch slices, RGB split, cycling hue, neon glare, pulsing vignette.
// source: original
// license: original
// order: 100
// animated: yes
// samples: yes
// raw: no

vec3 effect(vec3 c, vec2 uv) {
    vec2 p = uv;
    float t = time;

    // Glitch slices: ~40 horizontal bands, a few shove sideways in short bursts.
    float band = floor(p.y * 40.0);
    float h = fract(sin(band * 12.9898 + floor(t * 6.0) * 78.233) * 43758.5453);
    p.x += step(0.93, h) * (h - 0.93) * 1.4;

    // Wobble (one sin per axis); the vertical wave is also wrapped into the horizontal one.
    float wy = sin(p.y * 34.0 + t * 2.6);
    p.x += 0.004 * wy;
    p.y += 0.003 * sin(p.x * 28.0 - t * 2.1);

    // Pulsing chromatic split; direction swings with time.
    vec2 d = vec2(cos(t * 0.6), sin(t * 0.6)) * (0.004 + 0.003 * sin(t * 1.7));
    c = vec3(texture(tex, p + d).r, texture(tex, p).g, texture(tex, p - d).b);

    // Partial inversion + channel swap.
    c = mix(c, 1.0 - c.gbr, 0.7);
    float l = dot(c, vec3(0.299, 0.587, 0.114));

    // Hue rotation in YIQ: one sin/cos pair, angle driven by time, brightness and position.
    float a = t * 0.9 + l * 4.0 + (p.x + p.y) * 3.0;
    float s = sin(a), k = cos(a);
    vec3 yiq = vec3(l, dot(c, vec3(0.596, -0.274, -0.322)), dot(c, vec3(0.211, -0.523, 0.312)));
    yiq.yz = vec2(yiq.y * k - yiq.z * s, yiq.y * s + yiq.z * k) * 3.2;   // also oversaturates
    c = vec3(yiq.x + 0.956 * yiq.y + 0.621 * yiq.z,
             yiq.x - 0.272 * yiq.y - 0.647 * yiq.z,
             yiq.x - 1.106 * yiq.y + 1.703 * yiq.z);

    // Neon: hard contrast and a strong brightness lift, plus glare on bright areas.
    c = (c - 0.5) * 1.8 + 0.62 + 0.2 * l * l;

    // Scanlines (triangle wave, no trig) and a pulsing rainbow vignette.
    c *= 0.88 + 0.12 * abs(fract(uv.y * 810.0) * 2.0 - 1.0);
    vec2 q = uv - 0.5;
    float v = dot(q, q) * (1.4 + 0.5 * wy);
    return c + v * vec3(1.0, 0.1, 0.9) * (0.5 + 0.5 * k);
}
