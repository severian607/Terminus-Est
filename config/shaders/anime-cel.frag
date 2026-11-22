// label: Anime Cel
// emoji: 🖍️
// category: Art · Paint & Ink
// tags: cel shading, anime, cartoon, toon, posterize, outline
// description: Cel-shaded cartoon look: stepped brightness with darkened outlines.
// source: https://github.com/lostjared/shaders (game_anime_cel)
// license: MIT
// order: 80
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = 1.0 / SCREEN_SIZE;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    float steps = 5.0;
    float bin = floor(lum * steps) / steps + 0.5 / steps;
    vec3 quant = c * (bin / max(lum, 0.001));
    float gx = dot(texture(tex, uv + vec2(px.x, 0.0)).rgb - texture(tex, uv - vec2(px.x, 0.0)).rgb, vec3(0.333));
    float gy = dot(texture(tex, uv + vec2(0.0, px.y)).rgb - texture(tex, uv - vec2(0.0, px.y)).rgb, vec3(0.333));
    float edge = 1.0 - smoothstep(0.05, 0.18, sqrt(gx * gx + gy * gy));
    return quant * edge;
}
