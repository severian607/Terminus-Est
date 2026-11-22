// label: Reading
// emoji: 📖
// category: Comfort
// tags: reading, paper, ink, eye care, calm, text
// description: Ink-on-paper range with calmed chroma; no pure white glare.
// source: https://github.com/rokokol/hyprland-screen-shader
// license: MIT
// order: 10
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    const vec3 paper = vec3(0.78, 0.93, 0.80);
    const vec3 ink = vec3(0.10, 0.13, 0.11);
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec3 calm = mix(vec3(lum), c, 0.65);
    return mix(ink, paper, calm);
}
