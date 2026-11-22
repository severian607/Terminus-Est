// label: Oscilloscope
// emoji: 📟
// category: Retro · CRT
// tags: oscilloscope, vector, phosphor, green, edges, graticule
// description: Edges of the screen drawn as glowing green phosphor lines on black with a faint graticule.
// source: original
// license: original
// order: 120
// animated: no
// samples: yes

float osc_lum(vec2 p) {
    return dot(texture(tex, clamp(p, vec2(0.0), vec2(1.0))).rgb, vec3(0.299, 0.587, 0.114));
}

float osc_edge(vec2 uv, float r) {
    vec2 t = r / SCREEN_SIZE;
    float a = osc_lum(uv + vec2(-t.x, -t.y)), b = osc_lum(uv + vec2(0.0, -t.y)), c = osc_lum(uv + vec2(t.x, -t.y));
    float d = osc_lum(uv + vec2(-t.x, 0.0)), f = osc_lum(uv + vec2(t.x, 0.0));
    float g = osc_lum(uv + vec2(-t.x, t.y)), h = osc_lum(uv + vec2(0.0, t.y)), i = osc_lum(uv + vec2(t.x, t.y));
    float gx = (c + 2.0 * f + i) - (a + 2.0 * d + g);
    float gy = (g + 2.0 * h + i) - (a + 2.0 * b + c);
    return length(vec2(gx, gy));
}

vec3 effect(vec3 c, vec2 uv) {
    float core = smoothstep(0.08, 0.5, osc_edge(uv, 1.0));
    float halo = smoothstep(0.05, 0.8, osc_edge(uv, 3.0)) * 0.5;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    float trace = clamp(core + halo * 0.6 + lum * 0.12, 0.0, 1.5);

    vec2 px = uv * SCREEN_SIZE;
    vec2 g = uv * vec2(10.0, 8.0);
    vec2 gl = abs(fract(g) - 0.5);
    vec2 gw = vec2(10.0, 8.0) / SCREEN_SIZE * 0.5;
    float line = max(step(0.5 - gw.x, gl.x), step(0.5 - gw.y, gl.y));
    // minor ticks on the centre axes
    vec2 m = abs(uv - 0.5);
    float ticks = (m.y < 2.5 / SCREEN_SIZE.y && fract(uv.x * 50.0) < 0.06 ? 1.0 : 0.0)
                + (m.x < 2.5 / SCREEN_SIZE.x && fract(uv.y * 40.0) < 0.06 ? 1.0 : 0.0);
    float grat = clamp(line * 0.10 + ticks * 0.25, 0.0, 0.4);

    vec3 phosphor = vec3(0.25, 1.0, 0.35);
    vec3 col = vec3(0.0, 0.015, 0.004) + vec3(0.1, 0.45, 0.15) * grat;
    col += phosphor * trace * 1.1;
    col += vec3(0.7, 1.0, 0.75) * smoothstep(0.9, 1.4, trace) * 0.5;
    return col;
}
