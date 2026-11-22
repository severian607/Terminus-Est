// label: EGA / CGA 16 Colours
// emoji: 🕹️
// category: Retro · Pixel & Palette
// tags: ega, cga, 16 colour, palette, dos, vga
// description: Snaps every pixel to the nearest colour of the fixed 16-colour EGA palette.
// source: original
// license: original
// order: 10
// animated: no
// samples: no

const vec3 ega_pal[16] = vec3[16](
    vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, 0.667), vec3(0.0, 0.667, 0.0), vec3(0.0, 0.667, 0.667),
    vec3(0.667, 0.0, 0.0), vec3(0.667, 0.0, 0.667), vec3(0.667, 0.333, 0.0), vec3(0.667, 0.667, 0.667),
    vec3(0.333, 0.333, 0.333), vec3(0.333, 0.333, 1.0), vec3(0.333, 1.0, 0.333), vec3(0.333, 1.0, 1.0),
    vec3(1.0, 0.333, 0.333), vec3(1.0, 0.333, 1.0), vec3(1.0, 1.0, 0.333), vec3(1.0, 1.0, 1.0));

vec3 effect(vec3 c, vec2 uv) {
    vec3 best = ega_pal[0];
    float bd = 1e9;
    for (int i = 0; i < 16; i++) {
        vec3 d = (c - ega_pal[i]) * vec3(0.30, 0.59, 0.11) * 3.0;
        float e = dot(d, d);
        if (e < bd) { bd = e; best = ega_pal[i]; }
    }
    return best;
}
