// label: Edge Outline
// emoji: ✏️
// category: Art · Line
// tags: edges, outline, sobel, blueprint, neon lines
// description: Dims the picture and traces edges in sakura pink.
// source: original
// license: original
// order: 30
// animated: no
// samples: yes

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}
vec3 effect(vec3 c, vec2 uv) {
    vec2 px = 1.0 / SCREEN_SIZE;
    float gx = luma(texture(tex, uv + vec2(px.x, 0.0)).rgb) - luma(texture(tex, uv - vec2(px.x, 0.0)).rgb);
    float gy = luma(texture(tex, uv + vec2(0.0, px.y)).rgb) - luma(texture(tex, uv - vec2(0.0, px.y)).rgb);
    float e = smoothstep(0.08, 0.30, sqrt(gx * gx + gy * gy) * 2.0);
    return c * 0.55 + e * vec3(1.0, 0.70, 0.82);
}
