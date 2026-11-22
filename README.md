# Tessera

LeviLamina client mod for Minecraft Bedrock (1.26.60, Windows): resource packs can use the **Java Edition resource pack format** for blocks and items — block states, 3D models, OptiFine connected textures and CIT, 1.21.4 item definitions, labPBR — on top of everything they already do as Bedrock packs.

**Guide** (what it can do, how it works, making packs, tool, API):
[English](docs/guide/en.md) · [简体中文](docs/guide/zh-Hans.md) · [繁體中文](docs/guide/zh-Hant.md)

More: [API reference](docs/api.md) · [roadmap](docs/roadmap.md) · [research notes](docs/research-report.md)

## Build

```bash
export PATH="/path/to/LLVM-22/bin:$PATH"   # clang-cl from LLVM 22
xmake f -a x64 -m release -p windows -y
xmake build Tessera          # the mod -> bin/Tessera/
xmake build tessera_tool     # the pack tool -> build/windows/x64/release/tessera-tool.exe
```

## Low-level rules (`state_textures.json`)

Besides Java-format assets, a pack can also give Tessera rules directly in `state_textures.json` at its root (Bedrock state names, texture names from `terrain_texture.json`):

```json
{
  "format_version": 1,
  "rules": [
    {
      "block": "redstone_wire",
      "when": { "redstone_signal": "1..15" },
      "on": ["north", "south", "east", "west"],
      "replace": { "redstone_dust_line": "my_dust_climb_p{redstone_signal}" },
      "tint": "none"
    }
  ]
}
```

| Field | Meaning |
| --- | --- |
| `block` | Block name or list (`minecraft:` optional) |
| `when` | State conditions: number, `true`/`false`, string, range `"a..b"`, or a list of these; computed states (`tessera:…`) and other mods' keys too |
| `on` | Where the quad sits in the block cell: `bottom`, `top`, `north`, `south`, `east`, `west`, `side`, `center` |
| `replace` | Source texture → target texture (`name#1` picks an array entry, `{state}` is replaced by the value); `"*"` / `"*:<face>"` replace by face position |
| `rotation` | `{ "x": 90, "y": 180 }`: Java-style rotation for `"*:<face>"` keys |
| `tint` | `keep` (default), `none`, or `#RRGGBB` |

The first matching rule wins for each quad. Details: [docs/api.md](docs/api.md).
