# Tessera guide

[繁體中文](zh-Hant.md) · [简体中文](zh-Hans.md) · English

Tessera is a client mod for Minecraft Bedrock (Windows, LeviLamina) that lets a resource pack use the **Java Edition resource pack format** for blocks and items: block states, 3D models, connected textures, custom item textures and more. A pack keeps everything it has as a Bedrock pack, and adds Java-format files that Tessera reads.

Contents: [What it can do](#what-it-can-do) · [How it works](#how-it-works) · [Making a pack](#making-a-pack) · [tessera-tool](#tessera-tool) · [API for mods](#api-for-mods) · [Limits](#limits)

## What it can do

### Blocks
- **Models by block state.** `assets/minecraft/blockstates/*.json` exactly as in Java: `variants`, `multipart`, `x`/`y` rotation, weighted random models (`uvlock` is not supported yet). When several packs hold the same blockstate file, they merge state by state, as Java does.
- **3D models.** Java block models with `elements` (Blockbench "Java Block/Item" models), element rotation and `rescale`, `cullface`, `shade`, `tintindex` (grass, leaves, redstone), see-through and cut-out textures.
- **Same models in hand and in slots.** A block's Java item model is drawn as a real 3D block in the hand and in inventory slots (posed by its `display.gui`).
- **States Bedrock does not keep.** Tessera works them out on the client the way Java does, so blockstates written for Java just work:
  - note block `note` / `instrument`, jukebox `has_record`, comparator output, piston `extended`;
  - redstone dust and tripwire connections, glass pane connections;
  - door halves (`open` / `hinge` on both halves), leaves `distance` (decay display);
  - an estimated `powered` / `power` for any block, and `signal` for targets and sculk sensors.
- **Connected textures** (OptiFine CTM format, `optifine/ctm/**.properties`): `ctm_compact`, `fixed`, `random`, `repeat`, `top`, `overlay`, `overlay_fixed`, `overlay_random`, `overlay_repeat`, with `matchTiles` / `matchBlocks`, `connect`, `faces`, `heights`, `innerSeams`, `tintIndex` / `tintBlock`. They also apply to the faces of 3D models (seamless glass panes).
- **Fluids.** Java water and lava textures replace Bedrock's.

### Items
- **1.21.4+ item model definitions** (`assets/minecraft/items/*.json`): `model`, `composite`, `condition`, `select`, `range_dispatch`, with properties such as `display_context`, `damage`, `count`, `custom_name`, `component` (potion contents, enchantments, stored enchantments, suspicious stew).
- **OptiFine CIT** (`optifine/cit/**.properties`, `type=item`): `items`, `damage`, `stackSize`, `enchantmentIDs` / `enchantmentLevels`, `nbt.display.Name` (exact, pattern, regex), potions.
- Icons work everywhere: hotbar, inventory, containers, the stack on the mouse cursor.
- **3D held items.** An item model with `elements` is held, dropped and framed as a real 3D model.

### Shaders and PBR
- **labPBR maps** (`x_n.png`, `x_s.png`) of every texture Tessera draws are served as Bedrock texture sets, so Vibrant Visuals and shader mods (e.g. Arke) see normals, roughness, metalness and emission.

### Bedrock packs with Tessera effects
- Every Bedrock file of the pack keeps working as usual.
- A Java model that names a texture the pack only has as a Bedrock texture (`block/stone` → `textures/blocks/stone.png`) uses that picture. You don't have to copy your textures into the Java folders.

### For players and authors
- **F3+R** reloads resource packs without leaving the world (experimental).
- **tessera-tool** checks, previews and packages packs without the game ([below](#tessera-tool)).

## How it works

1. **Reading.** Tessera reads the Java files through the game's own resource loader, so it sees exactly the packs active in Global Resources, in their order. Vanilla Java models and textures a pack relies on come from the player's own Java client jar (`%APPDATA%\.minecraft\versions`, read only, never copied).
2. **Baking.** For every block a pack touches, every Java block state is matched to the Bedrock block and states it corresponds to (Chunker's Java-to-Bedrock table). Each state's models are "baked":
   - a plain cube becomes six face pictures;
   - anything else becomes a list of quads with their own pictures.
   The pictures are added to the game's terrain atlas. The result is cached in `mods/Tessera/data/java_bake.cache` and reused until a file it read changes.
3. **Drawing.** When the game builds a chunk, Tessera hooks the block tessellator. For each block it finds the rule for its current state (and computed states), then either moves the face UVs to the baked pictures or folds the block's own quads away and draws the baked quads, lit like the block's own faces. Connected textures are decided at the same moment from the neighbours.
4. **Computed states.** Values the client cannot read while drawing (block entities, leaves distance) are read on the main thread into a snapshot. When a value changes, its chunk is rebuilt.
5. **Items.** Item icons are swapped where the game asks for an item's icon. Held 3D items replace the mesh the game builds from the icon.

Nothing is converted on disk: the pack stays a normal Java-format pack inside a Bedrock pack.

## Making a pack

The recommended way: **start from your Bedrock pack and add Java-format files for what Bedrock cannot do.** Make those files with the Java tools you already know. Tessera reads them as they are.

### 1. Prepare the folders
```
tessera-tool init MyPack
```
This adds the folders below to a Bedrock pack (one with `manifest.json`) and writes `assets/TESSERA.txt`:
```
MyPack/
  manifest.json, pack_icon.png, textures/...     ← your Bedrock pack, unchanged
  assets/minecraft/
    blockstates/       block state → model (Java format)
    models/block/      block models (Blockbench: "Java Block/Item")
    models/item/       item models
    items/             1.21.4+ item model definitions
    textures/block/    textures (+ _n / _s labPBR maps, .png.mcmeta)
    textures/item/
    optifine/ctm/      connected textures
    optifine/cit/      custom item textures
```

### 2. Create with Java tools
- **Models:** Blockbench, "Java Block/Item" format. Export into `models/block` or `models/item`; texture references are Java names (`block/oak_planks`, `block/my_texture`).
- **Block states:** write `blockstates/<block>.json` as for Java (Java block names, e.g. `note_block`, `redstone_wire`).
- **Textures:** any image editor. A texture you already have on the Bedrock side does not need a Java copy.
- **Connected textures, CIT:** OptiFine's format (the [OptiFine docs](https://github.com/sp614x/optifine/tree/master/OptiFineDoc/doc) or the tools that generate it).
- **PBR:** labPBR `_n` / `_s` maps next to the texture.

Which picture a model gets for `block/x`, first found wins:
1. the Java texture `assets/minecraft/textures/block/x.png` in any active pack;
2. the pack's Bedrock texture of the same block (`textures/blocks/…`, Bedrock names: `block/oak_planks` → `planks_oak`);
3. the vanilla Java texture from the player's jar;
4. Bedrock's built-in texture.

### 3. Check and preview
```
tessera-tool check MyPack       → tessera-check/report.html
tessera-tool preview MyPack     → tessera-preview/preview.html
```
The report lists what will not work and why. The preview draws every block state, the connected-texture walls and the item icons as Tessera will show them.

### 4. Try it in game
Copy the pack folder into `resource_packs`, turn it on in Global Resources, and join a world. After changing files, press **F3+R** in the world to reload.

### 5. Publish
```
tessera-tool pack MyPack -o MyPack.mcpack
```
Players need the Tessera mod for the Java-format part; without it they still get your Bedrock pack.

Only publish your own work. Don't ship vanilla Java assets or other people's packs.

A drawing tool of our own may come later. Until then, the Java ecosystem's tools are the editor.

## tessera-tool

Build it with `xmake build tessera_tool` → `build/windows/x64/release/tessera-tool.exe`. Every command takes `--lang en | zh-Hans | zh-Hant` (default: the system language).

| Command | Does |
|---|---|
| `init <pack dir>` | Adds the Java-format folders and `assets/TESSERA.txt` to a Bedrock pack. |
| `check <pack> [-o dir] [--vanilla jar\|off]` | Writes `report.html`: rules per block, unknown blocks, missing blockstates / models / parents / textures, blocks Tessera leaves alone, textures taken from the Bedrock side, animated textures, CTM / CIT / item definition problems, files Tessera does not read. Exit code 1 when there are errors. |
| `preview <pack> [-o dir] [--block name] [--max n]` | Writes `preview.html`: each block state from the south-east (identical looks once), one sample wall per connected-texture rule, item icons. |
| `pack <pack dir> [-o file.mcpack] [--name name]` | Zips the pack. Adds `manifest.json` (new UUIDs, also saved next to the output: keep it in the pack folder) and `pack_icon.png` (from `pack.png`) when missing. |

`<pack>` can be a folder, `.zip` or `.mcpack`.

## API for mods

Header-only, no link dependency: `include/Tessera/Api.h` finds `Tessera.dll` at runtime. Every call returns false (or 0) when Tessera is not installed. Full reference (Traditional Chinese): [docs/api.md](../api.md).

**State values** (API 1). Give any block position a value that pack rules can test:
```cpp
#include "Tessera/Api.h"
tessera::api::Client tessera;
tessera.setInt(0, x, y, z, "mymod:charge", 3);   // dim: 0 overworld, 1 nether, 2 end
tessera.setBool(0, x, y, z, "mymod:active", true);
tessera.setString(0, x, y, z, "mymod:mode", "fast");
tessera.clear(0, x, y, z, "mymod:charge");
```

**Providers** (API 2). Tessera asks your function while drawing blocks whose rules read the key:
```cpp
bool charge(int dim, int x, int y, int z, char const* block, void* user, tessera_value* out) {
    out->kind = TESSERA_VALUE_INT;
    out->i    = myCharge(dim, x, y, z);   // your own data only: runs on chunk-build threads
    return true;
}
tessera.registerProvider("mymod:charge", &charge);
tessera.markDirty(dim, x, y, z);           // the answer for this position changed
```

**Item models** (API 3). The 3D model a pack gives an item, for mods that draw items themselves (dropped-item physics, displays):
```cpp
std::vector<tessera_quad> quads(4096);
int n = tessera.itemModel(&itemStack, quads.data(), int(quads.size()));
// n == 0: no 3D model. Quads are in block units on Java's axes (x east, y up, z south),
// cut per texel, one RGBA colour each; cross(p1 - p0, p3 - p0) faces out.
```

Keys must have a namespace; `tessera:` and `minecraft:` are reserved. All calls are thread-safe. C exports: `tessera_api_version` (3), `tessera_set_state_int/bool/string`, `tessera_clear_state`, `tessera_register_provider`, `tessera_unregister_provider`, `tessera_mark_dirty`, `tessera_item_model`.

## Limits

- Fences, walls, iron bars and chorus plants keep Bedrock's mesh. Their shape depends on neighbours in ways Tessera does not compute yet; connected textures still apply.
- Animated textures show their first frame (water and lava use Bedrock's animation).
- Entities, GUI, particles, sky, fonts, sounds and CEM are Java files Tessera does not read; use the Bedrock side of the pack for those.
- CTM methods `ctm` (47 tiles), `horizontal`, `vertical` and their combinations, `overlay_ctm`, and `biomes` are not supported yet.
- Thrown potions are drawn by the game as flat sprites.
- Windows only for now; Android later.
