// label: Sin City
// emoji: 🩸
// category: Color · Mono & Tint
// tags: noir, selective colour, red, black and white, comic
// description: High-contrast black and white that keeps only strongly red pixels in colour.
// source: original
// license: original
// order: 10
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    float g = clamp((l - 0.5) * 1.5 + 0.5, 0.0, 1.0);
    float redness = c.r - max(c.g, c.b);
    float keep = smoothstep(0.12, 0.35, redness);
    vec3 col = vec3(c.r * 1.2, c.g * 0.5, c.b * 0.5);
    return mix(vec3(g), clamp(col, 0.0, 1.0), keep);
}
