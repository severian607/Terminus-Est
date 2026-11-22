// label: Posterize
// emoji: 🎨
// category: Color · Grade
// tags: posterize, levels, pop art, comic, banding
// description: Sharply reduces the shade count for a pop-art poster look.
// source: https://github.com/rokokol/hyprland-screen-shader
// license: MIT
// order: 40
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float levels = 5.0;
    return floor(c * levels + 0.5) / levels;
}
