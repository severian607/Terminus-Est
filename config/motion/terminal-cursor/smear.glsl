// label: Smear
// emoji: 🖌️
// category: Cursor · Terminal
// slot: terminal-cursor
// tags: smear, trail, smooth, default
// description: A smooth hexagonal smear stretches between the old and new cursor spots.
// source: https://github.com/thamarnan/ghostty-shaders/blob/main/shaders/cursor_smear.glsl
// license: none stated upstream (see CREDITS.md)
// demo: typing

// Smooth cursor smear for Ghostty.
// Adapted for Terminus-Est from:
// https://github.com/thamarnan/ghostty-shaders/blob/main/shaders/cursor_smear.glsl

void processEdge(vec2 p, vec2 a, vec2 b, inout float minDist, inout float inside) {
    vec2 edge = b - a;
    vec2 pa = p - a;
    float lenSq = dot(edge, edge);

    if (lenSq < 0.000001) {
        minDist = min(minDist, dot(pa, pa));
        return;
    }

    float t = clamp(dot(pa, edge) / lenSq, 0.0, 1.0);
    vec2 diff = pa - edge * t;
    minDist = min(minDist, dot(diff, diff));

    float crossProduct = edge.x * pa.y - edge.y * pa.x;
    inside = min(inside, step(0.0, crossProduct));
}

float sdHexagon(
    vec2 p,
    vec2 v0,
    vec2 v1,
    vec2 v2,
    vec2 v3,
    vec2 v4,
    vec2 v5
) {
    float minDist = 1e20;
    float inside = 1.0;

    processEdge(p, v0, v1, minDist, inside);
    processEdge(p, v1, v2, minDist, inside);
    processEdge(p, v2, v3, minDist, inside);
    processEdge(p, v3, v4, minDist, inside);
    processEdge(p, v4, v5, minDist, inside);
    processEdge(p, v5, v0, minDist, inside);

    float dist = sqrt(max(minDist, 0.0));
    return mix(dist, -dist, inside);
}

float sdRectangle(vec2 p, vec2 center, vec2 halfSize) {
    vec2 d = abs(p - center) - halfSize;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

struct Quad {
    vec2 topLeft;
    vec2 topRight;
    vec2 bottomLeft;
    vec2 bottomRight;
};

Quad getQuad(vec2 pos, vec2 size) {
    Quad q;
    q.topLeft = pos;
    q.topRight = pos + vec2(size.x, 0.0);
    q.bottomLeft = pos - vec2(0.0, size.y);
    q.bottomRight = pos + vec2(size.x, -size.y);
    return q;
}

void selectTrailCorners(Quad q, vec2 selector, out vec2 p1, out vec2 p2, out vec2 p3) {
    p1 = mix(
        mix(q.topRight, q.topLeft, selector.x),
        mix(q.bottomRight, q.bottomLeft, selector.x),
        selector.y
    );
    p2 = mix(
        mix(q.topLeft, q.bottomLeft, selector.x),
        mix(q.topRight, q.bottomRight, selector.x),
        selector.y
    );
    p3 = mix(
        mix(q.bottomRight, q.topRight, selector.x),
        mix(q.bottomLeft, q.topLeft, selector.x),
        selector.y
    );
}

void selectCorners(
    Quad q,
    vec2 selector,
    out vec2 p1,
    out vec2 p2,
    out vec2 p3,
    out vec2 p4
) {
    selectTrailCorners(q, selector, p1, p2, p3);
    p4 = mix(
        mix(q.bottomLeft, q.bottomRight, selector.x),
        mix(q.topLeft, q.topRight, selector.x),
        selector.y
    );
}

float easeOutCubic(float x) {
    float t = 1.0 - x;
    return 1.0 - t * t * t;
}

const float DURATION = 0.18;

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec4 background = texture(iChannel0, uv);
    fragColor = background;

    float progress = clamp((iTime - iTimeCursorChange) / DURATION, 0.0, 1.0);
    vec2 movement = iCurrentCursor.xy - iPreviousCursor.xy;
    if (progress >= 1.0 || dot(movement, movement) < 0.25 || iCursorVisible.x < 0.5) {
        return;
    }

    float invResY = 1.0 / iResolution.y;
    float scale = 2.0 * invResY;
    float aaWidth = scale;
    vec2 normOffset = iResolution.xy * invResY;

    vec2 currentPos = iCurrentCursor.xy * scale - normOffset;
    vec2 previousPos = iPreviousCursor.xy * scale - normOffset;
    vec2 currentSize = iCurrentCursor.zw * scale;
    vec2 previousSize = iPreviousCursor.zw * scale;

    vec2 deltaPos = currentPos - previousPos;
    Quad currentCursor = getQuad(currentPos, currentSize);
    Quad previousCursor = getQuad(previousPos, previousSize);
    vec2 selector = step(vec2(0.0), deltaPos);

    vec2 currP1;
    vec2 currP2;
    vec2 currP3;
    vec2 currP4;
    vec2 prevP1;
    vec2 prevP2;
    vec2 prevP3;
    selectCorners(currentCursor, selector, currP1, currP2, currP3, currP4);
    selectTrailCorners(previousCursor, selector, prevP1, prevP2, prevP3);

    float tailProgress = easeOutCubic(progress);
    float edgeProgress = easeOutCubic(min(progress * 2.0, 1.0));
    vec2 trailP1 = mix(prevP1, currP1, tailProgress);
    vec2 trailP2 = mix(prevP2, currP2, edgeProgress);
    vec2 trailP3 = mix(prevP3, currP3, edgeProgress);

    vec2 normCoord = fragCoord * scale - normOffset;
    float smearSdf = sdHexagon(
        normCoord,
        trailP1,
        trailP2,
        currP2,
        currP4,
        currP3,
        trailP3
    );
    float alpha = 1.0 - smoothstep(-aaWidth, aaWidth, smearSdf);

    vec2 halfCurrentSize = currentSize * 0.5;
    vec2 currentCenter = currentPos + vec2(halfCurrentSize.x, -halfCurrentSize.y);
    float cursorSdf = sdRectangle(normCoord, currentCenter, halfCurrentSize);

    vec3 trailColor = iCurrentCursorColor.rgb;
    fragColor.rgb = mix(fragColor.rgb, trailColor, alpha * iCurrentCursorColor.a);
    fragColor.rgb = mix(fragColor.rgb, background.rgb, step(cursorSdf, 0.0));
}
