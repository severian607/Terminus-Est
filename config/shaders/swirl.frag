// label: Swirl
// emoji: 🌀
// category: Distortion · Lens
// tags: swirl, twirl, vortex, whirlpool, spin
// description: Slow twirl around the screen centre that fades out with distance.
// source: original (classic twirl)
// license: original
// order: 20
// animated: yes
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    float asp = SCREEN_SIZE.x / SCREEN_SIZE.y;
    vec2 p = uv - 0.5;
    p.x *= asp;
    float r = length(p);
    float fall = smoothstep(0.7, 0.0, r);
    float ang = (1.0 + 0.4 * sin(time * 0.5)) * fall * fall;
    float s = sin(ang), co = cos(ang);
    p = mat2(co, -s, s, co) * p;
    p.x /= asp;
    vec2 u = p + 0.5;
    u = 1.0 - abs(1.0 - mod(u, 2.0));
    return texture(tex, u).rgb;
}
