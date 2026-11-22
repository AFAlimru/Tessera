// Tessera - connected textures (OptiFine CTM format) in game.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

class Block;
class BlockPos;
class BlockSource;
class ResourcePackManager;
class Tessellator;

namespace ll::io {
class Logger;
}

namespace tessera::game::ctm {

void setLogger(ll::io::Logger* logger);
// Detailed per-quad diagnostics (glass panes) go here, flushed per line.
void setDiagFile(std::filesystem::path const& path);

struct Sprite {
    std::string name; // terrain atlas texture name
    std::string path; // sprite path the atlas loads (served from memory)
    std::string png;
};

// Atlas build: reads assets/minecraft/optifine/ctm/**.properties from the pack stack
// and returns the tile images to add to the terrain atlas.
std::vector<Sprite> collect(ResourcePackManager& packs);

// After block textures are (re)loaded: looks up where tiles and matched textures
// ended up in the atlas.
void resolve();

// True when any rule is loaded (the tessellation hooks then call apply for every block).
bool active();

// Chunk build: rewrites the quads [begin, end) the tessellator just emitted for the
// block at pos, choosing tiles from the neighbours in region. May append quads (a
// face split into quarters).
void apply(Tessellator& tess, size_t begin, size_t end, Block const& block, BlockPos const& pos, BlockSource& region);
// P5 blocks: moves the block's type to the alpha-tested render layer when it draws blended
// (sorted) or opaque with see-through texture pixels (once; undone when rules reload).
void ensureGeometryLayer(Block const& block, bool cutout, bool translucent = false);

} // namespace tessera::game::ctm
