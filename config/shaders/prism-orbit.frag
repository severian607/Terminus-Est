// label: Prism Orbit
// emoji: 🔺
// category: Psychedelic
// tags: prism, chromatic, orbit, rgb, shimmer, dreamy
// description: Red and blue ghosts slowly orbit the picture like a prism lens.
// source: original
// license: original
// order: 60
// animated: yes
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    float a = time * 0.8;
    vec2 o = vec2(cos(a), sin(a)) * 3.0 / SCREEN_SIZE;
    return vec3(texture(tex, uv + o).r, c.g, texture(tex, uv - o).b);
}
