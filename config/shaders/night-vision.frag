// label: Night Vision
// emoji: 🥽
// category: Color · Vision
// tags: night vision, green, goggles, noise, military
// description: Green image-intensifier goggles with gain, grain and a round vignette.
// source: original
// license: original
// order: 90
// animated: yes
// samples: no

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec3 effect(vec3 c, vec2 uv) {
    float l = pow(clamp(dot(c, vec3(0.299, 0.587, 0.114)) * 2.2, 0.0, 1.0), 0.7);
    l += (hash21(floor(uv * SCREEN_SIZE) + floor(time * 24.0) * 17.0) - 0.5) * 0.12;
    vec2 d = (uv - 0.5) * vec2(SCREEN_SIZE.x / SCREEN_SIZE.y, 1.0);
    float vig = mix(0.35, 1.0, smoothstep(0.9, 0.3, length(d)));
    float scan = 0.92 + 0.08 * sin(uv.y * SCREEN_SIZE.y * 1.5);
    return vec3(0.12, 1.0, 0.25) * l * vig * scan;
}
