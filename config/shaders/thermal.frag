// label: Thermal Camera
// emoji: 🔥
// category: Color · Vision
// tags: thermal, heat map, infrared, predator, false colour
// description: Maps brightness to a thermal-camera blue-purple-red-yellow-white ramp.
// source: original
// license: original
// order: 120
// animated: no
// samples: no

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}
vec3 effect(vec3 c, vec2 uv) {
    float t = clamp(luma(c), 0.0, 1.0);
    vec3 col = mix(vec3(0.0, 0.0, 0.15), vec3(0.4, 0.0, 0.6), smoothstep(0.0, 0.25, t));
    col = mix(col, vec3(0.9, 0.1, 0.1), smoothstep(0.25, 0.5, t));
    col = mix(col, vec3(1.0, 0.85, 0.1), smoothstep(0.5, 0.8, t));
    return mix(col, vec3(1.0), smoothstep(0.8, 1.0, t));
}
