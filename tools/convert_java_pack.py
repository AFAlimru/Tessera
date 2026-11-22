"""Generic Java resource pack -> Bedrock test pack for Tessera (prototype of the native loader).

LOCAL TEST ONLY: most Java packs forbid redistribution of derivatives. The output
lands in local/ and must not be shared without the pack author's permission.

    python -I tools/convert_java_pack.py <pack.zip|dir> --vanilla <client.jar> --mapping <GeneratedChunkerMappings.inc>
                                         [--out <dir>] [--name <display name>] [--id <folder/atlas id>]

How it works, per block the pack touches:
  1. take every Java block state from the Chunker Java->Bedrock table,
  2. resolve the pack's blockstate file (over vanilla) to models + x/y rotation,
  3. render each of the six block faces flat ("bake"),
  4. emit a Tessera rule: Bedrock block + states (+ tessera: states where Bedrock has
     none) -> those six images, laid out by position ("*:<face>").
Bedrock cannot add geometry yet, so 3D details are flattened onto the block's own shape.
"""
import argparse, hashlib, io, json, math, os, re, sys, uuid, zipfile, zlib
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8")
ap = argparse.ArgumentParser()
ap.add_argument("pack")
ap.add_argument("--vanilla", required=True)
ap.add_argument("--mapping", required=True)
ap.add_argument("--out")
ap.add_argument("--name")
ap.add_argument("--id")
args = ap.parse_args()

pack_base = os.path.splitext(os.path.basename(args.pack.rstrip("/\\")))[0]
PACK_ID = args.id or re.sub(r"[^A-Za-z0-9]+", "_", pack_base).strip("_") or "JavaPack"
OUT = os.path.normpath(args.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "local", PACK_ID + "_BE"))
TEX_REL = "textures/blocks/jt"
TEX_DIR = os.path.join(OUT, *TEX_REL.split("/"))
os.makedirs(TEX_DIR, exist_ok=True)
for f in os.listdir(TEX_DIR):
    os.remove(os.path.join(TEX_DIR, f))

# Blocks Bedrock draws in a way flat face images cannot follow.
SKIP_BLOCKS = {"fire", "soul_fire", "piston_head", "moving_piston", "water", "lava", "tripwire"}
# Java properties Bedrock has no block state for -> Tessera computed states.
BRIDGE = {
    "note_block": {"note": "tessera:note", "instrument": "tessera:instrument", "powered": "tessera:powered"},
    "piston": {"extended": "tessera:extended"},
    "sticky_piston": {"extended": "tessera:extended"},
    "jukebox": {"has_record": "tessera:has_record"},
    "target": {"power": "tessera:signal"},
    "sculk_sensor": {"power": "tessera:signal"},
    "calibrated_sculk_sensor": {"power": "tessera:signal"},
    "redstone_wire": {d: "redstone_" + d for d in ("north", "east", "south", "west")},
}
# Java property -> Bedrock state that must carry the same value for a table row to be used.
AGREE = {"lever": {"powered": "open_bit"}}
FACES = ["down", "up", "north", "south", "west", "east"]


# ---------------------------------------------------------------- sources
class Source:
    def __init__(self, path):
        self.zip = zipfile.ZipFile(path) if os.path.isfile(path) else None
        self.dir = None if self.zip else path
        self._names = None

    def read(self, rel):
        if self.zip:
            try:
                return self.zip.read(rel)
            except KeyError:
                return None
        p = os.path.join(self.dir, *rel.split("/"))
        if not os.path.isfile(p):
            return None
        with open(p, "rb") as f:
            return f.read()

    def names(self):
        if self._names is None:
            if self.zip:
                self._names = set(self.zip.namelist())
            else:
                self._names = set()
                for root, _, files in os.walk(self.dir):
                    for f in files:
                        self._names.add(os.path.relpath(os.path.join(root, f), self.dir).replace("\\", "/"))
        return self._names


pack = Source(args.pack)
vanilla = Source(args.vanilla)


def read_any(rel):
    data = pack.read(rel)
    return data if data is not None else vanilla.read(rel)


def strip_ns(name):
    return name.split(":", 1)[1] if ":" in name else name


_tex = {}


def java_tex(name):
    name = strip_ns(name)
    if name not in _tex:
        data = read_any(f"assets/minecraft/textures/{name}.png")
        img = None
        if data is not None:
            img = Image.open(io.BytesIO(data)).convert("RGBA")
            # Only a texture with an animation .mcmeta is a strip of frames (use the first).
            # Any other non-square texture is used whole: uv 0..16 spans its full height.
            meta = read_any(f"assets/minecraft/textures/{name}.png.mcmeta")
            if img.height > img.width and meta is not None and b'"animation"' in meta:
                img = img.crop((0, 0, img.width, img.width))
        _tex[name] = img
    return _tex[name]


def load_json_from(src, rel):
    data = src.read(rel)
    if data is None:
        return None
    try:
        return json.loads(data.decode("utf-8-sig"))
    except ValueError:
        return None


def load_json(rel):
    j = load_json_from(pack, rel)
    return j if j is not None else load_json_from(vanilla, rel)


_models = {}


def load_model(name):
    name = strip_ns(name)
    if name not in _models:
        j = load_json(f"assets/minecraft/models/{name}.json")
        if j is None:
            raise KeyError("model " + name)
        base = {"textures": {}, "elements": None}
        parent = j.get("parent")
        if parent and not strip_ns(parent).startswith("builtin/"):
            base = load_model(parent)
        tex = dict(base["textures"])
        tex.update(j.get("textures", {}))
        _models[name] = {"textures": tex, "elements": j.get("elements", base["elements"])}
    return _models[name]


def resolve_ref(textures, ref):
    for _ in range(8):
        if isinstance(ref, dict):  # 26.x: {"sprite": "block/x", ...}
            ref = ref.get("sprite", "")
        if not ref.startswith("#"):
            return ref
        ref = textures.get(ref[1:], "")
    return None


# ---------------------------------------------------------------- baking (see convert_xekr.py for the derivation)
DIRS = {"down": (0, -1, 0), "up": (0, 1, 0), "north": (0, 0, -1), "south": (0, 0, 1), "west": (-1, 0, 0), "east": (1, 0, 0)}
DIR_OF = {v: k for k, v in DIRS.items()}


def face_coords(face, p):
    x, y, z = p
    return {"up": (x, z, y), "down": (x, 16 - z, -y), "north": (16 - x, 16 - y, -z),
            "south": (x, 16 - y, z), "west": (z, 16 - y, -x), "east": (16 - z, 16 - y, x)}[face]


def plane_points(face, f, t):
    (x0, y0, z0), (x1, y1, z1) = f, t
    if face in ("up", "down"):
        y = y1 if face == "up" else y0
        return [(x, y, z) for x in (x0, x1) for z in (z0, z1)]
    if face in ("north", "south"):
        z = z0 if face == "north" else z1
        return [(x, y, z) for x in (x0, x1) for y in (y0, y1)]
    x = x0 if face == "west" else x1
    return [(x, y, z) for y in (y0, y1) for z in (z0, z1)]


def ordered_corners(face, pts):
    ab = [(face_coords(face, p)[:2], p) for p in pts]
    a_lo, a_hi = min(a for (a, _), _ in ab), max(a for (a, _), _ in ab)
    b_lo, b_hi = min(b for (_, b), _ in ab), max(b for (_, b), _ in ab)
    if a_hi - a_lo < 1e-6 or b_hi - b_lo < 1e-6:
        return None, None

    def pick(a, b):
        return next(p for (pa, pb), p in ab if abs(pa - a) < 1e-6 and abs(pb - b) < 1e-6)
    return [pick(a_lo, b_lo), pick(a_hi, b_lo), pick(a_hi, b_hi), pick(a_lo, b_hi)], (a_lo, b_lo, a_hi, b_hi)


def rot_point(p, rx, ry):
    x, y, z = p
    for _ in range((rx // 90) % 4):
        y, z = z, 16 - y
    for _ in range((ry // 90) % 4):
        x, z = 16 - z, x
    return (x, y, z)


AXIS_FACES = {"x": ("east", "west"), "y": ("up", "down"), "z": ("north", "south")}


def turn_point(p, rot):
    """A model element's own rotation: `angle` degrees about `axis` through `origin`."""
    ox, oy, oz = rot.get("origin", [8, 8, 8])
    x, y, z = p[0] - ox, p[1] - oy, p[2] - oz
    t = math.radians(rot["angle"])
    c, sn = math.cos(t), math.sin(t)
    if rot["axis"] == "y":
        x, z = x * c + z * sn, -x * sn + z * c
    elif rot["axis"] == "x":
        y, z = y * c - z * sn, y * sn + z * c
    else:
        x, y = x * c - y * sn, x * sn + y * c
    return (x + ox, y + oy, z + oz)


def affine_from(dst, src):
    """Coefficients for PIL's AFFINE: output pixel -> input pixel, from three point pairs."""
    (x0, y0), (x1, y1), (x2, y2) = dst
    det = x0 * (y1 - y2) - y0 * (x1 - x2) + (x1 * y2 - x2 * y1)
    out = []
    for k in (0, 1):
        v0, v1, v2 = src[0][k], src[1][k], src[2][k]
        a = (v0 * (y1 - y2) - y0 * (v1 - v2) + (v1 * y2 - v2 * y1)) / det
        b = (x0 * (v1 - v2) - v0 * (x1 - x2) + (x1 * v2 - x2 * v1)) / det
        c = (x0 * (y1 * v2 - y2 * v1) - y0 * (x1 * v2 - x2 * v1) + v0 * (x1 * y2 - x2 * y1)) / det
        out += [a, b, c]
    return out


def rot_dir(d, rx, ry):
    x, y, z = d
    for _ in range((rx // 90) % 4):
        y, z = z, -y
    for _ in range((ry // 90) % 4):
        x, z = -z, x
    return (x, y, z)


def tint(img, rgb):
    out = img.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            if a:
                px[x, y] = (r * rgb[0] // 255, g * rgb[1] // 255, b * rgb[2] // 255, a)
    return out


def bake_scale(parts):
    """Pixels per model unit: enough for the textures and for sub-unit element edges, at most 4."""
    need = 1
    for mname, _, _ in parts:
        m = load_model(mname)
        for el in m["elements"] or []:
            for v in list(el["from"]) + list(el["to"]):
                for s in (1, 2, 4):
                    if abs(v * s - round(v * s)) < 1e-6:
                        need = max(need, s)
                        break
                else:
                    need = 4
            for fc in el.get("faces", {}).values():
                ref = resolve_ref(m["textures"], fc.get("texture", ""))
                t = java_tex(ref) if ref else None
                if t is not None:
                    need = max(need, min(4, max(t.width, t.height) // 16))
    return min(4, need)


AXIS_OF = {"up": 1, "down": 1, "north": 2, "south": 2, "west": 0, "east": 0}


def collect_layers(parts, face, tints):
    """Every model face that looks toward `face`: (depth, order, corners, uvs, texture, tint, plane).
    plane = where the face lies along the axis it looks down, 0..1 (hollow blocks have several)."""
    layers = []
    for mname, rx, ry in parts:
        m = load_model(mname)
        for el in m["elements"] or []:
            rot = el.get("rotation")
            turned = bool(rot and rot.get("angle", 0) != 0)
            for fdir, fc in el.get("faces", {}).items():
                if fdir not in DIRS or DIR_OF[rot_dir(DIRS[fdir], rx, ry)] != face:
                    continue
                # A turned part's faces along its axis stay flat (a curved rail's ties,
                # turned about y, seen from above); its other faces are tilted: left out.
                if turned and fdir not in AXIS_FACES[rot.get("axis", "y")]:
                    continue
                corners, rect = ordered_corners(fdir, plane_points(fdir, el["from"], el["to"]))
                if corners is None:
                    continue
                u0, v0, u1, v1 = fc.get("uv", rect)
                uvs = [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]
                r = (fc.get("rotation", 0) // 90) % 4
                if r:
                    uvs = uvs[-r:] + uvs[:-r]
                ref = resolve_ref(m["textures"], fc.get("texture", ""))
                tex = java_tex(ref) if ref else None
                if tex is None:
                    continue  # missing texture: leave that part to Bedrock
                rotated = [rot_point(turn_point(p, rot) if turned else p, rx, ry) for p in corners]
                world = [face_coords(face, p) for p in rotated]
                flat = [face_coords(face, rot_point(p, rx, ry)) for p in corners] if turned else None
                plane = max(0.0, min(1.0, rotated[0][AXIS_OF[face]] / 16))
                layers.append((world[0][2], len(layers), world, uvs, tex,
                               (tints or {}).get(fc.get("tintindex", -1)), plane, flat))
    return layers


def render(layers, scale):
    size = 16 * scale
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    for _, _, placed, uvs, tex, tint_rgb, _, flat in sorted(layers, key=lambda l: (l[0], l[1])):
        world = flat or placed  # a turned face is drawn straight first, then turned into place
        a0, a1 = min(w[0] for w in world), max(w[0] for w in world)
        b0, b1 = min(w[1] for w in world), max(w[1] for w in world)
        w, h = round((a1 - a0) * scale), round((b1 - b0) * scale)
        if w <= 0 or h <= 0:
            continue
        su, sv = tex.width / 16, tex.height / 16
        quad = []
        for a, b in ((a0, b0), (a0, b1), (a1, b1), (a1, b0)):  # PIL QUAD: UL, LL, LR, UR
            u, v = next(uv for (wa, wb, _), uv in zip(world, uvs) if abs(wa - a) < 1e-6 and abs(wb - b) < 1e-6)
            quad += [u * su, v * sv]
        part = tex.transform((w, h), Image.QUAD, quad, resample=Image.NEAREST)
        if tint_rgb:
            part = tint(part, tint_rgb)
        if flat:
            dst = [(p[0] * scale, p[1] * scale) for p in placed[:3]]
            src = [((p[0] - a0) * scale, (p[1] - b0) * scale) for p in flat[:3]]
            canvas.alpha_composite(part.transform((size, size), Image.AFFINE, affine_from(dst, src),
                                                  resample=Image.NEAREST))
            continue
        x, y = round(a0 * scale), round(b0 * scale)
        cx0, cy0 = max(0, -x), max(0, -y)
        cx1, cy1 = min(w, size - x), min(h, size - y)
        if cx1 > cx0 and cy1 > cy0:
            canvas.alpha_composite(part.crop((cx0, cy0, cx1, cy1)), (x + cx0, y + cy0))
    return canvas


def blockstate_parts(bs, state):
    def pick(v):
        v = v[0] if isinstance(v, list) else v
        return (v["model"], v.get("x", 0), v.get("y", 0))

    if "variants" in bs:
        for key, v in bs["variants"].items():
            pairs = [kv.split("=") for kv in key.split(",") if kv]
            if all(state.get(k) == val for k, val in pairs):
                return [pick(v)]
        return []

    def matches(when):
        if "OR" in when:
            return any(matches(w) for w in when["OR"])
        if "AND" in when:
            return all(matches(w) for w in when["AND"])
        # JSON booleans/numbers in "when" mean the same as their string form ("powered": false).
        def text(v):
            return ("true" if v else "false") if isinstance(v, bool) else str(v)
        return all(state.get(k) in text(v).split("|") for k, v in when.items())
    return [pick(p["apply"]) for p in bs.get("multipart", []) if "when" not in p or matches(p["when"])]


def blockstate_props(bs):
    """Java properties the blockstate file actually looks at."""
    props = set()
    for key in bs.get("variants", {}):
        props.update(kv.split("=")[0] for kv in key.split(",") if kv)

    def walk(when):
        for k, v in when.items():
            if k in ("OR", "AND"):
                for w in v:
                    walk(w)
            else:
                props.add(k)
    for p in bs.get("multipart", []):
        if "when" in p:
            walk(p["when"])
    return props


# ---------------------------------------------------------------- Java -> Bedrock block states (Chunker table from LHolo)
def load_mapping(path):
    src = open(path, encoding="utf-8").read()
    body = src[src.index("kChunkerMappingCompressed[] = {"):]
    raw = zlib.decompress(bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", body))).decode("utf-8")
    table = {}
    for line in raw.split("\n"):
        if not line or line.startswith("#"):
            continue
        ver, jname, jprops, bname, bstates = line.split("\t")
        slot = table.setdefault(strip_ns(jname), {})
        if jprops not in slot or int(ver) >= slot[jprops][0]:
            slot[jprops] = (int(ver), strip_ns(bname) if bname.startswith("minecraft:") else bname, bstates)
    return table


def typed(v):
    if v in ("true", "false"):
        return v == "true"
    if re.fullmatch(r"-?\d+", v):
        return int(v)
    return v


mapping = load_mapping(args.mapping)


# ---------------------------------------------------------------- which blocks does the pack touch?
pack_names = pack.names()
pack_blockstates = {n.split("/")[-1][:-5] for n in pack_names
                    if n.startswith("assets/minecraft/blockstates/") and n.endswith(".json")}
pack_models = {n[len("assets/minecraft/models/"):-5] for n in pack_names
               if n.startswith("assets/minecraft/models/") and n.endswith(".json")}
pack_textures = {n[len("assets/minecraft/textures/"):-4] for n in pack_names
                 if n.startswith("assets/minecraft/textures/") and n.endswith(".png")}


def model_touched(name, seen=None):
    """Does the pack override this model, one of its parents, or a texture it uses?"""
    name = strip_ns(name)
    if name in pack_models:
        return True
    try:
        m = load_model(name)
    except KeyError:
        return False
    return any(isinstance(t, str) and strip_ns(t) in pack_textures for t in m["textures"].values())


def blockstate_models(bs):
    out = []

    def add(v):
        for e in (v if isinstance(v, list) else [v]):
            out.append(e["model"])
    for v in bs.get("variants", {}).values():
        add(v)
    for p in bs.get("multipart", []):
        add(p["apply"])
    return out


touched = set(pack_blockstates)
for n in vanilla.names():
    if n.startswith("assets/minecraft/blockstates/") and n.endswith(".json"):
        block = n.split("/")[-1][:-5]
        if block in touched:
            continue
        bs = load_json_from(vanilla, n)
        if bs and any(model_touched(m) for m in blockstate_models(bs)):
            touched.add(block)


# ---------------------------------------------------------------- rules
def dust_rgb(power):
    f = power / 15.0
    r = f * 0.6 + (0.4 if f > 0 else 0.3)
    g = max(0.0, min(1.0, f * f * 0.7 - 0.5))
    b = max(0.0, min(1.0, f * f * 0.6 - 0.7))
    return int(r * 255), int(g * 255), int(b * 255)


texture_data = {}
_by_hash = {}
_face_cache = {}


def emit(img):
    digest = hashlib.sha1(img.tobytes() + bytes(img.size)).hexdigest()[:12]
    if digest not in _by_hash:
        name = "jt_" + digest
        img.save(os.path.join(TEX_DIR, name + ".png"))
        texture_data[name] = {"textures": f"{TEX_REL}/{name}"}
        _by_hash[digest] = name
    return _by_hash[digest]


def plane_groups(layers):
    """Group layers whose planes are within 0.03 of each other (overlays hug their face)."""
    groups = []
    for layer in sorted(layers, key=lambda l: l[6]):
        if groups and layer[6] - groups[-1][0] <= 0.03:
            groups[-1][1].append(layer)
        else:
            groups.append((layer[6], [layer]))
    return [((min(l[6] for l in ls) + max(l[6] for l in ls)) / 2, ls) for _, ls in groups]


def faces_for(parts, tints):
    key = (tuple(parts), tuple(sorted((tints or {}).items())))
    if key not in _face_cache:
        scale = bake_scale(parts)
        rep = {}
        for face in FACES:
            layers = collect_layers(parts, face, tints)
            if not layers:
                continue
            img = render(layers, scale)
            if img.getbbox() is None:  # nothing drawn on this face: leave Bedrock's own
                continue
            rep[f"*:{face}"] = emit(img)  # any plane: what you see from outside
            groups = plane_groups(layers)
            if len(groups) > 1:  # hollow block: inner faces get their own image
                for plane, ls in groups:
                    sub = render(ls, scale)
                    if sub.getbbox() is not None:
                        rep[f"*:{face}@{plane:.4f}"] = emit(sub)
        _face_cache[key] = rep
    return _face_cache[key]


rules, stats, problems = [], {}, []
for block in sorted(touched):
    if block in SKIP_BLOCKS:
        continue
    rows = mapping.get(block)
    bs = load_json(f"assets/minecraft/blockstates/{block}.json")
    if not rows or not bs:
        problems.append(f"{block}: no {'mapping' if not rows else 'blockstate'}")
        continue
    if len(rows) > 1:
        rows = {k: v for k, v in rows.items() if k}  # the property-less row is only a fallback
    used_props = blockstate_props(bs)
    bridge = {k: v for k, v in BRIDGE.get(block, {}).items() if k in used_props}
    if block.endswith("_leaves") and "distance" in used_props:
        bridge["distance"] = "tessera:distance"
    seen_when = set()
    count = 0
    for jprops, (_, bname, bstates) in sorted(rows.items()):
        state = dict(kv.split("=") for kv in jprops.split(",") if kv)
        when = {k: typed(v) for k, v in (kv.split("=") for kv in bstates.split(",") if kv) if k != "waterlogged"}
        for jkey, bkey in bridge.items():
            if jkey in state:
                when[bkey] = typed(state[jkey])
        # The table keeps a lever's handle pointing the same way by swapping on/off for
        # two of the four facings; a pack shows power, so take the row where they agree.
        if any(jk in state and bk in when and typed(state[jk]) != when[bk] for jk, bk in AGREE.get(block, {}).items()):
            continue
        wkey = (bname, json.dumps(when, sort_keys=True))
        if wkey in seen_when:
            continue  # several Java states share this Bedrock state: the first one stands for all
        try:
            parts = blockstate_parts(bs, state)
            if not parts:
                continue
            tints = {0: dust_rgb(int(state.get("power", 0)))} if block == "redstone_wire" else None
            if len(parts) == 1:  # one model: bake it unrotated, let Tessera turn it
                model, rx, ry = parts[0]
                rep = faces_for([(model, 0, 0)], tints)
            else:
                rx = ry = 0
                rep = faces_for(parts, tints)
        except KeyError as e:
            problems.append(f"{block} [{jprops}]: missing {e}")
            continue
        seen_when.add(wkey)
        if not rep:
            continue
        r = {"block": bname}
        if when:
            r["when"] = when
        if rx or ry:
            r["rotation"] = {"x": rx, "y": ry}
        r["replace"] = rep
        if block == "redstone_wire":
            r["tint"] = "none"
        rules.append(r)
        count += 1
    stats[block] = count

# ---------------------------------------------------------------- pack files
with open(os.path.join(OUT, "state_textures.json"), "w", encoding="utf-8") as f:
    json.dump({"format_version": 1, "rules": rules}, f, separators=(",", ":"), ensure_ascii=False)
with open(os.path.join(OUT, "textures", "terrain_texture.json"), "w", encoding="utf-8") as f:
    json.dump({"resource_pack_name": PACK_ID, "texture_name": "atlas.terrain", "padding": 8, "num_mip_levels": 4,
               "texture_data": texture_data}, f, indent=1)

man_path = os.path.join(OUT, "manifest.json")
if os.path.exists(man_path):
    with open(man_path, encoding="utf-8") as f:
        old = json.load(f)
    head_uuid, mod_uuid = old["header"]["uuid"], old["modules"][0]["uuid"]
else:
    head_uuid, mod_uuid = str(uuid.uuid4()), str(uuid.uuid4())
with open(man_path, "w", encoding="utf-8") as f:
    json.dump({
        "format_version": 2,
        "header": {"name": (args.name or pack_base) + " (Tessera 本机测试)",
                   "description": "由 Tessera 工具从 Java 版资源包转换，仅供本机测试，请勿分发",
                   "uuid": head_uuid, "version": [0, 1, 0], "min_engine_version": [1, 26, 60]},
        "modules": [{"type": "resources", "uuid": mod_uuid, "version": [0, 1, 0]}],
    }, f, indent=2, ensure_ascii=False)
icon = pack.read("pack.png")
if icon:
    Image.open(io.BytesIO(icon)).save(os.path.join(OUT, "pack_icon.png"))

print(f"wrote {OUT}")
print(f"  blocks touched by the pack: {len(touched)}, with rules: {sum(1 for c in stats.values() if c)}")
print(f"  rules: {len(rules)}, textures: {len(texture_data)}")
print("  rules per block:", ", ".join(f"{b}={c}" for b, c in sorted(stats.items(), key=lambda kv: -kv[1])[:25]))
for p in problems[:25]:
    print("  !", p)
if len(problems) > 25:
    print(f"  ! ... {len(problems) - 25} more")
