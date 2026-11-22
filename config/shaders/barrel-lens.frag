// label: Barrel Lens
// emoji: 📷
// category: Distortion · Lens
// tags: barrel, toy camera, lomo, blur, vignette, lens
// description: Toy-camera lens with barrel bulge, edge-weighted blur and a heavy vignette.
// source: original
// license: original
// order: 10
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 p = uv - 0.5;
    p.x *= SCREEN_SIZE.x / SCREEN_SIZE.y;
    float r2 = dot(p, p);
    vec2 q = p * 0.76 * (1.0 + 0.35 * r2);
    q.x /= SCREEN_SIZE.x / SCREEN_SIZE.y;
    vec2 u = q + 0.5;
    float blur = 0.0008 + 0.012 * r2;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < 12; i++) {
        float a = float(i) * 2.399963;
        float rr = sqrt((float(i) + 0.5) / 12.0);
        vec2 o = vec2(cos(a), sin(a)) * rr * blur;
        o.x *= SCREEN_SIZE.y / SCREEN_SIZE.x;
        acc += texture(tex, clamp(u + o, 0.0, 1.0)).rgb;
    }
    vec3 col = acc / 12.0;
    float v = smoothstep(1.3, 0.1, r2 * 1.8);
    col *= mix(0.3, 1.0, v);
    col = mix(col, col * vec3(1.06, 1.0, 0.92), 0.6);
    return col;
}
