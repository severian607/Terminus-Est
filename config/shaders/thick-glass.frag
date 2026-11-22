// label: Thick Glass
// emoji: 🔍
// category: Distortion · Lens
// tags: glass, refraction, wobble, thick, lens, underwater
// description: Gentle slow refraction like looking through thick, uneven glass.
// source: https://github.com/lostjared/shaders (thick_glass, strength reduced)
// license: MIT
// order: 50
// animated: yes
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    float n = sin(uv.x * 10.0 + time * 0.5) * cos(uv.y * 10.0 + time * 0.5);
    vec2 p = uv + 0.004 * vec2(n, n);
    return texture(tex, clamp(p, 0.0, 1.0)).rgb;
}
