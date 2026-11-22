// label: AMOLED Mono
// emoji: 🖤
// category: Color · Mono & Tint
// tags: amoled, oled, black, monochrome, battery, true black
// description: Greyscale with crushed shadows so dark areas are pure OLED black.
// source: original (idea: github.com/sijan-dev/HyprShades amoled, no licence, reimplemented)
// license: original
// order: 80
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return vec3(smoothstep(0.05, 0.92, l));
}
