// label: Calm Colors
// emoji: 🍃
// category: Comfort
// tags: calm, muted, desaturate, soft, low saturation, relax
// description: Muted colours and softened white for a low-stimulation screen.
// source: original
// license: original
// order: 60
// animated: no
// samples: no

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}
vec3 effect(vec3 c, vec2 uv) {
    return mix(vec3(luma(c)), c, 0.7) * 0.92 + 0.02;
}
