// label: ASCII
// emoji: 🔤
// category: Art · Tiles & Shapes
// tags: ascii, text, characters, terminal, glyphs
// description: Rebuilds the screen from coloured text characters chosen by brightness.
// source: https://www.shadertoy.com/view/lssGDj (movAX13h, ASCII Art)
// license: unspecified (Shadertoy default CC BY-NC-SA 3.0)
// order: 60
// animated: no
// samples: yes

float character(int n, vec2 p) {
    p = floor(p * vec2(-4.0, 4.0) + 2.5);
    if (clamp(p.x, 0.0, 4.0) == p.x && clamp(p.y, 0.0, 4.0) == p.y) {
        int a = int(round(p.x) + 5.0 * round(p.y));
        if (((n >> a) & 1) == 1) return 1.0;
    }
    return 0.0;
}

vec3 effect(vec3 c, vec2 uv) {
    vec2 res = SCREEN_SIZE;
    vec2 pix = uv * res;
    vec3 col = texture(tex, (floor(pix / 10.0) * 10.0 + 5.0) / res).rgb;
    float gray = pow((col.r + col.g + col.b) / 3.0, 0.55);   // lift darks so dark UIs still read
    int n = 4096;                               // .
    if (gray > 0.2) n = 65600;                  // :
    if (gray > 0.3) n = 163153;                 // *
    if (gray > 0.4) n = 15255086;               // o
    if (gray > 0.5) n = 13121101;               // &
    if (gray > 0.6) n = 15252014;               // 8
    if (gray > 0.7) n = 13195790;               // @
    if (gray > 0.8) n = 11512810;               // #
    vec2 p = mod(pix / 5.0, 2.0) - vec2(1.0);
    return (col / max(max(col.r, max(col.g, col.b)), 0.1)) * character(n, p);
}
