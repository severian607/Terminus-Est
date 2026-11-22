// label: Comic Book
// emoji: 💥
// category: Art · Paint & Ink
// tags: comic, halftone, ink, outline, posterize, pop art
// description: Four-level posterised colour with bold black outlines and halftone dots in the shadows.
// source: original
// license: original
// order: 10
// animated: no
// samples: yes

float comic_lum(vec2 p) {
    return dot(texture(tex, clamp(p, vec2(0.0), vec2(1.0))).rgb, vec3(0.299, 0.587, 0.114));
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 t = 1.5 / SCREEN_SIZE;
    float a = comic_lum(uv + vec2(-t.x, -t.y)), b = comic_lum(uv + vec2(0.0, -t.y)), cc = comic_lum(uv + vec2(t.x, -t.y));
    float d = comic_lum(uv + vec2(-t.x, 0.0)), f = comic_lum(uv + vec2(t.x, 0.0));
    float g = comic_lum(uv + vec2(-t.x, t.y)), h = comic_lum(uv + vec2(0.0, t.y)), i = comic_lum(uv + vec2(t.x, t.y));
    float gx = (cc + 2.0 * f + i) - (a + 2.0 * d + g);
    float gy = (g + 2.0 * h + i) - (a + 2.0 * b + cc);
    float edge = smoothstep(0.18, 0.45, length(vec2(gx, gy)));

    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    // punchy colour: boost saturation, then posterise
    vec3 sat = clamp(mix(vec3(lum), c, 1.5), 0.0, 1.0);
    vec3 post = floor(sat * 3.999) / 3.0;

    // halftone dots (45 degree screen) in darker midtones
    vec2 px = uv * SCREEN_SIZE;
    mat2 rot = mat2(0.7071, -0.7071, 0.7071, 0.7071);
    vec2 q = rot * px / 6.0;
    float dist = length(fract(q) - 0.5);
    float shade = 1.0 - smoothstep(0.0, 0.5, lum * 2.0);
    float radius = sqrt(clamp(shade, 0.0, 1.0)) * 0.55;
    float dots = (1.0 - smoothstep(radius - 0.06, radius + 0.06, dist)) * step(0.02, shade);

    // shadow dots: ink dots over the posterised colour, only where there is some colour to cover
    float band = smoothstep(0.02, 0.12, lum);
    vec3 col = mix(post, vec3(0.02), dots * 0.85 * band);
    col = mix(col, vec3(0.0), edge);
    return col;
}
