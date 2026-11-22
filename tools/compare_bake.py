"""Compare two baked packs (Python converter output vs. the C++ baker's):

    python -I tools/compare_bake.py <python out dir> <c++ out dir>

Rules are matched by block + states; for each face key the two images are compared
pixel by pixel (texture names differ: the two use different hashes)."""
import json, os, sys
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8")
a_dir, b_dir = sys.argv[1], sys.argv[2]


def rules(d):
    data = json.load(open(os.path.join(d, "state_textures.json"), encoding="utf-8"))
    out = {}
    for r in data["rules"]:
        key = (r["block"], json.dumps(r.get("when", {}), sort_keys=True), json.dumps(r.get("rotation", {}), sort_keys=True))
        out[key] = r
    return out


cache = {}


def image(d, name):
    k = (d, name)
    if k not in cache:
        cache[k] = Image.open(os.path.join(d, "textures", "blocks", "jt", name + ".png")).convert("RGBA")
    return cache[k]


ra, rb = rules(a_dir), rules(b_dir)
only_a = sorted(set(ra) - set(rb))
only_b = sorted(set(rb) - set(ra))
print(f"rules: python {len(ra)}, c++ {len(rb)}, only python {len(only_a)}, only c++ {len(only_b)}")
for k in only_a[:5]:
    print("  only python:", k)
for k in only_b[:5]:
    print("  only c++:", k)

same = diff = key_mismatch = 0
worst = []
for k in sorted(set(ra) & set(rb)):
    a, b = ra[k], rb[k]
    if set(a["replace"]) != set(b["replace"]) or a.get("tint") != b.get("tint"):
        key_mismatch += 1
        if key_mismatch <= 5:
            print("  keys differ:", k[0], k[1], sorted(set(a["replace"]) ^ set(b["replace"])))
        continue
    for face in a["replace"]:
        ia, ib = image(a_dir, a["replace"][face]), image(b_dir, b["replace"][face])
        if ia.size != ib.size:
            diff += 1
            worst.append((10**9, k[0], k[1], face, f"size {ia.size} vs {ib.size}"))
            continue
        pa, pb = ia.tobytes(), ib.tobytes()
        if pa == pb:
            same += 1
            continue
        diff += 1
        n = sum(1 for i in range(0, len(pa), 4) if pa[i:i + 4] != pb[i:i + 4])
        worst.append((n, k[0], k[1], face, f"{n} px differ of {len(pa) // 4}"))
print(f"faces: identical {same}, different {diff}, rules with different keys {key_mismatch}")
for w in sorted(worst, reverse=True)[:12]:
    print("  ", w[1], w[2][:80], w[3], w[4])
