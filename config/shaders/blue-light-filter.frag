// label: Blue Light Filter
// emoji: 🕯️
// category: Comfort
// tags: blue light, filter, evening, mild warm, eye strain
// description: Mild blue-light reduction: whites turn slightly cream, colours stay.
// source: original (idea: github.com/sijan-dev/HyprShades blue-light-filter, no licence, reimplemented)
// license: original
// order: 30
// animated: no
// samples: no

vec3 effect(vec3 c, vec2 uv) {
    return c * vec3(1.0, 0.93, 0.78);
}
