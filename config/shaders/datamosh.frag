// label: Datamosh
// emoji: 🧬
// category: Glitch
// tags: datamosh, smear, compression, macroblock, glitch
// description: Macroblocks drift and smear like a broken video stream.
// source: original
// license: original
// order: 20
// animated: yes
// samples: yes

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
vec3 effect(vec3 c, vec2 uv) {
    vec2 bsz = vec2(48.0) / SCREEN_SIZE;
    vec2 blk = floor(uv / bsz);
    float ph = time / 1.6 + hash21(blk) * 3.0;
    float cyc = floor(ph);
    float f = fract(ph);
    float act = step(0.86, hash21(blk + cyc * 1.37));
    if (act < 0.5) return c;
    vec2 mv = vec2(hash21(blk + cyc * 2.1), hash21(blk + cyc * 3.3)) - 0.5;
    float env = smoothstep(0.0, 0.25, f) * (1.0 - smoothstep(0.7, 1.0, f));
    vec2 org = blk * bsz;
    vec2 p = uv + mv * bsz * 2.0 * env;
    p.y = mix(p.y, org.y + bsz.y * 0.35, env * 0.8);
    return texture(tex, clamp(p, 0.0, 1.0)).rgb;
}
