// label: Low Poly
// emoji: 🔺
// category: Art · Tiles & Shapes
// tags: low poly, triangles, mesh, facets, geometric
// description: Jittered triangle mesh where each facet is filled with the colour sampled at its centroid.
// source: original
// license: original
// order: 100
// animated: no
// samples: yes

vec2 lp_vert(vec2 id, float cell) {
    vec2 h = fract(sin(vec2(dot(id, vec2(127.1, 311.7)), dot(id, vec2(269.5, 183.3)))) * 43758.5453);
    return (id + 0.5 + (h - 0.5) * 0.7) * cell;
}

float lp_cross(vec2 a, vec2 b, vec2 p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

bool lp_inside(vec2 a, vec2 b, vec2 c, vec2 p) {
    float d1 = lp_cross(a, b, p), d2 = lp_cross(b, c, p), d3 = lp_cross(c, a, p);
    return (d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0) || (d1 <= 0.0 && d2 <= 0.0 && d3 <= 0.0);
}

vec3 effect(vec3 c, vec2 uv) {
    float cell = 30.0;
    vec2 px = uv * SCREEN_SIZE;
    vec2 base = floor(px / cell);
    vec3 col = c;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            vec2 id = base + vec2(float(i), float(j));
            vec2 a = lp_vert(id, cell);
            vec2 b = lp_vert(id + vec2(1, 0), cell);
            vec2 d = lp_vert(id + vec2(0, 1), cell);
            vec2 e = lp_vert(id + vec2(1, 1), cell);
            vec2 t1[3] = vec2[3](a, b, d);
            vec2 t2[3] = vec2[3](b, e, d);
            if (lp_inside(t1[0], t1[1], t1[2], px)) {
                vec2 g = (a + b + d) / 3.0;
                col = texture(tex, clamp(g / SCREEN_SIZE, vec2(0.0), vec2(1.0))).rgb;
                col *= 0.92 + 0.16 * fract(sin(dot(g, vec2(12.9898, 78.233))) * 43758.5453);
            } else if (lp_inside(t2[0], t2[1], t2[2], px)) {
                vec2 g = (b + e + d) / 3.0;
                col = texture(tex, clamp(g / SCREEN_SIZE, vec2(0.0), vec2(1.0))).rgb;
                col *= 0.92 + 0.16 * fract(sin(dot(g, vec2(12.9898, 78.233))) * 43758.5453);
            }
        }
    }
    return col;
}
