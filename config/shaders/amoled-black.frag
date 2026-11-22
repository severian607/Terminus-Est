// label: AMOLED Black
// emoji: ⬛
// category: Comfort
// tags: amoled, oled, black crush, battery, dark, true black
// description: Keeps colour but crushes near-black to pure black for OLED panels.
// source: original
// license: original
// order: 70
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    return max(c - 0.05, 0.0) / 0.95;
}
