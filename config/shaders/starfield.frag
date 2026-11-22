// label: Starfield
// emoji: 🌌
// category: Overlay
// tags: stars, warp, hyperspace, tunnel, space, streak
// description: Warp-speed stars streak outward from the centre, drawn only over the dark parts of the screen.
// source: original (polar warp-streak starfield)
// license: original
// order: 10
// animated: yes
// samples: no

float sfHash(float n) { return fract(sin(n * 78.233) * 43758.5453); }

vec3 effect(vec3 c, vec2 uv) {
    vec2 res = SCREEN_SIZE;
    vec2 p = (uv - 0.5) * vec2(res.x / res.y, 1.0);
    float r = length(p);
    float ang = atan(p.y, p.x) / 6.2831853 + 0.5;
    vec3 add = vec3(0.0);
    for (int l = 0; l < 4; l++) {
        float fl = float(l);
        float n = 36.0 + fl * 29.0;
        float sid = floor(ang * n);
        float f = fract(ang * n) - 0.5;
        float h = sfHash(sid + fl * 31.7);
        float z = fract(h * 13.0 + time * (0.12 + 0.05 * fl));
        float head = z * z * 1.3;
        float len = 0.01 + head * 0.35;
        float along = clamp(1.0 - (head - r) / len, 0.0, 1.0) * step(r, head);
        float tang = abs(f) * 6.2831853 * r / n * res.y * 0.5;     // approx pixels from the ray
        float wid = 0.6 + head * 2.0;
        float core = smoothstep(wid, 0.0, tang);
        float vis = step(0.35, sfHash(sid * 3.1 + fl * 9.0));
        float b = along * along * core * vis * smoothstep(0.02, 0.2, r) * (0.5 + head);
        vec3 tint = mix(vec3(0.6, 0.75, 1.0), vec3(1.0, 0.85, 0.7), sfHash(sid + 5.0 * fl));
        add += tint * b;
    }
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float mask = 1.0 - smoothstep(0.08, 0.55, lum);
    vec3 bg = vec3(0.0, 0.004, 0.02) * (1.0 - smoothstep(0.0, 0.9, r));
    return c + (add * 1.0 + bg) * mask;
}
