// label: Lens Flare
// emoji: ✨
// category: Light & Finish
// tags: flare, anamorphic, streak, ghost, bloom, cinematic
// description: Anamorphic blue streaks stretch from bright areas, with soft ghost blobs mirrored across the screen centre.
// source: original (anamorphic streak and ghost technique)
// license: original
// order: 110
// animated: no
// samples: yes

vec3 lfBright(vec2 p) {
    vec3 s = texture(tex, clamp(p, 0.0, 1.0)).rgb;
    float l = dot(s, vec3(0.2126, 0.7152, 0.0722));
    return s * smoothstep(0.45, 0.9, l);
}

vec3 effect(vec3 c, vec2 uv) {
    vec3 streak = vec3(0.0);
    float ws = 0.0;
    for (int i = -16; i <= 16; i++) {
        float fi = float(i);
        float w = exp(-abs(fi) * 0.14);
        streak += lfBright(uv + vec2(fi * 0.011, 0.0)) * w;
        ws += w;
    }
    streak /= ws;
    vec3 col = c + streak * vec3(0.35, 0.6, 1.5) * 1.6;

    vec2 dir = vec2(0.5) - uv;
    vec3 gh = vec3(0.0);
    gh += lfBright(uv + dir * 0.45) * vec3(1.0, 0.6, 0.3);
    gh += lfBright(uv + dir * 0.9) * vec3(0.4, 0.9, 0.6);
    gh += lfBright(uv + dir * 1.6) * vec3(0.5, 0.5, 1.0);
    gh += lfBright(uv - dir * 0.5) * vec3(0.9, 0.4, 0.8);
    float fall = 0.3 + 0.7 * smoothstep(0.0, 0.7, length(dir));
    col += gh * 0.25 * fall;
    return col;
}
