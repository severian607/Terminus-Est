// label: Sakura
// emoji: 🌸
// category: Color · Mono & Tint
// tags: sakura, pink, plum, mochi, grade, bloom
// description: Plum-black shadows, pink highlights, green mids and a soft pink bloom.
// source: original
// license: original
// order: 120
// animated: no
// samples: no
// raw: no
// The bloom reads one neighbouring pixel, so it is treated as colour-only (damage tracking stays on).

vec3 effect(vec3 c, vec2 uv) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec3 shadow = vec3(0.075, 0.030, 0.090);
    vec3 mid    = vec3(0.50, 0.78, 0.58);
    vec3 light  = vec3(1.00, 0.70, 0.82);
    vec3 grade = l < 0.5 ? mix(shadow, mid, l * 2.0) : mix(mid, light, (l - 0.5) * 2.0);
    // Keep 35% of the original hue so the content stays recognisable.
    vec3 o = mix(grade, grade * (c / max(l, 0.04)), 0.35);
    // Glow: bright areas bleed pink.
    float g = dot(texture(tex, uv + vec2(0.0015, 0.0015)).rgb, vec3(0.333));
    return o + light * 0.12 * smoothstep(0.75, 1.0, g);
}
