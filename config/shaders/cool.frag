// label: Cool
// emoji: 🧊
// category: Color · Mono & Tint
// tags: cool, blue, cold, tint, icy
// description: Cool bluish tint.
// source: https://github.com/rokokol/hyprland-screen-shader
// license: MIT
// order: 30
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    return clamp(c * vec3(0.85, 0.95, 1.12), 0.0, 1.0);
}
