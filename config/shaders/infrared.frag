// label: Infrared
// emoji: 🌿
// category: Color · Vision
// tags: infrared, false colour, foliage, channel swap, photography
// description: False-colour infrared photo look: greens turn pale pink-white and blues go dark.
// source: original
// license: original
// order: 10
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float ir = clamp(c.g * 1.1 + c.r * 0.2 - c.b * 0.3, 0.0, 1.0);
    vec3 col = vec3(ir * 1.1 + 0.1 * c.r, c.r * 0.55 + ir * 0.3, c.b * 0.5 + ir * 0.45);
    col = vec3(col.r, col.b * 0.9 + 0.05 * col.g, col.g);
    col.g = col.g * 0.8 + c.r * 0.15;
    col = mix(col, vec3(ir), smoothstep(0.6, 1.0, ir) * 0.5);
    return clamp(col, 0.0, 1.0);
}
