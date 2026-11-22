// label: VHS Tape
// emoji: 📼
// category: Retro · Tape & Film
// tags: vhs, tape, analog, wobble, chroma bleed, 80s
// description: Soft wobbling VHS picture: chroma bleed, tape noise, noisy bottom edge.
// source: https://github.com/lostjared/shaders (vhs, idea)
// license: MIT
// order: 30
// animated: yes
// samples: yes

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec3 effect(vec3 c, vec2 uv) {
    vec2 ts = 1.0 / SCREEN_SIZE;
    float y = uv.y * SCREEN_SIZE.y;
    float f = floor(time * 30.0);
    float foot = 1.0 - smoothstep(0.0, 0.06, uv.y);
    float wob = 0.0006 * sin(uv.y * 9.0 + time * 1.4)
              + (hash21(vec2(floor(y), f)) - 0.5) * (0.0008 + foot * 0.03);
    vec2 p = vec2(clamp(uv.x + wob, 0.0, 1.0), uv.y);
    vec3 col;
    col.r = 0.5 * (texture(tex, p + vec2(2.0 * ts.x, 0.0)).r + texture(tex, p + vec2(4.0 * ts.x, 0.0)).r);
    col.g = texture(tex, p).g;
    col.b = 0.5 * (texture(tex, p - vec2(2.0 * ts.x, 0.0)).b + texture(tex, p - vec2(4.0 * ts.x, 0.0)).b);
    float l = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(l), col, 0.82) * vec3(0.96, 0.95, 1.02) + vec3(0.02, 0.02, 0.04);
    col += (hash21(vec2(floor(uv.x * SCREEN_SIZE.x), y) + f * 13.0) - 0.5) * (0.04 + foot * 0.25);
    col *= 0.95 + 0.05 * sin(y * 3.14159265);
    return col;
}
