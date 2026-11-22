"""Isometric preview of P5 geometry rules from a bake_test output folder.

usage: python -I tools/preview_geometry.py <bake out dir> <block> <out.png> [rule index] [--when key=value,...]

Draws the rule's quads as seen from above, south and east (painter's algorithm), each
textured with its sprite, so shape, UVs and rotations can be checked without the game.
"""
import json
import math
import os
import sys

from PIL import Image

SIZE = 320
SCALE = 150


def project(x, y, z):
    c, s = math.cos(math.radians(30)), math.sin(math.radians(30))
    sx = SIZE / 2 + (x - z) * c * SCALE
    sy = SIZE * 0.62 - y * SCALE + (x + z) * s * SCALE * 0.5 - 0.5 * SCALE * 0.5
    return sx, sy


DIRS = [(0, -1, 0), (0, 1, 0), (0, 0, -1), (0, 0, 1), (-1, 0, 0), (1, 0, 0)]
SHADE = [0.5, 1.0, 0.8, 0.8, 0.6, 0.6]


def normal(p):
    ax, ay, az = (p[1][i] - p[0][i] for i in range(3))
    bx, by, bz = (p[3][i] - p[0][i] for i in range(3))
    return (ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx)


def draw_tri(canvas, tex, scr, uvs, shade):
    (x0, y0), (x1, y1), (x2, y2) = scr
    det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(det) < 1e-9:
        return
    minx, maxx = int(max(0, min(x0, x1, x2))), int(min(SIZE - 1, max(x0, x1, x2) + 1))
    miny, maxy = int(max(0, min(y0, y1, y2))), int(min(SIZE - 1, max(y0, y1, y2) + 1))
    tw, th = tex.size
    tp = tex.load()
    cp = canvas.load()
    for py in range(miny, maxy + 1):
        for px in range(minx, maxx + 1):
            fx, fy = px + 0.5, py + 0.5
            l1 = ((fx - x0) * (y2 - y0) - (x2 - x0) * (fy - y0)) / det
            l2 = ((x1 - x0) * (fy - y0) - (fx - x0) * (y1 - y0)) / det
            l0 = 1 - l1 - l2
            if l0 < -1e-6 or l1 < -1e-6 or l2 < -1e-6:
                continue
            u = l0 * uvs[0][0] + l1 * uvs[1][0] + l2 * uvs[2][0]
            v = l0 * uvs[0][1] + l1 * uvs[1][1] + l2 * uvs[2][1]
            r, g, b, a = tp[min(tw - 1, max(0, int(u * tw))), min(th - 1, max(0, int(v * th)))]
            if a < 128:
                continue
            cp[px, py] = (int(r * shade), int(g * shade), int(b * shade), 255)


def main():
    args = sys.argv[1:]
    when = {}
    if "--when" in args:
        i = args.index("--when")
        for kv in args[i + 1].split(","):
            k, v = kv.split("=")
            when[k] = v
        del args[i : i + 2]
    out_dir, block, out = args[0], args[1], args[2]
    index = int(args[3]) if len(args) > 3 else 0
    rules = json.load(open(os.path.join(out_dir, "state_textures.json"), encoding="utf-8"))["rules"]
    rules = [r for r in rules if r["block"] == block and "geometry" in r]
    if when:
        rules = [r for r in rules if all(str(r.get("when", {}).get(k)).lower() == v.lower() for k, v in when.items())]
    if not rules:
        sys.exit("no geometry rule for " + block)
    rule = rules[min(index, len(rules) - 1)]
    print("rule:", json.dumps(rule.get("when", {})), len(rule["geometry"]), "quads")
    canvas = Image.new("RGBA", (SIZE, SIZE), (40, 44, 52, 255))
    quads = []
    for q in rule["geometry"]:
        p = [tuple(q["p"][i * 3 : i * 3 + 3]) for i in range(4)]
        n = normal(p)
        if n[0] + n[1] + n[2] <= 1e-9:  # faces away from the viewer (+x +y +z)
            continue
        depth = sum(c[0] + c[1] + c[2] for c in p) / 4
        quads.append((depth, q, p))
    quads.sort(key=lambda t: t[0])
    texs = {}
    for _, q, p in quads:
        name = q["t"]
        if name not in texs:
            texs[name] = Image.open(os.path.join(out_dir, "textures", "blocks", "jt", name + ".png")).convert("RGBA")
        scr = [project(*c) for c in p]
        uv = [tuple(q["uv"][i * 2 : i * 2 + 2]) for i in range(4)]
        shade = SHADE[q["d"]] if q.get("s", True) and 0 <= q["d"] < 6 else 1.0
        draw_tri(canvas, texs[name], [scr[0], scr[1], scr[2]], [uv[0], uv[1], uv[2]], shade)
        draw_tri(canvas, texs[name], [scr[0], scr[2], scr[3]], [uv[0], uv[2], uv[3]], shade)
    canvas.save(out)


main()
