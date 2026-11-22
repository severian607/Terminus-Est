// label: Vignette
// emoji: 🎯
// category: Light & Finish
// tags: vignette, edges, dark corners, focus
// description: Soft darkening toward the screen edges.
// source: https://github.com/rokokol/hyprland-screen-shader
// license: MIT
// order: 60
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    vec2 d = uv - 0.5;
    float v = smoothstep(0.85, 0.35, length(d));
    return c * mix(0.45, 1.0, v);
}
