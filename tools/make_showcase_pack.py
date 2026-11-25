"""Generate the TesseraShowcase resource pack: a Bedrock pack with Java-format Tessera assets,
every picture drawn here (no vanilla or third-party files).

    python -I tools/make_showcase_pack.py

Shows: seamless glass (OptiFine CTM ctm_compact, blocks and panes), a 3D crafting table and
a 3D lantern (block, in slots and in the hand), enchanted books by enchantment (OptiFine CIT).
"""
import json, os, random, sys, uuid
from PIL import Image, ImageDraw

# TESSERA_SHOWCASE_ROOT: write into another pack (make_info_pack.py merges this one into
# TesseraRedstoneInfo); then the enchanted books and the manifest are left to that pack.
MERGED = os.environ.get("TESSERA_SHOWCASE_ROOT")
ROOT = MERGED or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "packs", "TesseraShowcase")
MC = os.path.join(ROOT, "assets", "minecraft")


def path(*parts):
    p = os.path.join(MC, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def save_json(obj, *parts):
    with open(path(*parts), "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, ensure_ascii=False)
        f.write("\n")


def img(w=16, h=16, fill=(0, 0, 0, 0)):
    return Image.new("RGBA", (w, h), fill)


def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c[:3]) + (c[3] if len(c) > 3 else 255,)


rnd = random.Random(7)

# ---------------------------------------------------------------------------
# Glass: ctm_compact tiles. 0 border all round, 1 none, 2 left+right, 3 top+bottom, 4 corners.
# ---------------------------------------------------------------------------
FRAME = (236, 250, 255, 255)
FRAME_IN = (110, 196, 236, 255)
FILL = (150, 215, 250, 46)
GLINT = (255, 255, 255, 170)


def glass_tile(top, bottom, left, right, corners=False):
    t = img(fill=FILL)
    px = t.load()
    # glints: two short diagonal streaks
    for i in range(4):
        px[4 + i, 9 - i] = GLINT
        px[5 + i, 9 - i] = (255, 255, 255, 90)
    for i in range(2):
        px[9 + i, 12 - i] = GLINT

    def edge(cells):
        for (x, y, inner) in cells:
            px[x, y] = FRAME_IN if inner else FRAME

    # The inner line runs to the tile's edge where no border crosses it, so it continues into
    # the next block of a connected wall.
    x0, x1 = (1 if left else 0), (14 if right else 15)
    y0, y1 = (1 if top else 0), (14 if bottom else 15)
    if top:
        edge([(x, 0, False) for x in range(16)] + [(x, 1, True) for x in range(x0, x1 + 1)])
    if bottom:
        edge([(x, 15, False) for x in range(16)] + [(x, 14, True) for x in range(x0, x1 + 1)])
    if left:
        edge([(0, y, False) for y in range(16)] + [(1, y, True) for y in range(y0, y1 + 1)])
    if right:
        edge([(15, y, False) for y in range(16)] + [(14, y, True) for y in range(y0, y1 + 1)])
    if corners:
        for (x, y) in [(0, 0), (15, 0), (0, 15), (15, 15)]:
            px[x, y] = FRAME
    return t


tiles = [
    glass_tile(True, True, True, True),
    glass_tile(False, False, False, False),
    glass_tile(False, False, True, True),
    glass_tile(True, True, False, False),
    glass_tile(False, False, False, False, corners=True),
]
for i, t in enumerate(tiles):
    t.save(path("optifine", "ctm", "showcase_glass", f"{i}.png"))
tiles[0].save(path("textures", "block", "glass.png"))
with open(path("optifine", "ctm", "showcase_glass", "glass.properties"), "w", encoding="utf-8") as f:
    f.write("# Seamless glass: blocks and panes that touch share one outer frame.\n"
            "matchTiles=glass\nmethod=ctm_compact\ntiles=0-4\nconnect=block\n")
# Pane edge strip (the thin top / bottom / end faces).
pt = img(fill=(0, 0, 0, 0))
d = ImageDraw.Draw(pt)
d.rectangle([7, 0, 8, 15], fill=FRAME)
pt.putpixel((7, 0), FRAME_IN)
pt.save(path("textures", "block", "glass_pane_top.png"))

# ---------------------------------------------------------------------------
# Crafting table: wood, dark wood, iron, a work grid, a hammer and a saw on top.
# ---------------------------------------------------------------------------
OAK = [(196, 148, 92), (178, 130, 78), (160, 114, 66), (206, 162, 104)]
DARK = [(104, 70, 42), (92, 60, 36), (118, 82, 50)]
IRON = [(196, 200, 206), (160, 166, 174), (222, 226, 230)]


def planks(colors, grain=True, seams=(5, 11)):
    t = img()
    px = t.load()
    for y in range(16):
        row = colors[(y // 2) % len(colors)]
        for x in range(16):
            c = row
            if grain and rnd.random() < 0.18:
                c = shade(c, 0.9)
            px[x, y] = c
    for y in seams:
        for x in range(16):
            px[x, y] = shade(colors[0], 0.62)
    return t


top = planks(OAK, seams=())
d = ImageDraw.Draw(top)
d.rectangle([0, 0, 15, 15], outline=shade(OAK[0], 0.55))
for g in (5, 10):  # 3x3 work grid
    d.line([(g, 2), (g, 13)], fill=shade(OAK[0], 0.5))
    d.line([(2, g), (13, g)], fill=shade(OAK[0], 0.5))
top.save(path("textures", "block", "showcase_table_top.png"))
side = planks(OAK)
ImageDraw.Draw(side).rectangle([0, 0, 15, 15], outline=shade(OAK[0], 0.55))
side.save(path("textures", "block", "showcase_table_side.png"))
planks(DARK, seams=()).save(path("textures", "block", "showcase_table_leg.png"))
iron = img()
for y in range(16):
    for x in range(16):
        iron.putpixel((x, y), IRON[(x + y) % 3] if (x + y) % 5 else IRON[1])
iron.save(path("textures", "block", "showcase_iron.png"))


def box(frm, to, tex, faces=("north", "south", "east", "west", "up", "down"), rotation=None, uv=None):
    # uv: one explicit rectangle for every face. Needed for parts above the block (y > 16):
    # their automatic UVs would fall outside the texture.
    f = {}
    for side_ in faces:
        f[side_] = {"texture": tex} if uv is None else {"uv": uv, "texture": tex}
    e = {"from": frm, "to": to, "faces": f}
    if rotation:
        e["rotation"] = rotation
    return e


BLOCK_DISPLAY = {
    "gui": {"rotation": [30, 225, 0], "translation": [0, 0, 0], "scale": [0.625, 0.625, 0.625]},
    "ground": {"rotation": [0, 0, 0], "translation": [0, 3, 0], "scale": [0.25, 0.25, 0.25]},
    "fixed": {"rotation": [0, 0, 0], "translation": [0, 0, 0], "scale": [0.5, 0.5, 0.5]},
    "thirdperson_righthand": {"rotation": [75, 45, 0], "translation": [0, 2.5, 0], "scale": [0.375, 0.375, 0.375]},
    "firstperson_righthand": {"rotation": [0, 45, 0], "translation": [0, 0, 0], "scale": [0.40, 0.40, 0.40]},
    "firstperson_lefthand": {"rotation": [0, 225, 0], "translation": [0, 0, 0], "scale": [0.40, 0.40, 0.40]},
}

table = {
    "textures": {"top": "block/showcase_table_top", "side": "block/showcase_table_side", "leg": "block/showcase_table_leg",
                 "iron": "block/showcase_iron", "particle": "block/showcase_table_side"},
    "display": BLOCK_DISPLAY,
    "elements": [
        # table top
        {"from": [0, 12, 0], "to": [16, 16, 16], "faces": {
            "up": {"texture": "#top"}, "down": {"texture": "#leg"},
            "north": {"uv": [0, 0, 16, 4], "texture": "#side"}, "south": {"uv": [0, 0, 16, 4], "texture": "#side"},
            "east": {"uv": [0, 0, 16, 4], "texture": "#side"}, "west": {"uv": [0, 0, 16, 4], "texture": "#side"}}},
        # base board: the game treats the table as a full block and hides the ground's top face
        # under it, so the model covers the floor itself
        {"from": [0, 0, 0], "to": [16, 1, 16], "faces": {
            "up": {"texture": "#leg"}, "down": {"texture": "#leg", "cullface": "down"},
            "north": {"uv": [0, 15, 16, 16], "texture": "#leg"}, "south": {"uv": [0, 15, 16, 16], "texture": "#leg"},
            "east": {"uv": [0, 15, 16, 16], "texture": "#leg"}, "west": {"uv": [0, 15, 16, 16], "texture": "#leg"}}},
        # legs
        box([1, 1, 1], [4, 12, 4], "#leg", faces=("north", "south", "east", "west")),
        box([12, 1, 1], [15, 12, 4], "#leg", faces=("north", "south", "east", "west")),
        box([1, 1, 12], [4, 12, 15], "#leg", faces=("north", "south", "east", "west")),
        box([12, 1, 12], [15, 12, 15], "#leg", faces=("north", "south", "east", "west")),
        # lower shelf
        {"from": [2, 3, 2], "to": [14, 5, 14], "faces": {
            "up": {"texture": "#side"}, "down": {"texture": "#leg"},
            "north": {"uv": [2, 0, 14, 2], "texture": "#side"}, "south": {"uv": [2, 0, 14, 2], "texture": "#side"},
            "east": {"uv": [2, 0, 14, 2], "texture": "#side"}, "west": {"uv": [2, 0, 14, 2], "texture": "#side"}}},
        # hammer: handle and head
        box([10, 16, 3], [11, 17, 10], "#leg", faces=("north", "south", "east", "west", "up"), uv=[2, 2, 3, 9]),
        box([9, 16, 2], [12, 18, 4], "#iron", faces=("north", "south", "east", "west", "up"), uv=[4, 4, 7, 6]),
        # saw blade and grip
        box([3, 16, 8], [8, 16.5, 14], "#iron", faces=("north", "south", "east", "west", "up"), uv=[2, 2, 7, 8]),
        box([3, 16, 13], [8, 17.5, 15], "#leg", faces=("north", "south", "east", "west", "up"), uv=[6, 6, 11, 8]),
    ],
}
save_json(table, "models", "block", "crafting_table.json")
save_json({"variants": {"": {"model": "minecraft:block/crafting_table"}}}, "blockstates", "crafting_table.json")
save_json({"parent": "minecraft:block/crafting_table"}, "models", "item", "crafting_table.json")
save_json({"model": {"type": "minecraft:model", "model": "minecraft:block/crafting_table"}}, "items", "crafting_table.json")

# ---------------------------------------------------------------------------
# Lantern: iron cage, warm glass, a handle. Standing and hanging.
# ---------------------------------------------------------------------------
lt = img()
d = ImageDraw.Draw(lt)
CAGE = (52, 56, 66, 255)
CAGE_HI = (98, 104, 118, 255)
GLOW = [(255, 214, 120, 255), (255, 182, 72, 255), (255, 236, 170, 255)]
# body face (0..6 x 0..7 region used by the model), cap (0..4 x 8..10), handle strip (8..11)
for y in range(7):
    for x in range(6):
        lt.putpixel((x, y), GLOW[(x * 3 + y) % 3])
d.rectangle([0, 0, 5, 6], outline=CAGE)
d.line([(0, 3), (5, 3)], fill=CAGE_HI)
d.rectangle([0, 8, 3, 10], fill=CAGE)
d.line([(0, 8), (3, 8)], fill=CAGE_HI)
d.rectangle([8, 0, 9, 5], fill=CAGE)
d.point([(8, 0), (9, 0)], fill=CAGE_HI)
lt.save(path("textures", "block", "showcase_lantern.png"))


def lantern(dy):
    def face(u0, v0, u1, v1):
        return {"uv": [u0, v0, u1, v1], "texture": "#all"}
    body = {"from": [5, 0 + dy, 5], "to": [11, 7 + dy, 11], "faces": {
        k: face(0, 0, 6, 7) for k in ("north", "south", "east", "west")} | {"up": face(0, 0, 6, 6), "down": face(0, 0, 6, 6)}}
    cap = {"from": [6, 7 + dy, 6], "to": [10, 9 + dy, 10], "faces": {
        k: face(0, 8, 4, 10) for k in ("north", "south", "east", "west")} | {"up": face(0, 8, 4, 10 + 2)}}
    handle1 = {"from": [6.5, 9 + dy, 8], "to": [9.5, 11 + dy, 8], "rotation": {"angle": 45, "axis": "y", "origin": [8, 8, 8]},
               "faces": {"north": face(8, 0, 10, 2), "south": face(8, 0, 10, 2)}}
    handle2 = {"from": [8, 9 + dy, 6.5], "to": [8, 11 + dy, 9.5], "rotation": {"angle": 45, "axis": "y", "origin": [8, 8, 8]},
               "faces": {"east": face(8, 0, 10, 2), "west": face(8, 0, 10, 2)}}
    return {"textures": {"all": "block/showcase_lantern", "particle": "block/showcase_lantern"}, "display": BLOCK_DISPLAY,
            "elements": [body, cap, handle1, handle2]}


save_json(lantern(0), "models", "block", "lantern.json")
save_json(lantern(5), "models", "block", "lantern_hanging.json")
save_json({"variants": {"hanging=false": {"model": "minecraft:block/lantern"},
                        "hanging=true": {"model": "minecraft:block/lantern_hanging"}}}, "blockstates", "lantern.json")
held = {"parent": "minecraft:block/lantern", "display": {
    "gui": {"rotation": [30, 225, 0], "translation": [0, 3, 0], "scale": [0.9, 0.9, 0.9]},
    "ground": {"rotation": [0, 0, 0], "translation": [0, 2, 0], "scale": [0.5, 0.5, 0.5]},
    "fixed": {"rotation": [0, 0, 0], "translation": [0, 2, 0], "scale": [1, 1, 1]},
    "thirdperson_righthand": {"rotation": [0, 0, 0], "translation": [0, 1.5, 2.5], "scale": [0.9, 0.9, 0.9]},
    "firstperson_righthand": {"rotation": [0, 45, 0], "translation": [0, 3, 0], "scale": [0.8, 0.8, 0.8]},
    "firstperson_lefthand": {"rotation": [0, 225, 0], "translation": [0, 3, 0], "scale": [0.8, 0.8, 0.8]}}}
save_json(held, "models", "item", "lantern.json")
save_json({"model": {"type": "minecraft:model", "model": "minecraft:item/lantern"}}, "items", "lantern.json")

if MERGED:
    print("showcase merged into", os.path.abspath(ROOT))
    sys.exit(0)

# ---------------------------------------------------------------------------
# Enchanted books by enchantment (OptiFine CIT).
# ---------------------------------------------------------------------------
SYMBOLS = {
    "sword": ["....#", "...#.", "#.#..", ".#...", "#.#.."],
    "pick": [".###.", "#.#.#", "..#..", "..#..", "..#.."],
    "shield": ["#####", "#...#", "#...#", ".#.#.", "..#.."],
    "heart": [".#.#.", "#####", "#####", ".###.", "..#.."],
}
BOOKS = [
    ("sharpness", (214, 48, 52), "sword"),
    ("efficiency", (232, 176, 36), "pick"),
    ("protection", (52, 112, 220), "shield"),
    ("mending", (48, 176, 88), "heart"),
]
for ench, color, sym in BOOKS:
    b = img()
    d = ImageDraw.Draw(b)
    cover = color + (255,)
    # pages (right edge) and cover with spine
    d.rectangle([3, 2, 13, 14], fill=(240, 234, 214, 255))
    d.rectangle([2, 1, 12, 13], fill=cover, outline=shade(cover, 0.55))
    d.line([(3, 2), (3, 12)], fill=shade(cover, 0.75))
    d.line([(4, 2), (11, 2)], fill=shade(cover, 1.25))
    for y, row in enumerate(SYMBOLS[sym]):
        for x, c in enumerate(row):
            if c == "#":
                b.putpixel((5 + x, 5 + y), (255, 248, 200, 255))
    # sparkle
    for (x, y) in [(13, 1), (14, 2), (12, 0)]:
        b.putpixel((x, y), (255, 255, 255, 230))
    b.save(path("optifine", "cit", "enchanted_books", f"{ench}.png"))
    with open(path("optifine", "cit", "enchanted_books", f"{ench}.properties"), "w", encoding="utf-8") as f:
        f.write(f"type=item\nitems=enchanted_book\nenchantmentIDs=minecraft:{ench}\ntexture={ench}\n")

# ---------------------------------------------------------------------------
# Bedrock shell: manifest (fixed UUIDs, so updates replace the pack) and icon.
# ---------------------------------------------------------------------------
manifest = {
    "format_version": 2,
    "header": {
        "name": "Tessera Showcase",
        "description": "Seamless glass, 3D crafting table and lantern, enchanted books by enchantment (needs the Tessera mod)",
        "uuid": "6f1f3b52-2a8e-4c61-9a0e-7d5b3c9e4a11",
        "version": [1, 0, 0],
        "min_engine_version": [1, 21, 0],
    },
    "modules": [{"type": "resources", "uuid": "9b7d6e2c-5f4a-4b3e-8c1d-2e6f7a8b9c02", "version": [1, 0, 0]}],
}
with open(os.path.join(ROOT, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
icon = img(64, 64, (30, 34, 44, 255))
g = tiles[0].resize((32, 32), Image.NEAREST)
icon.alpha_composite(g, (4, 4))
icon.alpha_composite(tiles[2].resize((32, 32), Image.NEAREST), (28, 4))
for i, (ench, _, _) in enumerate(BOOKS[:2]):
    bk = Image.open(path("optifine", "cit", "enchanted_books", f"{ench}.png")).resize((24, 24), Image.NEAREST)
    icon.alpha_composite(bk, (6 + i * 28, 36))
icon.save(os.path.join(ROOT, "pack_icon.png"))
print("written", os.path.abspath(ROOT))
