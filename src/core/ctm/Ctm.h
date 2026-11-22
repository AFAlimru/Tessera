// Tessera - connected textures in the OptiFine CTM format (platform independent).
//
// Pack authors write optifine/ctm/**.properties files exactly as for Java (OptiFine,
// Continuity). This part parses them and decides, from which neighbours a face
// connects to, which tile each part of the face shows. The game layer finds the
// neighbours and draws the result.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tessera::ctm {

enum class Method : uint8_t {
    Ctm,                // 47 tiles, 8-way
    Compact,            // ctm_compact: 5 tiles, each quarter of the face picks one
    Horizontal,         // 4 tiles
    Vertical,           // 4 tiles
    HorizontalVertical, // horizontal+vertical
    VerticalHorizontal, // vertical+horizontal
    Top,                // 1 tile, connects to the block above
    Random,
    Repeat,
    Fixed,
    Overlay,
    OverlayCtm,
    OverlayRandom,
    OverlayRepeat,
    OverlayFixed,
};

enum class Connect : uint8_t { Block, Tile, State };
enum class Symmetry : uint8_t { None, Opposite, All };

// Face directions, in the order Bedrock numbers them.
enum class Dir : uint8_t { Down, Up, North, South, West, East };
constexpr uint8_t dirBit(Dir d) { return uint8_t(1u << uint8_t(d)); }
constexpr uint8_t kAllDirs = 0x3F;

struct Tile {
    enum class Kind : uint8_t { Image, Skip, Default } kind = Kind::Image;
    std::string path; // pack path of the PNG, e.g. "assets/minecraft/optifine/ctm/glass/0.png"
};

// "[namespace:]name[:prop=v1,v2[:prop2=...]]" (Java names and properties).
struct BlockMatch {
    std::string                                                   name; // no namespace
    std::vector<std::pair<std::string, std::vector<std::string>>> props;
};

struct Rule {
    std::string             file; // path of the .properties file
    Method                  method = Method::Fixed;
    std::vector<Tile>       tiles;
    std::vector<BlockMatch> matchBlocks;
    std::vector<std::string> matchTiles; // texture names ("glass", "block/glass", "optifine/ctm/x/1")
    Connect                 connect = Connect::Block;
    uint8_t                 faces   = kAllDirs;
    int                     weight  = 0;
    bool                    innerSeams = false;
    int                     width = 0, height = 0; // repeat
    std::vector<int>        weights;               // random
    Symmetry                symmetry    = Symmetry::None;
    int                     randomLoops = 0;
    bool                    linked      = false;
    std::vector<std::pair<int, int>> compactMap;   // ctm_compact "ctm.<case>=<tile>"
    std::vector<BlockMatch> connectBlocks;          // overlay
    std::vector<std::string> connectTiles;          // overlay
    int                     tintIndex = -1;
    std::string             tintBlock;
    std::vector<std::pair<int, int>> heights;       // inclusive ranges
    std::vector<std::string> biomes;
    std::vector<std::string> warnings;
};

// Parses one .properties file. `path` is its location in the pack (it decides the
// defaults: <name>.properties -> matchTiles=<name>, block_<name>.properties ->
// matchBlocks=<name>, and where relative tile names are looked up).
std::optional<Rule> parse(std::string const& path, std::string const& text, std::string& error);

std::optional<Method> methodFromName(std::string const& name);
char const*           methodName(Method m);

// Neighbours as seen on the face, in the face's own image space (u to the right,
// v downwards, as the tile is drawn).
enum Neighbour : uint8_t {
    Left      = 1 << 0,
    Right     = 1 << 1,
    Up        = 1 << 2,
    Down      = 1 << 3,
    UpLeft    = 1 << 4,
    UpRight   = 1 << 5,
    DownLeft  = 1 << 6,
    DownRight = 1 << 7,
};

// ctm_compact: the tile (0..4) each quarter shows, in the order top-left, top-right,
// bottom-left, bottom-right. Tile 0: no connection (border all round), 1: connected
// everywhere, 2: connected up/down only (borders left and right), 3: connected
// left/right only (borders top and bottom), 4: connected on both sides but not across
// the corner (only the corner pixel left).
std::array<int, 4> compactQuarters(uint8_t neighbours);

// Bilinear corner order of a quarter: index in compactQuarters' order -> its (u, v)
// range within the face, 0..1.
struct Range {
    float u0, v0, u1, v1;
};
constexpr std::array<Range, 4> kQuarters = {{{0, 0, .5f, .5f}, {.5f, 0, 1, .5f}, {0, .5f, .5f, 1}, {.5f, .5f, 1, 1}}};

// overlay: the tiles (indices into the 17) to draw over a face, from which neighbours
// are "connect" blocks. Edges: 15 top, 1 bottom, 9 left, 7 right; two adjacent edges
// 11 (top+left), 10 (top+right), 4 (bottom+left), 3 (bottom+right); three edges 13
// (all but bottom), 5 (all but top), 6 (all but right), 12 (all but left); all four 8;
// a corner whose two edges do not connect: 16 top-left, 14 top-right, 2 bottom-left,
// 0 bottom-right. (Read off the tiles of overlay packs: each draws where its
// neighbour is.)
std::vector<int> overlayTiles(uint8_t neighbours);

// Deterministic per-position pick for random/repeat (same block, same face -> same tile).
int randomTile(Rule const& rule, int x, int y, int z, Dir face);
// repeat: the tile for a face position (cu, cv = the face's cell in image space).
int repeatTile(Rule const& rule, int cu, int cv);

} // namespace tessera::ctm
