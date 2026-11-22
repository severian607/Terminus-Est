// label: Halftone
// emoji: ⚪
// category: Art · Tiles & Shapes
// tags: halftone, dots, print, comic, newspaper
// description: 45-degree dot screen; dot size follows brightness.
// source: original
// license: original
// order: 20
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    const float cell = 7.0;
    const mat2 rot = mat2(0.70711, -0.70711, 0.70711, 0.70711);
    vec2 q = rot * (uv * SCREEN_SIZE);
    vec2 g = (floor(q / cell) + 0.5) * cell;
    vec2 cp = g * rot;
    vec3 col = texture(tex, clamp(cp / SCREEN_SIZE, 0.0, 1.0)).rgb;
    float v = max(col.r, max(col.g, col.b));
    float r = cell * 0.7071 * sqrt(v);
    float m = 1.0 - smoothstep(r - 0.7, r + 0.7, length(q - g));
    return col * m;
}
