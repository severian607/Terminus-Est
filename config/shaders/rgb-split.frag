// label: RGB Split
// emoji: 🔴
// category: Glitch
// tags: rgb split, chromatic, 3d glasses, anaglyph, fringe
// description: Fixed 2px red/blue horizontal fringe, like cheap anaglyph glasses.
// source: original
// license: original
// order: 70
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 s = vec2(2.0 / SCREEN_SIZE.x, 0.0);
    return vec3(texture(tex, uv + s).r, c.g, texture(tex, uv - s).b);
}
