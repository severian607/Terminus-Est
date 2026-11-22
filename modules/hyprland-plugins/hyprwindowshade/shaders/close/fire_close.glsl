#version 320 es
// Burn-My-Windows style fire: the window burns away from top to bottom.
// Adapted from Burn-My-Windows fire.frag (Simon Schneegans, GPL-3.0) via
// github.com/theguriev/hyprburn (GPL-3.0-or-later). Simplex noise: Ashima webgl-noise (MIT).
// @duration 1.2
precision highp float;

in vec2 v_texcoord;
out vec4 fragColor;
uniform sampler2D tex;

uniform float progress;
uniform float seed;
uniform vec2 surface_size;
uniform vec4 window_rect;

const float SCALE = 1.0;      // flame size
const float MOVEMENT = 1.2;   // upward drift of the flames
const float DURATION = 1.2;   // seconds, keep equal to @duration
const float HIDE_TIME = 0.4;
const float FADE_WIDTH = 0.1;
const float EDGE_FADE = 70.0;

const vec4 G0 = vec4(0.2980, 0.2000, 0.0980, 0.0);
const vec4 G1 = vec4(0.7059, 0.2157, 0.1176, 0.7);
const vec4 G2 = vec4(1.0000, 0.2980, 0.1490, 0.9);
const vec4 G3 = vec4(1.0000, 0.6510, 0.0980, 1.0);
const vec4 G4 = vec4(1.0000, 1.0000, 1.0000, 1.0);

vec3 mod289v3(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289v2(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x) { return mod289v3(((x * 34.0) + 1.0) * x); }

float snoise(vec2 v) {
    const vec4 C = vec4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    vec2 i = floor(v + dot(v, C.yy));
    vec2 x0 = v - i + dot(i, C.xx);
    vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289v2(i);
    vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
    vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
    m = m * m;
    m = m * m;
    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
    vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

float fbm(vec2 p) {
    const mat2 ROT = mat2(1.6, 1.2, -1.2, 1.6);
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        sum += amp * (0.5 + 0.5 * snoise(p));
        p = ROT * p;
        amp *= 0.5;
    }
    return sum;
}

vec4 fireColor(float v) {
    if (v < 0.0) return G0;
    if (v < 0.2) return mix(G0, G1, v / 0.2);
    if (v < 0.35) return mix(G1, G2, (v - 0.20) / 0.15);
    if (v < 0.5) return mix(G2, G3, (v - 0.35) / 0.15);
    if (v < 0.8) return mix(G3, G4, (v - 0.50) / 0.30);
    return G4;
}

void main() {
    vec2 local = (v_texcoord - window_rect.xy) / window_rect.zw;
    vec2 winSize = surface_size * window_rect.zw;
    vec2 px = local * winSize;
    float t = clamp(local.y, 0.0, 1.0) * (1.0 - FADE_WIDTH);

    float p = 2.0 * progress - progress * progress;
    float burnProgress = clamp(p / HIDE_TIME, 0.0, 1.0);
    float afterBurn = clamp((p - HIDE_TIME) / (1.0 - HIDE_TIME), 0.0, 1.0);

    float windowMask = 1.0 - clamp((burnProgress - t) / FADE_WIDTH, 0.0, 1.0);
    float effectMask = clamp(t * (1.0 - windowMask) / max(burnProgress, 0.0001), 0.0, 1.0);

    if (p > HIDE_TIME) {
        float fade = sqrt(1.0 - afterBurn * afterBurn);
        effectMask *= mix(1.0, 1.0 - t, afterBurn) * fade;
    }

    effectMask *= smoothstep(0.0, 1.0, clamp(px.x / EDGE_FADE, 0.0, 1.0)) *
                  smoothstep(0.0, 1.0, clamp((winSize.x - px.x) / EDGE_FADE, 0.0, 1.0)) *
                  smoothstep(0.0, 1.0, clamp(px.y / EDGE_FADE, 0.0, 1.0)) *
                  smoothstep(0.0, 1.0, clamp((winSize.y - px.y) / EDGE_FADE, 0.0, 1.0));

    vec2 nuv = px / vec2(400.0, 600.0) / SCALE;
    nuv.y += progress * DURATION * MOVEMENT + seed * 50.0;

    float n = fbm(nuv * 4.0) * effectMask;
    vec4 fire = fireColor(n);

    vec4 src = texture(tex, v_texcoord);
    vec4 win = vec4(src.a > 0.001 ? src.rgb / src.a : vec3(0.0), src.a);
    win.a *= windowMask;

    float outA = mix(win.a, 1.0, fire.a);
    vec3 outRGB = outA > 0.001 ? mix(win.rgb * win.a, fire.rgb, fire.a) / outA : vec3(0.0);
    fragColor = vec4(outRGB * outA, outA);
}
