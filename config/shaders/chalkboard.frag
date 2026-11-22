// label: Chalkboard
// emoji: 🧑‍🏫
// category: Art · Paint & Ink
// tags: chalk, crosshatch, sketch, blackboard, drawing
// description: Crosshatched chalk on a slate board; brighter areas get more hatch layers.
// source: original (hatching idea after flockaroo, https://www.shadertoy.com/view/MsSGD1)
// license: original
// order: 65
// animated: no
// samples: no

float h2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

vec3 effect(vec3 c, vec2 uv) {
    vec2 p = uv * SCREEN_SIZE;
    float l = pow(dot(c, vec3(0.299, 0.587, 0.114)), 0.8);
    float ink = 0.0;
    if (l > 0.20) ink += step(mod(p.x + p.y, 8.0), 1.2);
    if (l > 0.36) ink += step(mod(p.x - p.y, 8.0), 1.2);
    if (l > 0.54) ink += step(mod(p.x + p.y - 4.0, 8.0), 1.2);
    if (l > 0.72) ink += step(mod(p.x - p.y - 4.0, 8.0), 1.2);
    ink = clamp(ink, 0.0, 1.0) * (0.75 + 0.25 * h2(floor(p / 2.0)));   // chalky dropout
    vec3 board = vec3(0.07, 0.10, 0.09) + 0.02 * h2(floor(p));
    vec3 chalk = mix(vec3(0.92), c / max(max(c.r, max(c.g, c.b)), 0.05), 0.35);
    return mix(board, chalk, ink);
}
