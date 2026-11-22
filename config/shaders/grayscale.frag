// label: Grayscale
// emoji: ⚫
// category: Color · Mono & Tint
// tags: grayscale, black and white, monochrome, desaturate
// description: Plain greyscale using Rec. 709 luminance.
// source: https://github.com/rokokol/hyprland-screen-shader
// license: MIT
// order: 10
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    return vec3(dot(c, vec3(0.2126, 0.7152, 0.0722)));
}
