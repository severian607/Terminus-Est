// label: Amber Monitor
// emoji: 🟠
// category: Retro · CRT
// tags: amber, monochrome, terminal, phosphor, vintage
// description: Amber monochrome terminal look with light scanlines.
// source: https://github.com/lostjared/shaders (game_amber_mono)
// license: MIT
// order: 90
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float l = pow(dot(c, vec3(0.299, 0.587, 0.114)), 0.85);
    float scan = 0.88 + 0.12 * sin(uv.y * SCREEN_SIZE.y * 1.5);
    return vec3(1.0, 0.65, 0.10) * l * scan;
}
