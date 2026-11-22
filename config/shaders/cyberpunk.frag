// label: Cyberpunk
// emoji: 🌆
// category: Color · Grade
// tags: neon, cyberpunk, chromatic aberration, teal, magenta, glow
// description: Neon grade with edge chromatic aberration and a cyan/magenta glow.
// source: original
// license: original
// order: 150
// animated: no
// samples: yes
// raw: no

vec3 effect(vec3 c, vec2 uv) {
    vec2 d = uv - 0.5;
    vec2 off = d * (0.0012 + 0.010 * dot(d, d) * 4.0);   // aberration grows toward the edges
    c = vec3(texture(tex, uv + off).r,
             texture(tex, uv).g,
             texture(tex, uv - off).b);
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, 1.5);                            // saturation boost
    c *= mix(vec3(0.92, 0.95, 1.12), vec3(1.12, 0.90, 1.08), smoothstep(0.2, 0.8, l)); // teal shadows, magenta lights
    c += vec3(0.6, 0.25, 0.9) * 0.15 * smoothstep(0.7, 1.0, l);                        // neon glow
    return c;
}
