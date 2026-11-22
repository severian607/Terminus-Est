// label: Plasma Overlay
// emoji: 🔮
// category: Psychedelic
// tags: plasma, tint, lava lamp, colour waves, flowing
// description: Slow flowing plasma colours multiplied over the screen.
// source: original
// license: original
// order: 40
// animated: yes
// samples: no

vec3 pal(float t) {
    return 0.5 + 0.5 * cos(6.28318 * (t + vec3(0.0, 0.33, 0.67)));
}
vec3 effect(vec3 c, vec2 uv) {
    vec2 p = (uv - 0.5) * vec2(SCREEN_SIZE.x / SCREEN_SIZE.y, 1.0) * 4.0;
    float v = sin(p.x + time * 0.5) + sin(p.y * 1.3 - time * 0.4)
            + sin((p.x + p.y) * 0.7 + time * 0.3) + sin(length(p) * 1.5 - time * 0.6);
    vec3 tint = pal(v * 0.125 + 0.5 + time * 0.03);
    return mix(c, c * tint * 1.7, 0.55);
}
