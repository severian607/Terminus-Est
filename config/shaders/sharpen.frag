// label: Sharpen
// emoji: 🔪
// category: Light & Finish
// tags: sharpen, crisp, detail, kernel
// description: Aggressive 3x3 sharpening kernel (centre 9, neighbours -1).
// source: https://github.com/rokokol/hyprland-screen-shader
// license: MIT
// order: 70
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    vec2 px = vec2(fwidth(uv.x), fwidth(uv.y));
    vec3 n  = texture(tex, uv + vec2( 0.0,  px.y)).rgb;
    vec3 s  = texture(tex, uv + vec2( 0.0, -px.y)).rgb;
    vec3 w  = texture(tex, uv + vec2(-px.x,  0.0)).rgb;
    vec3 e  = texture(tex, uv + vec2( px.x,  0.0)).rgb;
    vec3 nw = texture(tex, uv + vec2(-px.x,  px.y)).rgb;
    vec3 ne = texture(tex, uv + vec2( px.x,  px.y)).rgb;
    vec3 sw = texture(tex, uv + vec2(-px.x, -px.y)).rgb;
    vec3 se = texture(tex, uv + vec2( px.x, -px.y)).rgb;
    return clamp(9.0 * c - (n + s + w + e + nw + ne + sw + se), 0.0, 1.0);
}
