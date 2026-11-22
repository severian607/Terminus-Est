// label: Atari 2600 TV
// emoji: 📺
// category: Retro · Pixel & Palette
// tags: atari, 2600, vcs, console, old tv, crt, retro, 160x192, composite, scanlines, rf
// description: An Atari 2600 on a 70s TV: 200x240 chunky pixels, a limited palette, composite colour bleed, scanlines, curved glass, roll bar and flicker.
// source: original
// license: original
// order: 150
// animated: yes
// samples: yes

vec3 hue_to_rgb(float h) {
    return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

// Snap a colour to an Atari-like palette: 16 hues, a few saturation steps, 8 luminance levels.
vec3 atari_palette(vec3 c) {
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float chroma = mx - mn;
    float hue = 0.0;
    if (chroma > 0.0001) {
        if (mx == c.r) hue = mod((c.g - c.b) / chroma, 6.0);
        else if (mx == c.g) hue = (c.b - c.r) / chroma + 2.0;
        else hue = (c.r - c.g) / chroma + 4.0;
        hue /= 6.0;
    }
    float sat = chroma < 0.08 ? 0.0 : mix(0.45, 0.8, step(0.35, chroma));
    hue = floor(hue * 16.0 + 0.5) / 16.0;
    float l = floor(luma * 9.0 + 0.5) / 9.0;
    vec3 tint = hue_to_rgb(hue);
    return clamp(mix(vec3(l), tint * (0.35 + 0.9 * l), sat), 0.0, 1.0);
}

vec3 effect(vec3 c, vec2 uv) {
    // Barrel-curved glass.
    vec2 d = uv - 0.5;
    vec2 p = 0.5 + d * (1.0 + 0.10 * dot(d, d) * 4.0);
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return vec3(0.0);

    // Atari 2600 pixel grid: 160 colour clocks by 192 lines.
    vec2 grid = vec2(200.0, 240.0);
    vec2 cell = (floor(p * grid) + 0.5) / grid;

    // Composite smear: colour bleeds one clock to the right, luminance stays crisp.
    vec3 here = texture(tex, cell).rgb;
    vec3 left = texture(tex, cell - vec2(1.0 / grid.x, 0.0)).rgb;
    float luma = dot(here, vec3(0.299, 0.587, 0.114));
    vec3 col = mix(here, left, 0.28);
    col = col - vec3(dot(col, vec3(0.299, 0.587, 0.114))) + vec3(luma);
    col = atari_palette(col);

    // Scanlines between the 192 lines, and a faint shadow-mask beat across the grid.
    float row = fract(p.y * grid.y);
    float scan = 0.70 + 0.30 * smoothstep(0.0, 0.3, row) * smoothstep(1.0, 0.7, row);
    float beat = 0.95 + 0.05 * sin(p.x * grid.x * 6.2831853);
    col *= scan * beat;

    // Rolling hum bar and mains flicker.
    float roll = 0.04 * sin(p.y * 7.0 - time * 0.9);
    float flicker = 1.0 - 0.02 * sin(time * 60.0) - 0.012 * sin(time * 17.3);
    col *= (1.0 + roll) * flicker;

    // Warm phosphor, vignette and a touch of glow.
    col *= vec3(1.0, 0.97, 0.9);
    col *= 1.0 - 0.44 * dot(d, d) * 2.4;
    col += 0.03 * smoothstep(0.55, 1.0, luma);
    return clamp(col, 0.0, 1.0);
}
