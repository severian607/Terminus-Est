// label: Stained Glass
// emoji: 🪟
// category: Art · Tiles & Shapes
// tags: voronoi, stained glass, cells, mosaic, lead
// description: Irregular Voronoi glass panes with dark lead lines; each pane takes the colour at its centre.
// source: https://iquilezles.org/articles/voronoilines/ (Inigo Quilez, Voronoi borders, reduced)
// license: original
// order: 80
// animated: no
// samples: yes

vec2 hash2(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453);
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 res = SCREEN_SIZE;
    float size = 48.0;
    vec2 p = uv * res / size;
    vec2 ip = floor(p), fp = fract(p);
    float d1 = 8.0, d2 = 8.0;
    vec2 site = vec2(0.0);
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            vec2 g = vec2(i, j);
            vec2 o = hash2(ip + g);
            vec2 r = g + o - fp;
            float d = dot(r, r);
            if (d < d1) { d2 = d1; d1 = d; site = ip + g + o; }
            else if (d < d2) { d2 = d; }
        }
    }
    // distance to border via second-nearest (approx), cheap and good enough
    float border = sqrt(d2) - sqrt(d1);
    vec3 col = texture(tex, site * size / res).rgb;
    col *= 1.15 - 0.35 * sqrt(d1);              // lit centre, darker rim
    float lead = smoothstep(0.02, 0.09, border);
    return mix(vec3(0.03), col, lead);
}
