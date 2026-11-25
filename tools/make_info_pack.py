"""Generate the TesseraRedstoneInfo resource pack (Java format, read by Tessera).

    python -I tools/make_info_pack.py

Shows redstone and block-state values as pixel digits (style A: 3x5 digits with a dark
outline), and enchanted-book / potion levels on their icons. Every picture is drawn here; the
models only *name* vanilla models and textures (redstone dust, plates, books...), which Tessera
takes from the player's own game. No vanilla file is copied into the pack.

Also runs make_showcase_pack.py into this pack (seamless glass, 3D crafting table and lantern),
so the 3D test pack and the info pack ship as one.
"""
import json, os, shutil, subprocess, sys
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'packs', 'TesseraRedstoneInfo')
MC = os.path.join(ROOT, 'assets', 'minecraft')
NS = 'minecraft:block/tessera_info/'   # our block models
TX = 'minecraft:block/tessera_info/'   # our block textures
ITX = 'minecraft:item/tessera_info/'   # our item textures / models


def path(*parts):
    p = os.path.join(MC, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def save(obj, *parts):
    with open(path(*parts), 'w', encoding='utf-8') as f:
        json.dump(obj, f, indent=1, ensure_ascii=False)
        f.write('\n')


# ---------------------------------------------------------------------------
# Pixel font (3x5) and number textures
# ---------------------------------------------------------------------------
F3 = {
    '0': ['111', '101', '101', '101', '111'], '1': ['010', '110', '010', '010', '111'], '2': ['111', '001', '111', '100', '111'],
    '3': ['111', '001', '111', '001', '111'], '4': ['101', '101', '111', '001', '001'], '5': ['111', '100', '111', '001', '111'],
    '6': ['111', '100', '111', '101', '111'], '7': ['111', '001', '010', '010', '010'], '8': ['111', '101', '111', '101', '111'],
    '9': ['111', '101', '111', '001', '111'], 'I': ['111', '010', '010', '010', '111'], 'V': ['101', '101', '101', '101', '010'],
    'X': ['101', '101', '010', '101', '101'], '+': ['000', '010', '111', '010', '000'], '-': ['000', '000', '111', '000', '000'],
    'L': ['100', '100', '100', '100', '111'],
}
NOTE_GLYPH = ['011', '010', '010', '110', '110']  # a quaver for "has a record"
OUTLINE = (0, 0, 0, 255)


def glyphs(s):
    return [NOTE_GLYPH if ch == '~' else F3[ch] for ch in s]


def draw_text(img, s, x, y, color, outline=OUTLINE):
    cells = []
    for i, g in enumerate(glyphs(s)):
        for yy, row in enumerate(g):
            for xx, b in enumerate(row):
                if b == '1':
                    cells.append((x + i * 4 + xx, y + yy))
    px = img.load()
    if outline:
        for (cx, cy) in cells:
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    X, Y = cx + dx, cy + dy
                    if 0 <= X < img.width and 0 <= Y < img.height:
                        px[X, Y] = outline
    for (cx, cy) in cells:
        if 0 <= cx < img.width and 0 <= cy < img.height:
            px[cx, cy] = color


def text_width(s):
    return len(s) * 4 - 1


BOXES = {}  # texture -> (x0, y0, x1, y1) of its opaque plate, in pixels


def number_texture(name, s, color, y=6):
    """Digits on a small solid black plate (no see-through pixels: some blocks draw in an opaque
    layer, where transparency would show black anyway)."""
    img = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    x = (16 - text_width(s)) // 2
    box = (x - 1, y - 1, x + text_width(s) + 1, y + 6)
    for yy in range(box[1], box[3]):
        for xx in range(box[0], box[2]):
            img.putpixel((xx, yy), OUTLINE)
    draw_text(img, s, x, y, color, outline=None)
    name = f'{name}_{color[0]:02x}{color[1]:02x}{color[2]:02x}_{y}'
    img.save(path('textures', 'block', 'tessera_info', name + '.png'))
    BOXES[TX + name] = box
    return TX + name


def glyph_plate(name, rows, color, size=8, bg=OUTLINE):
    """A centred square plate (size x size, opaque) with a glyph drawn in it."""
    img = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    o = (16 - size) // 2
    for yy in range(o, o + size):
        for xx in range(o, o + size):
            img.putpixel((xx, yy), bg)
    gw, gh = len(rows[0]), len(rows)
    gx, gy = o + (size - gw) // 2, o + (size - gh) // 2
    for yy, row in enumerate(rows):
        for xx, b in enumerate(row):
            if b == '1':
                img.putpixel((gx + xx, gy + yy), color)
    img.save(path('textures', 'block', 'tessera_info', name + '.png'))
    BOXES[TX + name] = (o, o, o + size, o + size)
    return TX + name


ARROW = ['001100', '011110', '111111', '001100', '001100', '001100']
CROSS = ['100001', '010010', '001100', '001100', '010010', '100001']


def dust_tag(power):
    """Redstone dust number: an upright tag in the dust's own colour with white digits."""
    s_ = str(power)
    img = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    w = text_width(s_) + 4
    h = 9
    x0, y0 = (16 - w) // 2, (16 - h) // 2
    fill = dust_rgb(power) + (255,)
    edge = tuple(max(0, int(c * 0.55)) for c in dust_rgb(power)) + (255,)
    for yy in range(y0, y0 + h):
        for xx in range(x0, x0 + w):
            corner = (xx in (x0, x0 + w - 1)) and (yy in (y0, y0 + h - 1))
            if corner:
                continue
            border = xx in (x0, x0 + w - 1) or yy in (y0, y0 + h - 1)
            img.putpixel((xx, yy), edge if border else fill)
    draw_text(img, s_, x0 + 2, y0 + 2, WHITE, outline=None)
    name = f'dust_{power}'
    img.save(path('textures', 'block', 'tessera_info', name + '.png'))
    BOXES[TX + name] = (x0, y0, x0 + w, y0 + h)
    return TX + name


def corner_digit(name, s_, color):
    """A small outlined digit in the top-left corner of a top face (as Wutong marks delays)."""
    img = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    draw_text(img, s_, 2, 2, color)
    img.save(path('textures', 'block', 'tessera_info', name + '.png'))
    BOXES[TX + name] = (1, 1, 2 + text_width(s_) + 1, 8)
    return TX + name


def dust_rgb(power):  # Java's dust colour by power
    f = power / 15
    r = f * 0.6 + (0.4 if power else 0.3)
    g = max(0.0, f * f * 0.7 - 0.5)
    b = max(0.0, f * f * 0.6 - 0.7)
    return (int(min(1, r) * 255), int(g * 255), int(b * 255))


WHITE = (255, 255, 255, 255)
RED = (255, 90, 70, 255)
GOLD = (255, 220, 90, 255)


def dust_color(power):  # digits brighten with the power, like the dust itself
    k = 0.45 + 0.55 * power / 15
    return (255, int(90 * k + 40), int(70 * k + 30), 255) if power else (190, 190, 190, 255)


# ---------------------------------------------------------------------------
# Overlay models: one flat face (or a ring of faces) carrying a number texture
# ---------------------------------------------------------------------------
# Text lying on a top face reads looking south, as a Java player faces by default (yaw 0 is
# south): the picture is turned half round (a model's up face has its top towards north).
SOUTH = 180


def flat_top(tex, y, rot=0):
    """The plate on a face looking up at height y (pixels); rot turns the picture (180: it
    reads facing south)."""
    x0, z0, x1, z1 = BOXES[tex]
    face = {'uv': [x0, z0, x1, z1], 'texture': '#n'}
    if rot:
        face['rotation'] = rot
    return {'textures': {'n': tex, 'particle': tex},
            'elements': [{'from': [x0, y, z0], 'to': [x1, y, z1], 'faces': {'up': face}}]}


def side_faces(tex, sides, cull=True, rotations=None):
    """The plate on block faces, each a flat element on that face."""
    x0, y0, x1, y1 = BOXES[tex]
    uv = [x0, y0, x1, y1]
    lo, hi = 16 - y1, 16 - y0  # height of the plate
    shapes = {
        'north': ([16 - x1, lo, 0], [16 - x0, hi, 0]),
        'south': ([x0, lo, 16], [x1, hi, 16]),
        'east': ([16, lo, 16 - x1], [16, hi, 16 - x0]),
        'west': ([0, lo, x0], [0, hi, x1]),
        'up': ([x0, 16, y0], [x1, 16, y1]),
        'down': ([x0, 0, y0], [x1, 0, y1]),
    }
    els = []
    for f in sides:
        a, b = shapes[f]
        face = {'uv': uv, 'texture': '#n'}
        if cull:
            face['cullface'] = f
        if rotations and rotations.get(f):
            face['rotation'] = rotations[f]
        els.append({'from': a, 'to': b, 'faces': {f: face}})
    return {'textures': {'n': tex, 'particle': tex}, 'elements': els}


def all_sides(tex, cull=True):
    """The number on the four sides and the top of a full block."""
    return side_faces(tex, ('north', 'south', 'east', 'west', 'up'), cull, rotations={'up': SOUTH})


def front(tex):
    """The number on the north face of a full block (rotated by the blockstate)."""
    return side_faces(tex, ('north',))


def model(name, obj):
    save(obj, 'models', 'block', 'tessera_info', name + '.json')
    return NS + name


def mc(name):
    return 'minecraft:block/' + name


# ---------------------------------------------------------------------------
# Blockstates (written out here, the way Java's own ones work, plus the number parts)
# ---------------------------------------------------------------------------
def redstone_wire():
    su = 'side|up'
    parts = [
        {'when': {'OR': [{'east': 'none', 'north': 'none', 'south': 'none', 'west': 'none'},
                         {'east': su, 'north': su}, {'east': su, 'south': su}, {'south': su, 'west': su}, {'north': su, 'west': su}]},
         'apply': {'model': mc('redstone_dust_dot')}},
        {'when': {'north': su}, 'apply': {'model': mc('redstone_dust_side0')}},
        {'when': {'south': su}, 'apply': {'model': mc('redstone_dust_side_alt0')}},
        {'when': {'east': su}, 'apply': {'model': mc('redstone_dust_side_alt1'), 'y': 270}},
        {'when': {'west': su}, 'apply': {'model': mc('redstone_dust_side1'), 'y': 270}},
        {'when': {'north': 'up'}, 'apply': {'model': mc('redstone_dust_up')}},
        {'when': {'east': 'up'}, 'apply': {'model': mc('redstone_dust_up'), 'y': 90}},
        {'when': {'south': 'up'}, 'apply': {'model': mc('redstone_dust_up'), 'y': 180}},
        {'when': {'west': 'up'}, 'apply': {'model': mc('redstone_dust_up'), 'y': 270}},
    ]
    dust_textures()
    for p in range(16):
        t = dust_number(p)
        m = {'ambientocclusion': False, 'textures': {'n': t, 'particle': t},
             'elements': [{'from': [5, 0.6, 5], 'to': [11, 0.6, 11], 'shade': False,
                           'faces': {'up': {'uv': [0, 0, 16, 16], 'texture': '#n', 'rotation': SOUTH}}}]}
        parts.append({'when': {'power': str(p)}, 'apply': {'model': model(f'dust_{p}', m)}})
    save({'multipart': parts}, 'blockstates', 'redstone_wire.json')


def dust_textures():
    """Straight, even dust (white: the game tints it by power), drawn here."""
    def save_tex(name, img):
        img.save(path('textures', 'block', name + '.png'))
    W = (255, 255, 255, 255)
    line = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    for y in range(16):
        for x in (7, 8):
            line.putpixel((x, y), W)
    save_tex('redstone_dust_line0', line)
    save_tex('redstone_dust_line1', line)  # Java's line0 and line1 both run north-south
    dot = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    for y in range(6, 10):
        for x in range(6, 10):
            dot.putpixel((x, y), W)
    save_tex('redstone_dust_dot', dot)
    save_tex('redstone_dust_overlay', Image.new('RGBA', (16, 16), (0, 0, 0, 0)))


def dust_number(power):
    """Digits at double size with an underline, shown small at the centre of the dust."""
    img = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    s_ = str(power)
    w = len(s_) * 8 - 2
    x0 = (16 - w) // 2
    for i, ch in enumerate(s_):
        for yy, row in enumerate(F3[ch]):
            for xx, b in enumerate(row):
                if b == '1':
                    for dx in (0, 1):
                        for dy in (0, 1):
                            img.putpixel((x0 + i * 8 + xx * 2 + dx, 2 + yy * 2 + dy), WHITE)
    for x in range(x0, x0 + w):
        img.putpixel((x, 14), WHITE)
    name = f'dust_{power}'
    img.save(path('textures', 'block', 'tessera_info', name + '.png'))
    return TX + name


def plates():
    for block in ('light_weighted_pressure_plate', 'heavy_weighted_pressure_plate'):
        parts = [{'when': {'power': '0'}, 'apply': {'model': mc(block)}},
                 {'when': {'power': '|'.join(str(p) for p in range(1, 16))}, 'apply': {'model': mc(block + '_down')}}]
        for p in range(16):
            t = number_texture(f'signal_{p}', str(p), WHITE)
            parts.append({'when': {'power': str(p)}, 'apply': {'model': model(f'plate_{p}', flat_top(t, 1.05 if p == 0 else 0.55, rot=SOUTH))}})
        save({'multipart': parts}, 'blockstates', block + '.json')


def daylight_detector():
    parts = [{'when': {'inverted': 'false'}, 'apply': {'model': mc('daylight_detector')}},
             {'when': {'inverted': 'true'}, 'apply': {'model': mc('daylight_detector_inverted')}}]
    for p in range(16):
        t = number_texture(f'signal_{p}', str(p), WHITE)
        parts.append({'when': {'power': str(p)}, 'apply': {'model': model(f'daylight_{p}', flat_top(t, 6.05, rot=SOUTH))}})
    save({'multipart': parts}, 'blockstates', 'daylight_detector.json')


INSTRUMENTS = {  # 6x6 pictures, colour
    'harp': (['011110', '100001', '101101', '101101', '100001', '011110'], (255, 220, 90)),
    'basedrum': (['011110', '111111', '100001', '100001', '111111', '011110'], (230, 90, 70)),
    'snare': (['111111', '100001', '111111', '100001', '100001', '111111'], (230, 230, 230)),
    'hat': (['000000', '111111', '001100', '001100', '111111', '000000'], (200, 200, 120)),
    'bass': (['000110', '000110', '001100', '011000', '111000', '110000'], (180, 120, 70)),
    'flute': (['000001', '000010', '000100', '001000', '010000', '100000'], (240, 200, 140)),
    'bell': (['001100', '011110', '011110', '111111', '111111', '001100'], (255, 210, 60)),
    'guitar': (['000011', '000110', '011100', '111100', '111000', '110000'], (210, 150, 90)),
    'chime': (['101010', '101010', '101010', '101010', '001010', '000010'], (140, 220, 255)),
    'xylophone': (['111111', '000000', '111110', '000000', '111100', '000000'], (240, 240, 220)),
    'iron_xylophone': (['111111', '000000', '111110', '000000', '111100', '000000'], (170, 200, 220)),
    'cow_bell': (['001100', '010010', '010010', '100001', '111111', '001100'], (200, 160, 110)),
    'didgeridoo': (['110000', '111000', '011100', '001110', '000111', '000011'], (160, 100, 60)),
    'bit': (['111111', '101011', '111111', '110101', '111111', '100001'], (90, 230, 120)),
    'banjo': (['011100', '111110', '111110', '011100', '001000', '001000'], (230, 200, 140)),
    'pling': (['001000', '101010', '011100', '111110', '011100', '101010'], (120, 255, 200)),
    'zombie': (['111111', '100001', '110011', '100001', '101101', '111111'], (110, 180, 110)),
    'skeleton': (['111111', '100001', '110011', '100001', '101101', '111111'], (230, 230, 230)),
    'creeper': (['111111', '110011', '111111', '100001', '101101', '111111'], (90, 200, 90)),
    'dragon': (['111111', '100001', '110011', '100001', '101101', '111111'], (180, 90, 220)),
    'wither_skeleton': (['111111', '100001', '110011', '100001', '101101', '111111'], (110, 110, 110)),
    'piglin': (['111111', '100001', '110011', '100001', '101101', '111111'], (240, 170, 150)),
    'custom_head': (['111111', '100001', '110011', '100001', '101101', '111111'], (200, 200, 200)),
}


def note_block():
    parts = [{'apply': {'model': mc('note_block')}}]
    for n in range(25):
        t = number_texture(f'note_{n}', str(n), WHITE)
        parts.append({'when': {'note': str(n)}, 'apply': {'model': model(f'note_{n}', side_faces(t, ('north', 'south', 'east', 'west')))}})
    for inst, (rows, col) in INSTRUMENTS.items():
        t = glyph_plate(f'inst_{inst}', rows, col + (255,), size=8)
        parts.append({'when': {'instrument': inst}, 'apply': {'model': model(f'inst_{inst}', side_faces(t, ('up',), rotations={'up': SOUTH}))}})
    save({'multipart': parts}, 'blockstates', 'note_block.json')


def target():
    parts = [{'apply': {'model': mc('target')}}]
    for p in range(1, 16):
        t = number_texture(f'signal_{p}', str(p), WHITE)
        parts.append({'when': {'power': str(p)}, 'apply': {'model': model(f'target_{p}', all_sides(t))}})
    save({'multipart': parts}, 'blockstates', 'target.json')


FACING_Y = {'south': 0, 'west': 90, 'north': 180, 'east': 270}  # repeater / comparator models face south


GREY = (170, 170, 170, 255)
LIT = (255, 60, 50, 255)


def repeater():
    parts = []
    for d in range(1, 5):
        for powered in ('false', 'true'):
            t = corner_digit(f'delay_{d}_{powered}', str(d), LIT if powered == 'true' else GREY)
            model(f'delay_{d}_{powered}', flat_top(t, 2.05))
        for facing, y in FACING_Y.items():
            for locked in ('false', 'true'):
                for powered in ('false', 'true'):
                    base = f'repeater_{d}tick' + ('_on' if powered == 'true' else '') + ('_locked' if locked == 'true' else '')
                    parts.append({'when': {'delay': str(d), 'facing': facing, 'locked': locked, 'powered': powered},
                                  'apply': {'model': mc(base), 'y': y}})
            for powered in ('false', 'true'):
                parts.append({'when': {'delay': str(d), 'facing': facing, 'powered': powered},
                              'apply': {'model': NS + f'delay_{d}_{powered}', 'y': y}})
    save({'multipart': parts}, 'blockstates', 'repeater.json')


def comparator():
    parts = []
    minus = corner_digit('subtract', '-', LIT)
    mm = model('subtract', flat_top(minus, 2.05))
    for facing, y in FACING_Y.items():
        for mode in ('compare', 'subtract'):
            for powered in ('false', 'true'):
                base = 'comparator' + ('_on' if powered == 'true' else '') + ('_subtract' if mode == 'subtract' else '')
                parts.append({'when': {'facing': facing, 'mode': mode, 'powered': powered}, 'apply': {'model': mc(base), 'y': y}})
        parts.append({'when': {'facing': facing, 'mode': 'subtract'}, 'apply': {'model': mm, 'y': y}})
    save({'multipart': parts}, 'blockstates', 'comparator.json')


def composter():
    parts = [{'apply': {'model': mc('composter')}}]
    heights = {1: 3, 2: 5, 3: 7, 4: 9, 5: 11, 6: 13, 7: 15, 8: 15}
    for lv in range(1, 9):
        parts.append({'when': {'level': str(lv)}, 'apply': {'model': mc('composter_contents' + (str(lv) if lv < 8 else '_ready'))}})
    for lv in range(9):
        t = number_texture(f'level_{lv}', str(lv), WHITE)
        parts.append({'when': {'level': str(lv)}, 'apply': {'model': model(f'composter_{lv}', flat_top(t, heights.get(lv, 2) + 0.05, rot=SOUTH))}})
    save({'multipart': parts}, 'blockstates', 'composter.json')


def respawn_anchor():
    parts = []
    for c in range(5):
        parts.append({'when': {'charges': str(c)}, 'apply': {'model': mc(f'respawn_anchor_{c}')}})
        t = number_texture(f'level_{c}', str(c), GOLD)
        parts.append({'when': {'charges': str(c)}, 'apply': {'model': model(f'anchor_{c}', all_sides(t))}})
    save({'multipart': parts}, 'blockstates', 'respawn_anchor.json')


def beehives():
    for block in ('beehive', 'bee_nest'):
        parts = []
        for facing, y in {'north': 0, 'east': 90, 'south': 180, 'west': 270}.items():
            for h in range(6):
                parts.append({'when': {'facing': facing, 'honey_level': str(h)},
                              'apply': {'model': mc(block + ('_honey' if h == 5 else '_empty')), 'y': y}})
                t = number_texture(f'level_{h}', str(h), GOLD, y=9)
                parts.append({'when': {'facing': facing, 'honey_level': str(h)},
                              'apply': {'model': model(f'honey_{h}', front(t)), 'y': y}})
        save({'multipart': parts}, 'blockstates', block + '.json')


def cauldron():
    levels = {1: ('water_cauldron_level1', 9), 2: ('water_cauldron_level2', 12), 3: ('water_cauldron_full', 15)}
    parts = []
    for lv, (m, h) in levels.items():
        parts.append({'when': {'level': str(lv)}, 'apply': {'model': mc(m)}})
        t = number_texture(f'level_{lv}', str(lv), WHITE)
        parts.append({'when': {'level': str(lv)}, 'apply': {'model': model(f'cauldron_{lv}', flat_top(t, h + 0.05, rot=SOUTH))}})
    save({'multipart': parts}, 'blockstates', 'water_cauldron.json')


def jukebox():
    t = number_texture('record', '~', GOLD)
    save({'multipart': [{'apply': {'model': mc('jukebox')}},
                        {'when': {'has_record': 'true'}, 'apply': {'model': model('record', flat_top(t, 16, rot=SOUTH))}}]},
         'blockstates', 'jukebox.json')


# Front = north in the models; the blockstate turns them (Java's own convention for these).
FACE_ROT = {'north': (0, 0), 'east': (0, 90), 'south': (0, 180), 'west': (0, 270), 'up': (270, 0), 'down': (90, 0)}
ARROW_FACES = ('east', 'west', 'up', 'down')
ARROW_ROT = {'up': 0, 'down': 180, 'east': 90, 'west': 270}  # pictures pointing to the front


def arrows(name, active, back=False):
    t = glyph_plate(f'arrow_{"on" if active else "off"}', ARROW, LIT if active else GREY)
    rot = {f: (r + 180) % 360 if back else r for f, r in ARROW_ROT.items()}
    return model(name, side_faces(t, ARROW_FACES, rotations=rot))


def facing_parts(base_for, active_key, active_values=('true',), back=False):
    """base_for(facing, active) -> (model, x, y) of the block itself. back: arrows point away
    from the front (where an observer sends its signal)."""
    parts = []
    tag = '_back' if back else ''
    on, off = arrows('arrows_on' + tag, True, back), arrows('arrows_off' + tag, False, back)
    for facing, (x, y) in FACE_ROT.items():
        for active in ('false', 'true'):
            m, bx, by = base_for(facing, active)
            ap = {'model': m}
            if bx:
                ap['x'] = bx
            if by:
                ap['y'] = by
            parts.append({'when': {'facing': facing, active_key: active}, 'apply': ap})
            ov = {'model': on if active in active_values else off}
            if x:
                ov['x'] = x
            if y:
                ov['y'] = y
            parts.append({'when': {'facing': facing, active_key: active}, 'apply': ov})
    return parts


def pistons():
    # The pushed-out head shows a red dot in the middle of its face, as XeKr marks it. Bedrock
    # draws the arm itself (not from a blockstate), so the dot belongs to the base, one block
    # ahead: on the head's face (z = -16, just in front of it).
    dot = glyph_plate('piston_dot', ['1'], LIT, size=2, bg=LIT)
    dm = model('piston_head_dot', {'textures': {'n': dot, 'particle': dot},
                                   'elements': [{'from': [7, 7, -16.05], 'to': [9, 9, -16.05],
                                                 'faces': {'north': {'uv': [7, 7, 9, 9], 'texture': '#n'}}}]})
    for block in ('piston', 'sticky_piston'):
        def base(facing, extended, block=block):
            x, y = FACE_ROT[facing]
            return (mc('piston_base') if extended == 'true' else mc(block)), x, y
        parts = facing_parts(base, 'extended')
        for facing, (x, y) in FACE_ROT.items():
            ap = {'model': dm}
            if x:
                ap['x'] = x
            if y:
                ap['y'] = y
            parts.append({'when': {'facing': facing, 'extended': 'true'}, 'apply': ap})
        save({'multipart': parts}, 'blockstates', block + '.json')


def observer():
    def base(facing, powered):
        x, y = FACE_ROT[facing]
        return mc('observer_on' if powered == 'true' else 'observer'), x, y
    save({'multipart': facing_parts(base, 'powered', back=True)}, 'blockstates', 'observer.json')


# Dispensers and droppers as XeKr shows them: a channel drawn on the sides and top that opens
# towards the front, dim when idle and lit when triggered, and a bar on the back. Drawn as thin
# solid strips (these blocks draw in an opaque layer: no see-through pixels allowed).
_CHANNEL_BACK = [  # a pipe turning back on itself, drawn open towards the back (v = 0 is the front edge)
    (3, 3, 13, 4), (3, 4, 4, 5), (12, 4, 13, 5), (3, 5, 4, 6), (5, 5, 11, 6), (12, 5, 13, 6), (3, 6, 4, 7), (5, 6, 6, 7), (10, 6, 11, 7), (12, 6, 13, 7), (3, 7, 4, 8), (5, 7, 6, 8), (10, 7, 11, 8), (12, 7, 13, 8), (3, 8, 4, 9), (5, 8, 6, 9), (10, 8, 11, 9), (12, 8, 13, 9), (3, 9, 4, 10), (5, 9, 6, 10), (10, 9, 11, 10), (12, 9, 13, 10), (3, 10, 4, 11), (5, 10, 7, 11), (9, 10, 11, 11), (12, 10, 13, 11), (3, 11, 4, 12), (6, 11, 7, 12), (9, 11, 10, 12), (12, 11, 13, 12), (3, 12, 7, 13), (9, 12, 13, 13),
]
# ... and used the other way round: open towards the front, where the items come out
CHANNEL = [(u0, 16 - v1, u1, 16 - v0) for u0, v0, u1, v1 in _CHANNEL_BACK]


def _square(u, v, size):
    """A 1 px outlined square with its top-left corner at (u, v)."""
    e = u + size, v + size
    return [(u, v, e[0], v + 1), (u, e[1] - 1, e[0], e[1]), (u, v + 1, u + 1, e[1] - 1), (e[0] - 1, v + 1, e[0], e[1] - 1)]


BACK_BAR = _square(3, 3, 10) + _square(5, 5, 6)  # the back: a square in a square (as Wutong marks it)


def solid(name, rgb):
    img = Image.new('RGBA', (16, 16), rgb + (255,))
    img.save(path('textures', 'block', 'tessera_info', name + '.png'))
    return TX + name


def turn(rect, rot):
    """A face-space rectangle turned clockwise by rot degrees about the face centre."""
    u0, v0, u1, v1 = rect
    for _ in range((rot // 90) % 4):
        u0, v0, u1, v1 = 16 - v1, u0, 16 - v0, u1
    return u0, v0, u1, v1


def strips(tex, faces):
    """faces: {face: (rects, rot)}; each rect becomes a flat element on that face."""
    els = []
    for f, (rects, rot) in faces.items():
        for r in rects:
            x0, y0, x1, y1 = turn(r, rot)  # texture space of that face
            lo, hi = 16 - y1, 16 - y0
            shapes = {
                'north': ([16 - x1, lo, 0], [16 - x0, hi, 0]),
                'south': ([x0, lo, 16], [x1, hi, 16]),
                'east': ([16, lo, 16 - x1], [16, hi, 16 - x0]),
                'west': ([0, lo, x0], [0, hi, x1]),
                'up': ([x0, 16, y0], [x1, 16, y1]),
                'down': ([x0, 0, 16 - y1], [x1, 0, 16 - y0]),
            }
            fr, to = shapes[f]
            els.append({'from': fr, 'to': to, 'faces': {f: {'uv': [0, 0, 1, 1], 'texture': '#n', 'cullface': f}}})
    return {'textures': {'n': tex, 'particle': tex}, 'elements': els}


DISPENSER_COLORS = {'dispenser': ((150, 72, 72), (255, 40, 40)), 'dropper': ((165, 135, 90), (255, 165, 30))}


def dispensers():
    for block in ('dispenser', 'dropper'):
        dim, lit = DISPENSER_COLORS[block]
        models = {}
        for state, rgb in (('false', dim), ('true', lit)):
            t = solid(f'{block}_{state}', rgb)
            faces = {f: (CHANNEL, ARROW_ROT[f]) for f in ARROW_FACES}
            faces['south'] = (BACK_BAR, 0)
            models[state] = model(f'{block}_{state}', strips(t, faces))
        parts = []
        for facing, (x, y) in FACE_ROT.items():
            for triggered in ('false', 'true'):
                if facing == 'up':
                    bm, bx, by = mc(block + '_vertical'), 0, 0
                elif facing == 'down':
                    bm, bx, by = mc(block + '_vertical'), 180, 0
                else:
                    bm, bx, by = mc(block), x, y
                ap = {'model': bm}
                if bx:
                    ap['x'] = bx
                if by:
                    ap['y'] = by
                parts.append({'when': {'facing': facing, 'triggered': triggered}, 'apply': ap})
                ov = {'model': models[triggered]}
                if x:
                    ov['x'] = x
                if y:
                    ov['y'] = y
                parts.append({'when': {'facing': facing, 'triggered': triggered}, 'apply': ov})
        save({'multipart': parts}, 'blockstates', block + '.json')


def hopper():
    lock = glyph_plate('locked', CROSS, LIT, size=8)
    lm = model('hopper_locked', side_faces(lock, ('north', 'south', 'east', 'west'), cull=False))
    # the plate sits on the hopper's top part (y 10..16): shift it there
    data = json.load(open(path('models', 'block', 'tessera_info', 'hopper_locked.json'), encoding='utf-8'))
    for e in data['elements']:
        e['from'][1], e['to'][1] = 10.5, 15.5
    # and on the floor inside the bowl (y 11), seen from above: a hopper minecart's walls hide
    # the sides
    x0, z0, x1, z1 = BOXES[lock]
    data['elements'].append({'from': [x0, 11.05, z0], 'to': [x1, 11.05, z1],
                             'faces': {'up': {'uv': [x0, z0, x1, z1], 'texture': '#n'}}})
    save(data, 'models', 'block', 'tessera_info', 'hopper_locked.json')
    parts = []
    for facing, y in {'down': None, 'north': 0, 'east': 90, 'south': 180, 'west': 270}.items():
        ap = {'model': mc('hopper' if facing == 'down' else 'hopper_side')}
        if y:
            ap['y'] = y
        parts.append({'when': {'facing': facing}, 'apply': ap})
    parts.append({'when': {'enabled': 'false'}, 'apply': {'model': lm}})
    # Output direction on the funnel (x/z 4..12, y 4..10): down -> arrows down on all four
    # sides; sideways -> arrows on the two sides along it, pointing that way.
    small = glyph_plate('arrow_small', ['0110', '1111', '0110', '0110'], WHITE, size=6)

    def funnel(sides, rots):
        els = []
        lo, hi = 4.5, 9.5
        a, b = 5.5, 10.5  # plate across the face (5 px)
        for f in sides:
            if f == 'north':
                fr, to = [a, lo, 4], [b, hi, 4]
            elif f == 'south':
                fr, to = [a, lo, 12], [b, hi, 12]
            elif f == 'west':
                fr, to = [4, lo, a], [4, hi, b]
            else:
                fr, to = [12, lo, a], [12, hi, b]
            face = {'uv': [5, 5, 11, 11], 'texture': '#n', 'rotation': rots[f]}
            els.append({'from': fr, 'to': to, 'faces': {f: face}})
        return {'textures': {'n': small, 'particle': small}, 'elements': els}
    down = model('hopper_down', funnel(('north', 'south', 'east', 'west'), {f: 180 for f in ('north', 'south', 'east', 'west')}))
    side = model('hopper_out', funnel(('east', 'west'), {'east': 90, 'west': 270}))
    parts.append({'when': {'facing': 'down'}, 'apply': {'model': down}})
    for facing, y in {'north': 0, 'east': 90, 'south': 180, 'west': 270}.items():
        ap = {'model': side}
        if y:
            ap['y'] = y
        parts.append({'when': {'facing': facing}, 'apply': ap})
    save({'multipart': parts}, 'blockstates', 'hopper.json')


BUTTONS = ['oak', 'spruce', 'birch', 'jungle', 'acacia', 'dark_oak', 'mangrove', 'cherry', 'bamboo', 'crimson', 'warped', 'pale_oak',
           'stone', 'polished_blackstone']


def buttons():
    """A pressed button shows a red top (and an unpressed one stays as it is)."""
    lit = glyph_plate('button_lit', ['1'], LIT, size=4, bg=LIT)
    m = model('button_lit', {'textures': {'n': lit, 'particle': lit},
                             'elements': [{'from': [5.5, 1.05, 6.5], 'to': [10.5, 1.05, 9.5],
                                           'faces': {'up': {'uv': [6, 6, 10, 10], 'texture': '#n'}}}]})
    rot = {'floor': {'north': (0, 0), 'east': (0, 90), 'south': (0, 180), 'west': (0, 270)},
           'wall': {'north': (90, 0), 'east': (90, 90), 'south': (90, 180), 'west': (90, 270)},
           'ceiling': {'north': (180, 180), 'east': (180, 270), 'south': (180, 0), 'west': (180, 90)}}
    for kind in BUTTONS:
        parts = []
        for face, by_facing in rot.items():
            for facing, (x, y) in by_facing.items():
                for powered in ('false', 'true'):
                    ap = {'model': mc(f'{kind}_button' + ('_pressed' if powered == 'true' else ''))}
                    if x:
                        ap['x'] = x
                    if y:
                        ap['y'] = y
                    if face == 'wall':
                        ap['uvlock'] = True
                    parts.append({'when': {'face': face, 'facing': facing, 'powered': powered}, 'apply': ap})
                ov = {'model': m}
                if x:
                    ov['x'] = x
                if y:
                    ov['y'] = y
                parts.append({'when': {'face': face, 'facing': facing, 'powered': 'true'}, 'apply': ov})
        save({'multipart': parts}, 'blockstates', f'{kind}_button.json')


# ---------------------------------------------------------------------------
# Items: enchanted books by enchantment and level (OptiFine CIT), potions by strength
# ---------------------------------------------------------------------------
ROMAN = {1: 'I', 2: 'II', 3: 'III', 4: 'IV', 5: 'V'}
CATEGORY = {  # gem colour in the book's corner
    'weapon': ((232, 52, 52), ['sharpness', 'smite', 'bane_of_arthropods', 'knockback', 'fire_aspect', 'looting', 'sweeping_edge',
                               'density', 'breach', 'wind_burst']),
    'tool': ((255, 200, 40), ['efficiency', 'fortune', 'silk_touch']),
    'armor': ((70, 140, 255), ['protection', 'fire_protection', 'blast_protection', 'projectile_protection', 'thorns', 'respiration',
                               'aqua_affinity', 'depth_strider', 'frost_walker', 'feather_falling', 'soul_speed', 'swift_sneak']),
    'bow': ((90, 210, 90), ['power', 'punch', 'flame', 'infinity', 'multishot', 'piercing', 'quick_charge']),
    'trident': ((60, 220, 220), ['loyalty', 'impaling', 'riptide', 'channeling', 'lunge']),
    'fishing': ((120, 180, 255), ['luck_of_the_sea', 'lure']),
    'general': ((245, 245, 245), ['unbreaking', 'mending']),
    'curse': ((170, 70, 220), ['binding_curse', 'vanishing_curse']),
}
MAX_LEVEL = {'sharpness': 5, 'smite': 5, 'bane_of_arthropods': 5, 'knockback': 2, 'fire_aspect': 2, 'looting': 3, 'sweeping_edge': 3,
             'density': 5, 'breach': 4, 'wind_burst': 3, 'efficiency': 5, 'fortune': 3, 'silk_touch': 1, 'protection': 4,
             'fire_protection': 4, 'blast_protection': 4, 'projectile_protection': 4, 'thorns': 3, 'respiration': 3, 'aqua_affinity': 1,
             'depth_strider': 3, 'frost_walker': 2, 'feather_falling': 4, 'soul_speed': 3, 'swift_sneak': 3, 'power': 5, 'punch': 2,
             'flame': 1, 'infinity': 1, 'multishot': 1, 'piercing': 4, 'quick_charge': 3, 'loyalty': 3, 'impaling': 5, 'riptide': 3,
             'channeling': 1, 'lunge': 3, 'luck_of_the_sea': 3, 'lure': 3, 'unbreaking': 3, 'mending': 1, 'binding_curse': 1,
             'vanishing_curse': 1}


def item_texture(name, img):
    img.save(path('textures', 'item', 'tessera_info', name + '.png'))
    return ITX + name


def gem(color):
    img = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    px = img.load()
    for (x, y) in [(1, 13), (2, 13), (1, 14), (2, 14)]:
        px[x, y] = color + (255,)
    for (x, y) in [(0, 13), (0, 14), (3, 13), (3, 14), (1, 12), (2, 12), (1, 15), (2, 15)]:
        px[x, y] = OUTLINE
    return img


def level_overlay(s):
    img = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    draw_text(img, s, 16 - text_width(s) - 1, 1, GOLD)
    return img


def books():
    level_tex = {lv: item_texture(f'level_{lv}', level_overlay(ROMAN[lv])) for lv in ROMAN}
    gem_tex = {cat: item_texture(f'gem_{cat}', gem(col)) for cat, (col, _) in CATEGORY.items()}
    for cat, (_, names) in CATEGORY.items():
        for ench in names:
            for lv in range(1, MAX_LEVEL[ench] + 1):
                name = f'book_{ench}_{lv}'
                save({'parent': 'minecraft:item/generated',
                      'textures': {'layer0': 'minecraft:item/enchanted_book', 'layer1': gem_tex[cat], 'layer2': level_tex[lv]}},
                     'models', 'item', 'tessera_info', name + '.json')
                with open(path('optifine', 'cit', 'tessera_info', 'books', f'{ench}_{lv}.properties'), 'w', encoding='utf-8') as f:
                    f.write(f'type=item\nitems=enchanted_book\nenchantmentIDs=minecraft:{ench}\nenchantmentLevels={lv}\n'
                            f'model=minecraft:item/tessera_info/{name}\n')


POTIONS = ['night_vision', 'invisibility', 'leaping', 'fire_resistance', 'swiftness', 'slowness', 'turtle_master', 'water_breathing',
           'healing', 'harming', 'poison', 'regeneration', 'strength', 'weakness', 'slow_falling']
STRONG = ['leaping', 'swiftness', 'slowness', 'turtle_master', 'healing', 'harming', 'poison', 'regeneration', 'strength']
LONG = ['night_vision', 'invisibility', 'leaping', 'fire_resistance', 'swiftness', 'slowness', 'turtle_master', 'water_breathing',
        'poison', 'regeneration', 'strength', 'weakness', 'slow_falling']


def potions():
    marks = {'strong': item_texture('potion_strong', level_overlay('II')), 'long': item_texture('potion_long', level_overlay('+'))}
    for item, overlay, bottle in (('potion', 'item/potion_overlay', 'item/potion'),
                                  ('splash_potion', 'item/potion_overlay', 'item/splash_potion'),
                                  ('lingering_potion', 'item/potion_overlay', 'item/lingering_potion')):
        for kind in ('strong', 'long'):
            save({'parent': 'minecraft:item/generated',
                  'textures': {'layer0': 'minecraft:' + overlay, 'layer1': 'minecraft:' + bottle, 'layer2': marks[kind]}},
                 'models', 'item', 'tessera_info', f'{item}_{kind}.json')
        tint = [{'type': 'minecraft:potion', 'default': -13083194}]
        cases = []
        for p in STRONG:
            cases.append({'when': [{'potion': f'minecraft:strong_{p}'}],
                          'model': {'type': 'minecraft:model', 'model': f'minecraft:item/tessera_info/{item}_strong', 'tints': tint}})
        for p in LONG:
            cases.append({'when': [{'potion': f'minecraft:long_{p}'}],
                          'model': {'type': 'minecraft:model', 'model': f'minecraft:item/tessera_info/{item}_long', 'tints': tint}})
        save({'model': {'type': 'minecraft:select', 'property': 'minecraft:component', 'component': 'minecraft:potion_contents',
                        'cases': cases, 'fallback': {'type': 'minecraft:model', 'model': f'minecraft:item/{item}', 'tints': tint}}},
             'items', item + '.json')


# ---------------------------------------------------------------------------
# Scaffolding at the end of its reach (hanging, distance 6 or 7: one more would fall), as
# XeKr marks it: a red frame on the top.
def scaffolding():
    t = solid('scaffold_limit', (230, 30, 30))
    els = []
    for u0, v0, u1, v1 in _square(3, 3, 10):
        els.append({'from': [u0, 16.02, v0], 'to': [u1, 16.02, v1],
                    'faces': {'up': {'uv': [0, 0, 1, 1], 'texture': '#n'}, 'down': {'uv': [0, 0, 1, 1], 'texture': '#n'}}})
    m = model('scaffolding_limit', {'textures': {'n': t, 'particle': t}, 'elements': els})
    save({'multipart': [
        {'when': {'bottom': 'false'}, 'apply': {'model': mc('scaffolding_stable')}},
        {'when': {'bottom': 'true'}, 'apply': {'model': mc('scaffolding_unstable')}},
        {'when': {'bottom': 'true', 'distance': '6|7'}, 'apply': {'model': m}},
    ]}, 'blockstates', 'scaffolding.json')


# ---------------------------------------------------------------------------
# Fluids: flow direction. The game itself turns the flowing texture along the flow (Java and
# Bedrock alike; Tessera hands Bedrock the Java flow strip), with "down" in the picture being
# downstream, so arrows pointing down in the texture show the direction. A face shows half the
# 32 px texture: the middle (pixels 8..24) on top, and on the sides of falling liquid the
# top-left quarter, unless the pack asks Tessera to show the middle there too ("flow_sides":
# "middle" in the .mcmeta, as here). So one arrow at the middle is centred on every face. It
# stays put while the streaks behind it scroll downstream.
def _arrow(cx, top, shaft):
    """A down arrow: a 2 px shaft of the given length over an 8-4 px head; cx is the left
    column of the shaft."""
    px = [(cx + d, top + r) for r in range(shaft) for d in (0, 1)]
    for i, half in enumerate((4, 3, 2, 1)):
        px += [(x, top + shaft + i) for x in range(cx + 1 - half, cx + 1 + half)]
    return px


FLOW_ARROWS = _arrow(15, 12, 4)


def flow_strip(name, base, streak, arrow, frametime):
    import random
    rnd = random.Random(name)
    back = Image.new('RGBA', (16, 16))
    for y in range(16):  # the background: soft streaks running with the flow
        for x in range(16):
            c = streak if (x * 7 + (y // 4) * 3) % 5 == 0 else base
            j = rnd.randint(-6, 6)
            back.putpixel((x, y), tuple(max(0, min(255, v + j)) for v in c) + (255,))
    frames = 16
    marks = set(FLOW_ARROWS)
    strip = Image.new('RGBA', (32, 32 * frames))
    for f in range(frames):
        for y in range(32):
            for x in range(32):
                px = arrow + (255,) if (x, y) in marks else back.getpixel((x % 16, (y - f) % 16))
                strip.putpixel((x, f * 32 + y), px)
    strip.save(path('textures', 'block', name + '.png'))
    with open(path('textures', 'block', name + '.png.mcmeta'), 'w', encoding='utf-8') as fh:
        json.dump({'animation': {'frametime': frametime}, 'tessera': {'flow_sides': 'middle'}}, fh)


def fluids():
    # water is drawn in grey and tinted blue by the game
    flow_strip('water_flow', (200, 200, 200), (225, 225, 225), (110, 110, 110), 2)
    flow_strip('lava_flow', (214, 96, 20), (235, 130, 40), (120, 20, 10), 3)


def powder_snow():
    """Powder snow apart from snow at a glance: pale, with a dashed blue edge on every face."""
    import random
    rnd = random.Random('powder_snow')
    img = Image.new('RGBA', (16, 16))
    for y in range(16):
        for x in range(16):
            v = rnd.randint(236, 252)
            img.putpixel((x, y), (v - 8, v, 255, 255) if rnd.random() < 0.18 else (v, v, v, 255))
    edge = (90, 170, 230, 255)
    for i in range(16):
        if (i // 2) % 2 == 0:
            for x, y in ((i, 0), (i, 15), (0, i), (15, i)):
                img.putpixel((x, y), edge)
    for x, y in ((7, 7), (8, 8), (7, 8), (8, 7), (5, 5), (10, 10), (5, 10), (10, 5)):  # a small flake
        img.putpixel((x, y), edge)
    img.save(path('textures', 'block', 'powder_snow.png'))


# ---------------------------------------------------------------------------
def main():
    shutil.rmtree(ROOT, ignore_errors=True)
    redstone_wire()
    plates()
    daylight_detector()
    note_block()
    target()
    repeater()
    comparator()
    composter()
    respawn_anchor()
    beehives()
    cauldron()
    jukebox()
    pistons()
    observer()
    dispensers()
    hopper()
    buttons()
    books()
    potions()
    fluids()
    powder_snow()
    scaffolding()
    # the 3D pack (seamless glass, 3D crafting table and lantern); its enchanted books are
    # dropped, books() already covers every enchantment and level.
    subprocess.run([sys.executable, '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'make_showcase_pack.py')],
                   env=dict(os.environ, TESSERA_SHOWCASE_ROOT=ROOT), check=True)
    with open(os.path.join(ROOT, 'manifest.json'), 'w', encoding='utf-8') as f:
        json.dump({'format_version': 2,
                   'header': {'name': 'Tessera Redstone Info + 3D',
                              'description': 'Redstone power, note pitch, levels and enchantment levels as pixel digits; seamless glass, 3D crafting table and lantern (needs the Tessera mod)',
                              'uuid': '3d1c9a8e-6b24-4f7d-9e5a-2c8b1f0a7d31', 'version': [1, 0, 0], 'min_engine_version': [1, 21, 0]},
                   'modules': [{'type': 'resources', 'uuid': 'a7e2c4b9-1d3f-4a68-b5c0-9e8d7f6a5b42', 'version': [1, 0, 0]}]},
                  f, indent=2)
    icon = Image.new('RGBA', (64, 64), (30, 32, 40, 255))
    big = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    draw_text(big, '15', 4, 6, RED)
    icon.alpha_composite(big.resize((64, 64), Image.NEAREST))
    icon.save(os.path.join(ROOT, 'pack_icon.png'))
    n = sum(len(fs) for _, _, fs in os.walk(ROOT))
    print('written', os.path.abspath(ROOT), n, 'files')


main()
