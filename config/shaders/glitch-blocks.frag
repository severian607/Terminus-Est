// label: Glitch Blocks
// emoji: 🧱
// category: Glitch
// tags: glitch, blocks, rgb split, displacement, digital
// description: Random rectangular blocks jump sideways with an RGB split.
// source: https://github.com/lostjared/shaders (glitch1, idea)
// license: MIT
// order: 10
// animated: yes
// samples: yes

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec3 effect(vec3 c, vec2 uv) {
    float t = floor(time * 8.0);
    vec2 cell = vec2(floor(uv.x * 12.0), floor(uv.y * 24.0));
    float act = step(0.93, hash21(cell + t * 7.13));
    if (act < 0.5) return c;
    float dx = (hash21(cell.yx + t * 3.7) - 0.5) * 0.12;
    vec2 p = uv + vec2(dx, 0.0);
    vec2 s = vec2(0.004, 0.0);
    vec3 col;
    col.r = texture(tex, clamp(p + s, 0.0, 1.0)).r;
    col.g = texture(tex, clamp(p, 0.0, 1.0)).g;
    col.b = texture(tex, clamp(p - s, 0.0, 1.0)).b;
    return col;
}
