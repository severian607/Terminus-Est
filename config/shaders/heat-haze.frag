// label: Heat Haze
// emoji: ♨️
// category: Distortion · Water
// tags: heat, haze, shimmer, mirage, wobble, wave
// description: Subtle shimmering mirage wobble; text stays readable.
// source: https://github.com/lostjared/shaders (heat-wave, reworked gentle)
// license: MIT
// order: 10
// animated: yes
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    float y = uv.y * SCREEN_SIZE.y;
    float w = sin(y * 0.045 + time * 2.2) + 0.6 * sin(y * 0.11 - time * 3.1);
    float v = sin(uv.x * SCREEN_SIZE.x * 0.03 + time * 1.7);
    vec2 p = uv + vec2(w * 1.4 / SCREEN_SIZE.x, 0.6 * v / SCREEN_SIZE.y);
    return texture(tex, clamp(p, 0.0, 1.0)).rgb;
}
