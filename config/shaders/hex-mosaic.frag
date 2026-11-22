// label: Hex Mosaic
// emoji: ⬡
// category: Art · Tiles & Shapes
// tags: hexagon, hex, mosaic, tiles, honeycomb
// description: Hexagonal pixelation with a thin gap between honeycomb cells.
// source: https://www.shadertoy.com/view/Xljczw (hexagonal tiling after Shane / iq, rewritten)
// license: original
// order: 85
// animated: no
// samples: yes

vec4 hexCoords(vec2 p) {
    const vec2 s = vec2(1.0, 1.7320508);
    vec2 a = mod(p, s) - s * 0.5;
    vec2 b = mod(p - s * 0.5, s) - s * 0.5;
    vec2 gv = dot(a, a) < dot(b, b) ? a : b;
    vec2 id = p - gv;
    return vec4(gv, id);
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 res = SCREEN_SIZE;
    float size = 22.0;
    vec2 p = uv * res / size;
    vec4 h = hexCoords(p);
    vec2 centre = h.zw * size / res;
    vec3 col = texture(tex, centre).rgb;
    // hex distance for the gap
    vec2 q = abs(h.xy);
    float hd = max(dot(q, normalize(vec2(1.0, 1.7320508))), q.x);
    float gap = smoothstep(0.46, 0.5, hd);
    return mix(col, col * 0.25, gap);
}
