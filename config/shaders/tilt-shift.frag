// label: Tilt-Shift
// emoji: 🏙️
// category: Distortion · Lens
// tags: tilt shift, miniature, blur, depth of field, toy
// description: Sharp horizontal band in the middle, blurred toward top and bottom, with a saturation boost for a miniature look.
// source: original
// license: original
// order: 45
// animated: no
// samples: yes

vec3 effect(vec3 c, vec2 uv) {
    float d = abs(uv.y - 0.5);
    float amt = smoothstep(0.12, 0.5, d);
    vec2 px = 1.0 / SCREEN_SIZE;
    float rad = amt * 6.0;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int i = -4; i <= 4; i++) {
        for (int j = -4; j <= 4; j++) {
            vec2 o = vec2(i, j) * 0.25;
            float w = exp(-dot(o, o) * 0.9);
            sum += w * texture(tex, uv + o * rad * px * 2.0).rgb;
            wsum += w;
        }
    }
    vec3 col = sum / wsum;
    float g = dot(col, vec3(0.299, 0.587, 0.114));
    return mix(vec3(g), col, 1.35);
}
