#!/usr/bin/env python3
"""Generate the rat bar-widget font (modules/terminus-bar-plugins/rat/fonts/ratwalk2.otf).

Glyph "a" is the sleeping rat, copied from the previous font (--idle-from);
"b".."f" are a 5-frame gallop drawn here: a low, stretched body with short
stubby legs, a bobbing body and a whipping tail.

Run: uv run --with fonttools --with skia-pathops tools/make_ratwalk.py [--idle-from OLD.otf] [OUT.otf]
"""
import argparse
import math
from pathlib import Path

import pathops
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont

PLUGIN = Path(__file__).resolve().parent.parent / "modules/terminus-bar-plugins/rat/fonts"


def poly(points):
    p = pathops.Path()
    p.moveTo(*points[0])
    for pt in points[1:]:
        p.lineTo(*pt)
    p.close()
    return p


def ellipse(cx, cy, rx, ry, rot=0.0, n=72):
    c, s = math.cos(rot), math.sin(rot)
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        x, y = rx * math.cos(t), ry * math.sin(t)
        pts.append((cx + x * c - y * s, cy + x * s + y * c))
    return poly(pts)


def capsule(a, b, ra, rb):
    """Tapered limb from a (radius ra) to b (radius rb)."""
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    nx, ny = -math.sin(ang), math.cos(ang)
    quad = poly([
        (a[0] + nx * ra, a[1] + ny * ra), (b[0] + nx * rb, b[1] + ny * rb),
        (b[0] - nx * rb, b[1] - ny * rb), (a[0] - nx * ra, a[1] - ny * ra),
    ])
    return union([quad, ellipse(*a, ra, ra, n=32), ellipse(*b, rb, rb, n=32)])


def bez(p0, p1, p2, p3, t):
    u = 1 - t
    return tuple(u**3 * p0[i] + 3 * u * u * t * p1[i] + 3 * u * t * t * p2[i] + t**3 * p3[i] for i in range(2))


def tail(p0, p1, p2, p3, w0, w1, n=24):
    pts = [bez(p0, p1, p2, p3, i / n) for i in range(n + 1)]
    left, right = [], []
    for i, pt in enumerate(pts):
        a, b = pts[max(i - 1, 0)], pts[min(i + 1, n)]
        ang = math.atan2(b[1] - a[1], b[0] - a[0])
        w = w0 + (w1 - w0) * i / n
        left.append((pt[0] - math.sin(ang) * w, pt[1] + math.cos(ang) * w))
        right.append((pt[0] + math.sin(ang) * w, pt[1] - math.cos(ang) * w))
    return poly(left + right[::-1])


def union(paths):
    out = pathops.Path()
    for p in paths:
        out = pathops.op(out, p, pathops.PathOp.UNION, fix_winding=True)
    return out


def head_and_body(dy, pitch):
    """Body, head, snout, ear; returns (solid, holes)."""
    body = ellipse(430, -10 + dy, 250, 100, rot=pitch)
    hx, hy = 700, 0 + dy + pitch * 90
    head = ellipse(hx, hy, 95, 78)
    snout = poly([(hx + 30, hy + 62), (hx + 190, hy - 22), (hx + 30, hy - 72)])
    nose = ellipse(hx + 188, hy - 22, 13, 13, n=24)
    ear = ellipse(hx - 40, hy + 66, 52, 52, n=40)
    solid = union([body, head, snout, nose, ear])
    holes = [ellipse(hx - 40, hy + 66, 28, 28, n=32), ellipse(hx + 28, hy + 12, 15, 15, n=24)]
    return solid, holes, (hx, hy)


# frame: body dy, pitch, tail rise, hind/front leg swing (x offset from the hip), foot lift.
# Legs are deliberately stubby: hip-to-foot never exceeds ~70 units.
FRAMES = {
    "b": dict(dy=-3, pitch=0.00, tail=10, hind=-48, front=48, lift=0),
    "c": dict(dy=4, pitch=0.02, tail=35, hind=-18, front=22, lift=0),
    "d": dict(dy=14, pitch=0.04, tail=55, hind=24, front=-24, lift=22),
    "e": dict(dy=10, pitch=-0.02, tail=30, hind=36, front=-38, lift=20),
    "f": dict(dy=1, pitch=-0.01, tail=-5, hind=-34, front=34, lift=4),
}
GROUND = -158
HIPS = [(330, -88), (390, -92), (610, -88), (665, -92)]


def draw_frame(f):
    solid, holes, _ = head_and_body(f["dy"], f["pitch"])
    parts = [solid]
    dy, rise = f["dy"], f["tail"]
    parts.append(tail((195, 0 + dy), (120, 0 + dy + rise * 0.3), (60, 10 + rise), (-10, 20 + rise * 1.4), 18, 5))
    feet = []
    for i, (hx, hy) in enumerate(HIPS):
        swing = f["hind"] if i < 2 else f["front"]
        swing += 14 if i % 2 else 0
        lift = f["lift"] * (0.6 if i % 2 else 1.0)
        hip = (hx, hy + dy)
        foot = (hx + swing, GROUND + lift)
        feet.append(foot)
        parts.append(capsule(hip, foot, 44, 34))
    # paws: small forward-pointing ovals
    for foot in feet:
        parts.append(ellipse(foot[0] + 12, foot[1] - 2, 40, 22))
    out = union(parts)
    for h in holes:
        out = pathops.op(out, h, pathops.PathOp.DIFFERENCE, fix_winding=True)
    return out


def to_charstring(path, dx, adv):
    pen = T2CharStringPen(adv, None)
    path.draw(_Shift(pen, dx))
    return pen.getCharString()


class _Shift:
    def __init__(self, pen, dx):
        self.pen, self.dx = pen, dx

    def _s(self, pts):
        return [(x + self.dx, y) for x, y in pts]

    def moveTo(self, p): self.pen.moveTo(*self._s([p]))
    def lineTo(self, p): self.pen.lineTo(*self._s([p]))
    def curveTo(self, *p): self.pen.curveTo(*self._s(p))
    def qCurveTo(self, *p): self.pen.qCurveTo(*self._s(p))
    def closePath(self): self.pen.closePath()
    def endPath(self): self.pen.endPath()


def bounds(path):
    bp = BoundsPen(None)
    path.draw(bp)
    return bp.bounds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default=str(PLUGIN / "ratwalk2.otf"))
    ap.add_argument("--idle-from", help="font to copy the sleeping glyph from (default: the output file itself)")
    a = ap.parse_args()

    glyphs = {n: draw_frame(f) for n, f in FRAMES.items()}

    old = TTFont(a.idle_from or a.out)
    rec = RecordingPen()
    old.getGlyphSet()["a"].draw(rec)
    ob = BoundsPen(old.getGlyphSet())
    old.getGlyphSet()["a"].draw(ob)

    run_b = [bounds(p) for p in glyphs.values()]
    left = min(b[0] for b in run_b + [ob.bounds])
    right = max(b[2] for b in run_b + [ob.bounds])
    shift = 30 - left
    adv = int(right + shift + 30)

    cs = {".notdef": T2CharStringPen(adv, None).getCharString(), "space": T2CharStringPen(470, None).getCharString()}
    idle_pen = T2CharStringPen(adv, None)
    rec.replay(_Shift(idle_pen, shift))
    cs["a"] = idle_pen.getCharString()
    for n, p in glyphs.items():
        cs[n] = to_charstring(p, shift, adv)

    order = [".notdef", "space", "a", "b", "c", "d", "e", "f"]
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({32: "space", **{ord(c): c for c in "abcdef"}})
    fb.setupCFF("TerminusRatwalk2-Regular", {"FullName": "Terminus Ratwalk 2"}, cs, {})
    fb.setupHorizontalMetrics({g: (470 if g == "space" else adv, 0) for g in order})
    fb.setupHorizontalHeader(ascent=500, descent=-500)
    fb.setupNameTable({"familyName": "Terminus Ratwalk 2", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=500, sTypoDescender=-500, usWinAscent=500, usWinDescent=500)
    fb.setupPost()
    fb.save(a.out)
    print("wrote", a.out, "advance", adv)


if __name__ == "__main__":
    main()
