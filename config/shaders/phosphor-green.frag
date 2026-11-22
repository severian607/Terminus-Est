// label: Green Phosphor
// emoji: 🟢
// category: Retro · CRT
// tags: green, monochrome, terminal, phosphor, hacker
// description: Green phosphor monochrome terminal with light scanlines.
// source: original
// license: original
// order: 100
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float l = pow(dot(c, vec3(0.299, 0.587, 0.114)), 0.9);
    float scan = 0.9 + 0.1 * sin(uv.y * SCREEN_SIZE.y * 1.5);
    return vec3(0.10, 1.0, 0.25) * l * scan;
}
