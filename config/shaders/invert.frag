// label: Invert
// emoji: 🌓
// category: Color · Mono & Tint
// tags: invert, negative, inverted, dark mode
// description: Straight colour inversion.
// source: original
// license: original
// order: 100
// animated: no
// samples: no
// raw: no

vec3 effect(vec3 c, vec2 uv) {
    return 1.0 - c;
}
