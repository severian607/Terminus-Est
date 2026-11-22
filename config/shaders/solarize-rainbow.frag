// label: Solarize Rainbow
// emoji: 🌈
// category: Psychedelic
// tags: solarize, rainbow, psychedelic, acid, palette
// description: Solarized shadows with a drifting rainbow palette keyed to brightness.
// source: original
// license: original
// order: 30
// animated: yes
// samples: no

vec3 pal(float t) {
    return 0.5 + 0.5 * cos(6.28318 * (t + vec3(0.0, 0.33, 0.67)));
}
vec3 effect(vec3 c, vec2 uv) {
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 sol = mix(c, 1.0 - c, smoothstep(0.4, 0.7, c));
    return mix(sol, sol * pal(l + time * 0.05) * 1.6, 0.6);
}
