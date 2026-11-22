// label: Blueprint
// emoji: 📐
// category: Art · Line
// tags: blueprint, edges, sobel, technical, grid, cyanotype
// description: White line-art edges on a cyanotype-blue drafting grid.
// source: original (Sobel edge detection)
// license: original
// order: 75
// animated: no
// samples: yes

float lum(vec2 p) { return dot(texture(tex, p).rgb, vec3(0.299, 0.587, 0.114)); }

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = 1.0 / SCREEN_SIZE;
    float tl = lum(uv + px * vec2(-1, -1)), t = lum(uv + px * vec2(0, -1)), tr = lum(uv + px * vec2(1, -1));
    float l = lum(uv + px * vec2(-1, 0)), r = lum(uv + px * vec2(1, 0));
    float bl = lum(uv + px * vec2(-1, 1)), b = lum(uv + px * vec2(0, 1)), br = lum(uv + px * vec2(1, 1));
    float gx = -tl - 2.0 * l - bl + tr + 2.0 * r + br;
    float gy = -tl - 2.0 * t - tr + bl + 2.0 * b + br;
    float edge = smoothstep(0.12, 0.45, length(vec2(gx, gy)));
    vec2 p = uv * SCREEN_SIZE;
    float minor = 1.0 - step(1.0, min(mod(p.x, 20.0), mod(p.y, 20.0)));
    float major = 1.0 - step(1.5, min(mod(p.x, 100.0), mod(p.y, 100.0)));
    vec3 bg = vec3(0.04, 0.20, 0.45) + minor * 0.05 + major * 0.08;
    bg += 0.10 * lum(uv);                        // faint ghost of the content
    return mix(bg, vec3(0.92, 0.97, 1.0), edge);
}
