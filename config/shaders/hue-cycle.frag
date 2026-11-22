// label: Hue Cycle
// emoji: 🔄
// category: Psychedelic
// tags: hue, rainbow, cycle, colour rotation, trippy
// description: Slowly rotates every colour around the hue wheel (about 18 s per loop).
// source: original
// license: original
// order: 10
// animated: yes
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float a = time * 0.35;
    const vec3 k = vec3(0.57735);
    float ca = cos(a);
    return c * ca + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - ca);
}
