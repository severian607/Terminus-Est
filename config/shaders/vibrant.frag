// label: Vibrant
// emoji: 🌈
// category: Color · Grade
// tags: vibrant, saturation, punchy, colourful, boost
// description: Boosts saturation and contrast slightly for punchier colour.
// source: original
// license: original
// order: 50
// animated: no
// samples: no

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}
vec3 effect(vec3 c, vec2 uv) {
    vec3 col = mix(vec3(luma(c)), c, 1.35);
    return (col - 0.5) * 1.05 + 0.5;
}
