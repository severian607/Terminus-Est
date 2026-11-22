// label: Night Dim
// emoji: 🌙
// category: Comfort
// tags: dim, dark, night, low brightness, warm, bedtime
// description: Darkens below the backlight minimum and warms the picture slightly.
// source: original
// license: original
// order: 50
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    return pow(c, vec3(1.15)) * vec3(0.72, 0.66, 0.56);
}
