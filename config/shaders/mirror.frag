// label: Mirror
// emoji: 🪞
// category: Distortion · Lens
// tags: mirror, symmetry, reflect, flip, half
// description: Mirrors the left half of the screen onto the right half.
// source: https://github.com/lostjared/shaders (mirror1, idea)
// license: MIT
// order: 40
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    if (uv.x < 0.5) return c;
    return texture(tex, vec2(1.0 - uv.x, uv.y)).rgb;
}
