// label: VHS Tracking
// emoji: 📶
// category: Retro · Tape & Film
// tags: vhs, tracking, roll, snow, tape, distortion
// description: A band of mistracking noise rolls down the screen every few seconds.
// source: https://github.com/lostjared/shaders (vhs-code-tracking-roll)
// license: MIT
// order: 40
// animated: yes
// samples: yes

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec3 effect(vec3 c, vec2 uv) {
    float f = floor(time * 24.0);
    float scanY = uv.y * SCREEN_SIZE.y;
    float rollCenter = fract(time * 0.13);
    float rollDistance = abs(fract(uv.y - rollCenter + 0.5) - 0.5);
    float band = 1.0 - smoothstep(0.015, 0.09, rollDistance);
    float lineNoise = hash21(vec2(floor(scanY), f));
    float wobble = sin(scanY * 0.11 + time * 7.0) * 0.0015;
    vec2 p = uv;
    p.x += wobble + band * (lineNoise - 0.5) * 0.085;
    p.y = fract(p.y + (hash21(vec2(f, 8.0)) - 0.5) * 0.004);
    p = clamp(p, 0.001, 0.999);
    vec3 col;
    col.r = texture(tex, p + vec2(0.0025, 0.0)).r;
    col.g = texture(tex, p).g;
    col.b = texture(tex, p - vec2(0.0035, 0.0)).b;
    float snow = hash21(floor(uv * SCREEN_SIZE) + f * 17.0) - 0.5;
    col += snow * (0.035 + band * 0.18);
    col *= 0.94 + 0.06 * sin(scanY * 3.14159265);
    return col * (1.0 - band * 0.22);
}
