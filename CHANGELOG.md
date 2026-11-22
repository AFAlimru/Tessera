# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-11-22

First preview release. Guide: [English](docs/guide/en.md) · [简体中文](docs/guide/zh-Hans.md) · [繁體中文](docs/guide/zh-Hant.md)

### Added

- Java-format resource packs inside Bedrock packs (`assets/minecraft/...`), read through the game's own pack stack:
  - block states (`variants`, `multipart`, rotation, weighted models, merged across packs) and 3D block models with elements, also in the hand and in inventory slots;
  - states Bedrock does not keep, worked out as Java does: note block note/instrument, jukebox, comparator output, piston extended, redstone dust / tripwire / glass pane connections, door halves, leaves distance, estimated power and signal;
  - OptiFine connected textures (`ctm_compact`, `fixed`, `random`, `repeat`, `top`, `overlay`, `overlay_fixed`, `overlay_random`, `overlay_repeat`), also on 3D model faces;
  - OptiFine CIT (`type=item`) and 1.21.4 item model definitions, in every slot, on the cursor and in the hand; 3D held, dropped and framed items;
  - labPBR maps served as texture sets (Vibrant Visuals, shader mods);
  - Java water and lava textures;
  - a pack's own Bedrock textures are used for Java model textures it has no Java copy of.
- Vanilla Java models and textures are read from the player's own Java client jar (never copied).
- `F3+R` reloads resource packs inside a world (experimental).
- C API for other mods (`include/Tessera/Api.h`): per-position state values, state providers, item 3D models.
- `tessera-tool` (separate download): `init`, `check` (HTML report), `preview` (blocks by state, connected-texture walls, item icons), `pack` (.mcpack); English, 简体中文, 繁體中文.

### Known limits

- Fences, walls, iron bars and chorus plants keep Bedrock's mesh; animated textures show their first frame.
- CTM `ctm` (47 tiles), `horizontal`, `vertical`, `overlay_ctm` and `biomes` are not supported yet.
- Built for Minecraft Bedrock 1.26.60 / LeviLamina 26.60 on Windows.
