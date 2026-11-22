// label: Sakura Duotone
// emoji: 🌸
// category: Color · Mono & Tint
// tags: sakura, duotone, pink, plum, gradient map, mochi
// description: Gradient map from plum-black shadows to sakura-pink lights.
// source: original
// license: original
// order: 70
// animated: no
// samples: no

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}
vec3 effect(vec3 c, vec2 uv) {
    float l = luma(c);
    vec3 shadow = vec3(0.075, 0.031, 0.086);
    vec3 mid = vec3(0.52, 0.20, 0.38);
    vec3 light = vec3(1.0, 0.70, 0.82);
    return mix(mix(shadow, mid, smoothstep(0.0, 0.55, l)), light, smoothstep(0.45, 1.0, l));
}
