// label: Water Ripple
// emoji: 💧
// category: Distortion · Water
// tags: ripple, water, rings, waves, centre, pond
// description: Soft rings radiate from the screen centre and refract the picture.
// source: https://github.com/lostjared/shaders (ripple, audio removed, reworked)
// license: MIT
// order: 20
// animated: yes
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 asp = vec2(SCREEN_SIZE.x / SCREEN_SIZE.y, 1.0);
    vec2 d = (uv - 0.5) * asp;
    float r = length(d);
    float w = sin(r * 38.0 - time * 2.5) * exp(-r * 2.5);
    vec2 p = uv + (d / max(r, 0.0001)) / asp * w * 0.004;
    return texture(tex, clamp(p, 0.0, 1.0)).rgb;
}
