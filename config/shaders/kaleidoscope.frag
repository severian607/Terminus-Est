// label: Kaleidoscope
// emoji: 🔱
// category: Psychedelic
// tags: kaleidoscope, mirror, symmetry, rotate, segments
// description: Six-way kaleidoscope that slowly rotates around the screen centre.
// source: https://github.com/lostjared/shaders (kale, aspect/edge fixes)
// license: MIT
// order: 50
// animated: yes
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    float aspect = SCREEN_SIZE.x / SCREEN_SIZE.y;
    vec2 pos = (uv - 0.5) * vec2(aspect, 1.0);
    float r = length(pos);
    float seg = 6.28318531 / 6.0;
    float a = mod(atan(pos.y, pos.x), seg);
    a = abs(a - seg * 0.5);
    pos = r * vec2(cos(a), sin(a));
    float rot = time * 0.15;
    mat2 m = mat2(cos(rot), -sin(rot), sin(rot), cos(rot));
    pos = m * pos;
    vec2 p = pos / vec2(aspect, 1.0) + 0.5;
    p = 1.0 - abs(1.0 - mod(p, 2.0));
    return texture(tex, p).rgb;
}
