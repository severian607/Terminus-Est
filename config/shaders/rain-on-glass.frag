// label: Rain on Glass
// emoji: 🌧️
// category: Distortion · Water
// tags: rain, drops, window, glass, refraction, heartfelt
// description: Raindrops slide down the glass, refracting the screen behind with small droplets and trails.
// source: drop layers after Heartfelt by BigWings (https://www.shadertoy.com/view/ltffzl), simplified
// license: original
// order: 80
// animated: yes
// samples: yes

vec3 rgN13(float p) {
    vec3 p3 = fract(vec3(p) * vec3(0.1031, 0.11369, 0.13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
}
float rgSaw(float b, float t) { return smoothstep(0.0, b, t) * smoothstep(1.0, b, t); }

// x = drop mask, y = trail mask
vec2 rgLayer(vec2 uv, float t, vec2 gsz) {
    vec2 UV = uv;
    uv.y += t * 0.75;
    vec2 a = gsz;
    vec2 grid = a * 2.0;
    vec2 id = floor(uv * grid);
    float colShift = rgN13(id.x).x;
    uv.y += colShift;
    id = floor(uv * grid);
    vec3 n = rgN13(id.x * 35.2 + id.y * 2376.1);
    vec2 st = fract(uv * grid) - vec2(0.5, 0.0);
    float x = n.x - 0.5;
    float y = UV.y * 20.0;
    float wiggle = sin(y + sin(y));
    x += wiggle * (0.5 - abs(x)) * (n.z - 0.5);
    x *= 0.7;
    float ti = fract(t + n.z);
    y = (rgSaw(0.85, ti) - 0.5) * 0.9 + 0.5;
    vec2 p = vec2(x, y);
    float d = length((st - p) * grid.yx);
    float mainDrop = smoothstep(0.4, 0.0, d);
    float r = sqrt(smoothstep(1.0, y, st.y));
    float cd = abs(st.x - x);
    float trail = smoothstep(0.23 * r, 0.15 * r * r, cd);
    float trailFront = smoothstep(-0.02, 0.02, st.y - y);
    trail *= trailFront * r * r;
    float yy = UV.y;
    float trail2 = smoothstep(0.2 * r, 0.0, cd);
    float droplets = max(0.0, sin(yy * (1.0 - yy) * 120.0 - st.y)) * trail * trail2 * n.z;
    yy = fract(yy * 10.0) + (st.y - 0.5);
    float dd = length(st - vec2(x, yy));
    droplets = smoothstep(0.3, 0.0, dd);
    float m = mainDrop + droplets * r * trailFront;
    return vec2(m, trail);
}

vec2 rgDrops(vec2 uv, float t) {
    vec2 a = rgLayer(uv, t, vec2(6.0, 1.0));
    vec2 b = rgLayer(uv * 1.85 + 7.5, t * 1.1, vec2(6.0, 1.0));
    return vec2(max(a.x, b.x * 0.9), max(a.y, b.y));
}

vec3 effect(vec3 c, vec2 uv) {
    float asp = SCREEN_SIZE.x / SCREEN_SIZE.y;
    vec2 p = vec2(uv.x * asp, uv.y) * 0.9;
    float t = time * 0.25;
    vec2 e = vec2(0.0015, 0.0);
    vec2 c0 = rgDrops(p, t);
    vec2 cx = rgDrops(p + e.xy, t);
    vec2 cy = rgDrops(p + e.yx, t);
    vec2 n = vec2(cx.x - c0.x, cy.x - c0.x);
    float fog = 1.0 - clamp(c0.x + c0.y * 2.0, 0.0, 1.0);
    vec2 u = uv - n * 1.6 * vec2(1.0 / asp, 1.0) * 6.0 * 0.01 * 10.0;
    u = clamp(u, 0.0, 1.0);
    vec3 sharp = texture(tex, u).rgb;
    vec2 px = 3.0 / SCREEN_SIZE;
    vec3 soft = (texture(tex, clamp(uv + vec2(px.x, px.y), 0.0, 1.0)).rgb +
                 texture(tex, clamp(uv + vec2(-px.x, px.y), 0.0, 1.0)).rgb +
                 texture(tex, clamp(uv + vec2(px.x, -px.y), 0.0, 1.0)).rgb +
                 texture(tex, clamp(uv + vec2(-px.x, -px.y), 0.0, 1.0)).rgb) * 0.25;
    vec3 col = mix(sharp, soft, fog * 0.7);
    col *= 1.0 - 0.1 * fog;
    col += vec3(0.6, 0.75, 1.0) * clamp(c0.x * 0.14, 0.0, 0.12);
    return col;
}
