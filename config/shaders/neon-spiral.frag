// label: Neon Spiral
// emoji: 🌀
// category: Psychedelic
// tags: neon, spiral, purple, overlay, swirl, trippy
// description: A slowly rotating neon-purple spiral overlaid on the screen.
// source: https://github.com/lostjared/shaders (neon, audio/uniforms removed)
// license: MIT
// order: 20
// animated: yes
// samples: no

vec3 overlay_blend(vec3 base, vec3 blend, float opacity) {
    vec3 c2 = blend * 2.0;
    vec3 c1 = 1.0 - 2.0 * (1.0 - blend);
    vec3 result = mix(base * c2, c1, step(0.5, base));
    return mix(base, result, opacity);
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 d = 2.0 * (uv - 0.5) * vec2(SCREEN_SIZE.x / SCREEN_SIZE.y, 1.0);
    float dist = length(d);
    float ang = atan(d.y, d.x) + time * 1.5;
    float spiral = cos(10.0 * dist - ang);
    float mask = smoothstep(0.1, 0.2, abs(spiral) - dist * 0.25);
    return overlay_blend(c, vec3(0.62, 0.20, 1.0), mask * 0.55);
}
