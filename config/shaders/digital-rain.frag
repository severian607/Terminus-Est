// label: Digital Rain
// emoji: 💚
// category: Overlay
// tags: matrix, rain, glyphs, green, falling, code
// description: Falling green glyph columns drawn over dark areas only; bright content is left untouched.
// source: original (hash-driven pseudo-glyph columns)
// license: original
// order: 20
// animated: yes
// samples: no

float drHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 res = SCREEN_SIZE;
    vec2 cell = vec2(16.0, 24.0);
    vec2 g = uv * res / cell;
    vec2 id = floor(g);
    vec2 f = fract(g);
    float rows = ceil(res.y / cell.y);
    float colH = drHash(vec2(id.x, 1.0));
    float speed = 3.0 + colH * 6.0;
    float off = drHash(vec2(id.x, 7.0)) * rows * 3.0;
    float headRow = mod(time * speed + off, rows * 2.0 + 10.0) - 5.0;
    float dist = headRow - id.y;                       // rows behind the head
    float tail = 6.0 + 12.0 * drHash(vec2(id.x, 3.0));
    float inten = (dist >= 0.0 && dist < tail) ? pow(1.0 - dist / tail, 1.6) : 0.0;
    // pseudo glyph: 4x6 random bit grid, changing slowly
    float tick = floor(time * (2.0 + 4.0 * colH) + id.y * 0.7);
    vec2 gp = floor(vec2(f.x * 4.0, f.y * 6.0));
    float seed = drHash(id + vec2(tick, 0.0));
    float bit = step(0.5, drHash(gp + seed * 97.0));
    vec2 inner = abs(f - 0.5);
    float box = step(inner.x, 0.36) * step(inner.y, 0.4);
    float glyph = bit * box;
    float head = (dist >= 0.0 && dist < 1.0) ? 1.0 : 0.0;
    vec3 rain = vec3(0.1, 1.0, 0.35) * glyph * inten;
    rain += vec3(0.7, 1.0, 0.8) * glyph * head;
    rain += vec3(0.0, 0.08, 0.02) * inten * box;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float mask = 1.0 - smoothstep(0.1, 0.5, lum);
    return c + rain * mask * 0.9;
}
