// label: Chromatic Aberration
// emoji: 🔬
// category: Distortion · Lens
// tags: chromatic aberration, lens, rgb, fringe, edges, cinematic
// description: Lens colour fringing that grows toward the edges; centre stays sharp.
// source: original
// license: original
// order: 100
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 d = uv - 0.5;
    vec2 off = d * dot(d, d) * 0.04;
    return vec3(texture(tex, uv + off).r, c.g, texture(tex, uv - off).b);
}
