// label: Earthquake
// emoji: 🫨
// category: Glitch
// tags: shake, quake, tremor, rumble, jitter, rgb split
// description: Screen shake bursts every few seconds with decaying jitter and a little RGB split on each hit.
// source: original
// license: original
// order: 130
// animated: yes
// samples: yes

float eqHash(float n) { return fract(sin(n * 127.1) * 43758.5453); }

vec3 effect(vec3 c, vec2 uv) {
    float period = 3.5;
    float cycle = floor(time / period);
    float t = time - cycle * period;
    float jitterStep = floor(time * 30.0);
    float amp = exp(-t * 2.2) * (0.6 + 0.4 * eqHash(cycle));
    // aftershock
    float t2 = max(t - 1.1, 0.0);
    amp += step(1.1, t) * exp(-t2 * 5.0) * 0.4;
    vec2 off = (vec2(eqHash(jitterStep), eqHash(jitterStep + 17.0)) - 0.5) * 2.0;
    off *= amp * vec2(0.012, 0.018);
    off.x += sin(uv.y * 40.0 + time * 60.0) * amp * 0.0015;
    float split = amp * 0.004;
    vec2 u = uv + off;
    vec3 col;
    col.r = texture(tex, clamp(u + vec2(split, 0.0), 0.0, 1.0)).r;
    col.g = texture(tex, clamp(u, 0.0, 1.0)).g;
    col.b = texture(tex, clamp(u - vec2(split, 0.0), 0.0, 1.0)).b;
    return col;
}
