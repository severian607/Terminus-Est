// label: Cross Process
// emoji: 🧪
// category: Color · Grade
// tags: cross process, slide film, green, yellow, lomo, film
// description: Slide film in the wrong chemicals: green shadows, yellow highlights, crushed curves.
// source: original
// license: original
// order: 40
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    vec3 s = c * c * (3.0 - 2.0 * c);
    vec3 col;
    col.r = mix(c.r, s.r, 0.9) * 1.05;
    col.g = mix(c.g, s.g, 0.4) + 0.06;
    col.b = 0.15 + c.b * 0.6 - s.b * 0.1;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    col += vec3(0.0, 0.06, -0.04) * (1.0 - l);
    col += vec3(0.12, 0.10, -0.12) * smoothstep(0.5, 1.0, l);
    col = mix(vec3(dot(col, vec3(0.299, 0.587, 0.114))), col, 1.2);
    return clamp(col, 0.0, 1.0);
}
