// label: Warm (Night)
// emoji: 🌅
// category: Comfort
// tags: warm, night, blue light, evening, orange, eye strain
// description: Strong warm filter that cuts blue light for late evening use.
// source: https://github.com/rokokol/hyprland-screen-shader
// license: MIT
// order: 20
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    return c * vec3(1.0, 0.80, 0.58);
}
