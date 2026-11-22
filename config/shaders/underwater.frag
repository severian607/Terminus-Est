// label: Underwater
// emoji: 🌊
// category: Distortion · Water
// tags: water, caustics, wobble, ocean, blue, swim
// description: Gentle swimming distortion, teal tint with depth falloff, and drifting caustic light.
// source: caustic pattern after https://www.shadertoy.com/view/MdlXz8 (Dave_Hoskins / joltz0r), simplified
// license: original
// order: 55
// animated: yes
// samples: yes

float caustic(vec2 p, float t) {
    vec2 i = p;
    float c = 1.0;
    for (int n = 0; n < 4; n++) {
        float tt = t * (1.0 - 3.5 / float(n + 1));
        i = p + vec2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
        c += 1.0 / length(vec2(p.x / (sin(i.x + tt) / 0.005), p.y / (cos(i.y + tt) / 0.005)));
    }
    c /= 4.0;
    return 1.17 - pow(c, 1.4);
}

vec3 effect(vec3 c0, vec2 uv) {
    vec2 res = SCREEN_SIZE;
    vec2 w = vec2(sin(uv.y * 14.0 + time * 1.3), cos(uv.x * 11.0 + time * 1.1)) * 0.0035;
    vec3 col = texture(tex, uv + w).rgb;
    float aspect = res.x / res.y;
    vec2 p = mod(uv * vec2(aspect, 1.0) * 6.2831853, 6.2831853) - 250.0;
    float cs = pow(abs(caustic(p, time * 0.5 + 23.0)), 8.0);
    col *= vec3(0.55, 0.85, 1.0);
    col = mix(col, col * vec3(0.5, 0.75, 1.0), uv.y * 0.5);   // darker toward the bottom
    col += vec3(0.35, 0.7, 0.9) * cs * 0.12;
    return col;
}
