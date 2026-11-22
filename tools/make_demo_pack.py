"""Generate the TesseraDemo resource pack (textures + JSON). Run: python -I tools/make_demo_pack.py"""
import json, os, sys, uuid
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "packs", "TesseraDemo")
TEX = os.path.join(ROOT, "textures", "blocks", "tessera")
os.makedirs(TEX, exist_ok=True)

FONT = {  # 3x5 digits
    "0": ["111", "101", "101", "101", "111"], "1": ["010", "110", "010", "010", "111"],
    "2": ["111", "001", "111", "100", "111"], "3": ["111", "001", "111", "001", "111"],
    "4": ["101", "101", "111", "001", "001"], "5": ["111", "100", "111", "001", "111"],
    "6": ["111", "100", "111", "101", "111"], "7": ["111", "001", "010", "010", "010"],
    "8": ["111", "101", "111", "101", "111"], "9": ["111", "101", "111", "001", "111"],
}

def digits(img, text, color, outline=(0, 0, 0, 255)):
    w = len(text) * 4 - 1
    x0, y0 = (16 - w) // 2, 6
    px = img.load()
    cells = []
    for i, ch in enumerate(text):
        for y, row in enumerate(FONT[ch]):
            for x, b in enumerate(row):
                if b == "1":
                    cells.append((x0 + i * 4 + x, y0 + y))
    for (x, y) in cells:  # outline first
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if 0 <= x + dx < 16 and 0 <= y + dy < 16:
                    px[x + dx, y + dy] = outline
    for (x, y) in cells:
        px[x, y] = color

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def power_color(p):
    return lerp((70, 0, 0), (255, 40, 40), p / 15) + (255,)

def climb_color(p):
    return lerp((60, 50, 0), (255, 220, 40), p / 15) + (255,)

def cross(color):
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    px = img.load()
    for i in range(16):
        for j in (7, 8):
            px[i, j] = color
            px[j, i] = color
    return img

def line(color):
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    px = img.load()
    for i in range(16):
        for j in (7, 8):
            px[i, j] = color
    return img

def plate(p):
    base = lerp((120, 95, 20), (250, 215, 60), p / 15) + (255,)
    img = Image.new("RGBA", (16, 16), base)
    px = img.load()
    for i in range(16):
        px[i, 0] = px[i, 15] = px[0, i] = px[15, i] = (90, 70, 10, 255)
    return img

texture_data = {}
def add(name, img):
    img.save(os.path.join(TEX, name + ".png"))
    texture_data[name] = {"textures": "textures/blocks/tessera/" + name}

for p in range(16):
    c = cross(power_color(p)); digits(c, str(p), (255, 255, 255, 255)); add(f"tessera_dust_cross_p{p}", c)
    l = line(power_color(p)); digits(l, str(p), (255, 255, 255, 255)); add(f"tessera_dust_line_p{p}", l)
    k = line(climb_color(p)); add(f"tessera_dust_climb_p{p}", k)  # wall segments: colour only, no digits
    g = plate(p); digits(g, str(p), (255, 255, 255, 255)); add(f"tessera_plate_p{p}", g)

def powered_side():
    """Stone body with a glowing red frame: 'this block is powered'."""
    img = Image.new("RGBA", (16, 16), (118, 118, 118, 255))
    px = img.load()
    for y in range(16):
        for x in range(16):
            if (x * 7 + y * 13) % 11 == 0:
                px[x, y] = (100, 100, 100, 255)
    for i in range(16):
        px[i, 0] = px[i, 15] = px[0, i] = px[15, i] = (255, 40, 40, 255)
        px[i, 1] = px[i, 14] = px[1, i] = px[14, i] = (170, 20, 20, 255)
    bolt = ["..##", ".##.", "####", ".##.", "##.."]  # small lightning mark
    for y, row in enumerate(bolt):
        for x, c in enumerate(row):
            if c == "#":
                px[6 + x, 5 + y] = (255, 220, 60, 255)
    return img

def powered_front():
    """Same frame with a dark opening and a red ring: fronts of droppers, the piston's inner face."""
    img = powered_side()
    px = img.load()
    for y in range(4, 12):
        for x in range(4, 12):
            ring = x in (4, 11) or y in (4, 11)
            px[x, y] = (255, 60, 30, 255) if ring else (25, 10, 10, 255)
    return img

add("tessera_on_side", powered_side())

def number_tile(n, base, fg=(255, 255, 255, 255)):
    img = Image.new("RGBA", (16, 16), base)
    px = img.load()
    for i in range(16):
        px[i, 0] = px[i, 15] = px[0, i] = px[15, i] = (40, 40, 40, 255)
    digits(img, str(n), fg)
    return img

# Block-entity states read by Tessera: comparator output, note block pitch.
for n in range(16):
    add(f"tessera_cmp_out_{n}", number_tile(n, (150, 150, 150, 255), (255, 60, 60, 255)))
for n in range(25):
    add(f"tessera_note_{n}", number_tile(n, (95, 60, 40, 255)))
    on = number_tile(n, (150, 40, 30, 255), (255, 230, 80, 255))  # powered: red tile, yellow digits
    for i in range(16):
        on.putpixel((i, 0), (255, 60, 40, 255)); on.putpixel((i, 15), (255, 60, 40, 255))
        on.putpixel((0, i), (255, 60, 40, 255)); on.putpixel((15, i), (255, 60, 40, 255))
    add(f"tessera_note_on_{n}", on)
add("tessera_on_front", powered_front())

with open(os.path.join(ROOT, "textures", "terrain_texture.json"), "w", encoding="utf-8") as f:
    json.dump({"resource_pack_name": "TesseraDemo", "texture_name": "atlas.terrain", "padding": 8,
               "num_mip_levels": 4, "texture_data": texture_data}, f, indent=2)

rules = {
    "format_version": 1,
    "rules": [
        {"block": "redstone_wire", "on": ["north", "south", "east", "west"],
         "replace": {"redstone_dust_line": "tessera_dust_climb_p{redstone_signal}",
                     "redstone_dust_cross": "tessera_dust_climb_p{redstone_signal}"},
         "tint": "none"},
        {"block": "redstone_wire",
         "replace": {"redstone_dust_cross": "tessera_dust_cross_p{redstone_signal}",
                     "redstone_dust_line": "tessera_dust_line_p{redstone_signal}"},
         "tint": "none"},
        # Powered mechanisms. Dropper/dispenser expose power as triggered_bit,
        # observer as powered_bit, hopper (locked) as toggle_bit. Bedrock pistons
        # have no power state, so Tessera computes tessera:extended (arm is out).
        {"block": "dropper", "when": {"triggered_bit": True},
         "replace": {"dropper_front_horizontal": "tessera_on_front", "dropper_front_vertical": "tessera_on_front",
                     "dropper_side": "tessera_on_side", "dropper_top": "tessera_on_side"}},
        {"block": "dispenser", "when": {"triggered_bit": True},
         "replace": {"dispenser_front_horizontal": "tessera_on_front", "dispenser_front_vertical": "tessera_on_front",
                     "dispenser_side": "tessera_on_side", "dispenser_top": "tessera_on_side"}},
        {"block": ["piston", "sticky_piston"], "when": {"tessera:extended": True},
         "replace": {"piston_side": "tessera_on_side", "piston_bottom": "tessera_on_side", "piston_top": "tessera_on_front"}},
        {"block": "observer", "when": {"powered_bit": True},
         "replace": {f"observer_{f}#{v}": "tessera_on_side" for f in ("top", "bottom", "east", "west", "north")
                     for v in (0, 1)}},
        {"block": "hopper", "when": {"toggle_bit": True},
         "replace": {"hopper_outside": "tessera_on_side", "hopper_top": "tessera_on_side"}},
        # Block-entity states (tessera:output, tessera:note, tessera:has_record) come from Tessera itself.
        {"block": ["powered_comparator", "unpowered_comparator"],
         "replace": {"comparator_up#0": "tessera_cmp_out_{tessera:output}", "comparator_up#1": "tessera_cmp_out_{tessera:output}"}},
        # tessera:powered is estimated from the redstone parts around the block (any block can use it).
        {"block": "noteblock", "when": {"tessera:powered": True},
         "replace": {"noteblock": "tessera_note_on_{tessera:note}"}},
        {"block": "noteblock", "replace": {"noteblock": "tessera_note_{tessera:note}"}},
        {"block": "jukebox", "when": {"tessera:has_record": True}, "replace": {"jukebox_top": "tessera_on_front"}},
        {"block": ["light_weighted_pressure_plate", "heavy_weighted_pressure_plate"],
         "replace": {"gold_block": "tessera_plate_p{redstone_signal}", "iron_block": "tessera_plate_p{redstone_signal}"}},
    ],
}
with open(os.path.join(ROOT, "state_textures.json"), "w", encoding="utf-8") as f:
    json.dump(rules, f, indent=2)

man_path = os.path.join(ROOT, "manifest.json")
if not os.path.exists(man_path):  # keep UUIDs stable across regenerations
    man = {"format_version": 2,
           "header": {"name": "Tessera Demo", "description": "Redstone power and weighted plate demo for Tessera",
                      "uuid": str(uuid.uuid4()), "version": [0, 1, 0], "min_engine_version": [1, 26, 60]},
           "modules": [{"type": "resources", "uuid": str(uuid.uuid4()), "version": [0, 1, 0]}]}
    with open(man_path, "w", encoding="utf-8") as f:
        json.dump(man, f, indent=2)
print("pack written to", os.path.normpath(ROOT), "textures:", len(texture_data))
