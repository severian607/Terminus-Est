// label: Watercolor
// emoji: 🎨
// category: Art · Paint & Ink
// tags: watercolor, wash, paper, bleed, soft, painting
// description: Soft bleeding washes with darker pigment edges and paper grain in a washed-out light palette.
// source: original
// license: original
// order: 20
// animated: no
// samples: yes

float wc_hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float wc_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(wc_hash(i), wc_hash(i + vec2(1, 0)), f.x),
               mix(wc_hash(i + vec2(0, 1)), wc_hash(i + vec2(1, 1)), f.x), f.y);
}

vec3 wc_blur(vec2 uv, float r) {
    vec3 sum = texture(tex, uv).rgb;
    for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.7853982 + 0.3;
        vec2 o = vec2(cos(a), sin(a)) * r / SCREEN_SIZE;
        sum += texture(tex, clamp(uv + o, vec2(0.0), vec2(1.0))).rgb;
        sum += texture(tex, clamp(uv + o * 0.5, vec2(0.0), vec2(1.0))).rgb;
    }
    return sum / 17.0;
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = uv * SCREEN_SIZE;
    vec2 wob = vec2(wc_noise(px / 40.0), wc_noise(px / 40.0 + 17.0)) - 0.5;
    vec2 u2 = uv + wob * 10.0 / SCREEN_SIZE;
    vec3 soft = wc_blur(u2, 5.0);
    vec3 wide = wc_blur(u2, 11.0);
    // pigment pools at edges: where the tight blur differs from the wide one
    float edge = length(soft - wide);
    float pool = smoothstep(0.02, 0.2, edge);
    float lum = dot(soft, vec3(0.299, 0.587, 0.114));
    vec3 paper = vec3(0.97, 0.95, 0.90);
    float grain = wc_noise(px / 2.0) * 0.5 + wc_noise(px / 9.0) * 0.5;
    float wash = 0.55 + 0.45 * wc_noise(px / 70.0);
    vec3 pig = mix(vec3(lum), soft, 1.25);
    pig = clamp(pig, 0.0, 1.0);
    // dilute colour toward paper, keep some body so dark stays dark-ish
    vec3 col = mix(paper, paper * (0.35 + 0.9 * pig) , 0.62 * wash + 0.2);
    col = mix(col, col * (pig * 0.6 + 0.25), pool * 0.75);
    col *= 0.93 + 0.12 * grain;
    return clamp(col, 0.0, 1.0);
}
