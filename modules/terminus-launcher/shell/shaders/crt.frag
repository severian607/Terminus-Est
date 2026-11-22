#version 440
// The launcher's CRT: one pass over the rendered screen (a ShaderEffectSource of the text layer, transparent ground).
// Build: shaders/build.sh (qsb). Uniforms are set by modules/launcher/Launcher.qml.
//
//   ground       warm near-black glass
//   phosphor     the theme accent (the line and the warm-up glow)
//   size         texture size in px; texel = 1 / size
//   period       scanline period in px; the row height is a whole number of periods, so every row is cut the same
//   scanDepth    how much the last px of each period is darkened (the px before it gets a third of that)
//   bloomFrom    luminance above which a pixel blooms (only the bright tier reaches it)
//   bloom        bloom strength; a second, wider ring of the same taps gives the halation around bright text
//   mask         aperture grille: every other px column is darkened by this much (2 px divides the 14 px cell,
//                so every glyph gets the same columns)
//   grain        phosphor grain, +- this much brightness (per px; animated with motion)
//   corner       radius in px of the tube's rounded corners; the glass darkens toward its curved edge
//   vigBand      px from the sides (x) and from top/bottom (y) over which the vignette darkens the glass; kept inside
//                the launcher's text margins
//   glass        strength of the faint lit centre of the tube and its reflection highlight
//   power        0..1 the picture opens vertically from a line
//   bright       0..1 the tube brightens, unevenly
//   settle       0..1 the text settles (horizontal unrest dies away)
//   fade         0..1 the whole tube; the last step of switching off
//   seed         per-tube constant for the ageing pattern
//   time         seconds since the launcher was shown
//   motion       0..1 the live effects: animated phosphor grain, a faint refresh band drifting down every 7 s and a
//                1-2% slow waver of the brightness. None of it moves pixels, so the text stays on its grid.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 ground;
    vec4 phosphor;
    vec2 size;
    vec2 texel;
    float period;
    float scanDepth;
    float bloomFrom;
    float bloom;
    float power;
    float bright;
    float settle;
    float fade;
    float seed;
    float mask;
    float grain;
    float corner;
    float glass;
    float time;
    float motion;
    vec2 vigBand;
};

layout(binding = 1) uniform sampler2D source;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec2 uv = qt_TexCoord0;
    vec2 px = uv * size;

    // Vertical opening: the picture is squeezed into a band around the middle.
    float halfBand = max(power * 0.5, texel.y);
    float dy = uv.y - 0.5;
    float inBand = 1.0 - smoothstep(halfBand - texel.y, halfBand + texel.y, abs(dy));
    vec2 suv = vec2(uv.x, 0.5 + dy / max(power, 0.002));

    // Settling: each scanline is pushed sideways a little, less and less.
    float unrest = 1.0 - settle;
    float line = floor(suv.y * size.y / period);
    suv.x += unrest * unrest * (hash(vec2(line, seed)) - 0.5) * 10.0 * texel.x;

    vec4 c = texture(source, suv);

    // Bloom from the bright tier only: a golden-angle disc of taps, thresholded on luminance. The outer taps reach
    // further and weigh less: the halation of a strong tube.
    vec3 glow = vec3(0.0);
    vec3 halo = vec3(0.0);
    float wsum = 0.0;
    float hsum = 0.0;
    for (int i = 0; i < 24; i++) {
        float a = float(i) * 2.39996;
        float r = 1.2 + float(i) * 0.42;
        vec2 dir = vec2(cos(a), sin(a));
        vec4 s = texture(source, suv + dir * r * texel);
        float k = smoothstep(bloomFrom, bloomFrom + 0.06, luma(s.rgb));
        float w = 1.0 / (1.0 + r * 0.35);
        glow += s.rgb * k * w;
        wsum += w;
        vec4 h = texture(source, suv + dir * r * 3.2 * texel);
        float kh = smoothstep(bloomFrom, bloomFrom + 0.06, luma(h.rgb));
        halo += h.rgb * kh;
        hsum += 1.0;
    }
    glow /= wsum;
    halo /= hsum;

    // The glass: faintly lit by the beam toward the middle, then the text over it, then bloom and halation.
    vec2 cd = (uv - 0.5) * vec2(1.0, 1.4);
    vec3 glassLit = phosphor.rgb * glass * 0.05 * (1.0 - smoothstep(0.0, 0.75, length(cd)));
    vec3 col = (ground.rgb + glassLit) * (1.0 - c.a) + c.rgb + glow * bloom + halo * bloom * 0.45;

    // The aged tube: low-frequency unevenness and static grain. The vignette lives only in a band along the edges,
    // vigBand px wide (x: the sides, y: top and bottom), which the launcher keeps inside its text margins, so no
    // text is ever darkened by it.
    float age = 0.93 + 0.1 * noise(uv * vec2(2.3, 1.7) + seed * 3.1);
    float ex = min(px.x, size.x - px.x);
    float ey = min(px.y, size.y - px.y);
    float vig = mix(0.45, 1.0, smoothstep(0.0, vigBand.x, ex)) * mix(0.45, 1.0, smoothstep(0.0, vigBand.y, ey));
    col *= age * vig;

    // Phosphor grain: very fine (one px), changing 30 times a second; full strength on the glass, a third on the
    // text so glyph edges stay clean. Without motion it is still and on the glass only.
    float frame = mod(floor(time * 30.0), 997.0) * motion;
    float speck = (hash(floor(px) + seed + vec2(frame * 17.0, frame * 31.0)) - 0.5) * 2.0;
    col *= 1.0 + grain * speck * mix(1.0 - c.a, 1.0 - c.a * 0.67, motion);

    // The refresh band: a faint brighter strip drifting slowly down the glass, one pass every 7 s.
    float bandY = fract(time / 7.0) * 1.4 - 0.2;
    float band = exp(-pow((uv.y - bandY) / 0.06, 2.0));
    col += (col * 0.12 + phosphor.rgb * 0.012) * band * motion;

    // Waver: the brightness drifts by 1-2% on two slow, unrelated periods; never fast enough to read as flicker.
    col *= 1.0 + motion * (0.01 * sin(time * 6.2832 / 4.3) + 0.006 * sin(time * 6.2832 / 7.1 + seed));

    // Warm-up: patches light at different moments.
    float n = noise(uv * vec2(3.0, 2.0) + seed);
    float lit = smoothstep(n - 0.35, n + 0.05, bright * 1.4);
    col = mix(ground.rgb * 0.5, col, lit);

    // Scanlines: the last px of every period dark, the one before it a little.
    float phase = mod(px.y, period);
    col *= 1.0 - scanDepth * step(period - 1.0, phase) - scanDepth * 0.33 * step(period - 2.0, phase) * step(phase, period - 1.0);

    // Aperture grille: every other column, the same in every glyph cell.
    col *= 1.0 - mask * step(1.0, mod(px.x, 2.0));

    // The curved glass: rounded corners and a soft shadow inside the edge, plus a faint reflection across the
    // upper left.
    vec2 q = abs(px - size * 0.5) - (size * 0.5 - vec2(corner));
    float edge = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - corner;      // < 0 inside, px
    float inside = 1.0 - smoothstep(-1.0, 0.5, edge);
    float rim = smoothstep(-min(vigBand.x, vigBand.y), 0.0, edge);
    col *= mix(1.0, 0.6, rim);
    float sheen = smoothstep(0.55, 0.0, length((uv - vec2(0.18, 0.12)) * vec2(1.0, 1.8)));
    col += vec3(1.0, 0.96, 0.92) * glass * 0.025 * sheen;

    // The line itself while the tube opens or collapses.
    float lineGlow = (1.0 - power) * exp(-abs(dy) * size.y / 2.0) * (1.0 - smoothstep(0.35, 0.5, abs(uv.x - 0.5)));
    col = mix(ground.rgb * 0.55, col, inBand) + phosphor.rgb * lineGlow * 1.4;

    // Outside the rounded corners: the bezel.
    col = mix(ground.rgb * 0.3, col, inside);

    fragColor = vec4(col, 1.0) * fade * qt_Opacity;
}
