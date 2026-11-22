#include "game/Ctm.h"
#include "game/Engine.h"
#include "game/JavaPacks.h"

#include "core/Remap.h"
#include "core/ctm/Ctm.h"
#include "core/java/JavaPack.h"

#include "ll/api/io/Logger.h"

#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/client/world/level/biome/biome_color_sampling/GrassTessellationPolicy.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/file/Path.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/minecraft_renderer/renderer/MeshData.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/resources/PackInstance.h"
#include "mc/resources/ResourcePack.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"
#include "mc/deps/core/string/HashedString.h"

#include <algorithm>
#include <optional>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

namespace tessera::game::ctm {
namespace core = tessera::ctm;
namespace {

ll::io::Logger* gLog = nullptr;

constexpr std::string_view kCtmDir    = "assets/minecraft/optifine/ctm/";
constexpr std::string_view kSpriteDir = "textures/tessera_java/"; // served by JavaPacks' load hooks

uint64_t fnv(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

ResourceLocation userPath(std::string const& path) { return ResourceLocation{Core::PathView{path}, ResourceFileSystem::UserPackage}; }

std::optional<std::string> readPack(ResourcePackManager& packs, std::string const& path) {
    std::string out;
    try {
        if (static_cast<ResourceLoader const&>(packs).load(userPath(path), out)) return out;
    } catch (...) {}
    return std::nullopt;
}

// Loaded rules waiting for the atlas (tile path -> atlas texture name).
struct Loaded {
    std::vector<core::Rule>            rules;
    std::map<std::string, std::string> tileNames;
};
std::mutex              gLoadMutex;
std::shared_ptr<Loaded> gLoaded;

// ---------------------------------------------------------------------------
// Runtime (after resolve)
// ---------------------------------------------------------------------------
struct Live {
    core::Rule const*     rule = nullptr;
    int                   order = 0;          // file order (tile rules first)
    std::vector<UvRect>   tiles;              // invalid rect: <skip>/<default>/missing
    std::vector<UvRect>   sources;            // matchTiles, in the atlas
    std::set<std::string> blocks;             // matchBlocks, Bedrock names
    std::set<std::string> connectBlocks;      // overlay: neighbours that get drawn onto the face
    std::vector<UvRect>   connectTiles;
    // Java block states on matchBlocks / connectBlocks entries (Bedrock name -> entries); a
    // name listed without states is in the *Any set and matches in every state.
    std::map<std::string, std::vector<core::BlockMatch const*>> matchStates, connectStates;
    std::set<std::string>                                       matchAny, connectAny;
    bool                  overlay = false;
};

struct Snapshot {
    std::shared_ptr<Loaded const>                    loaded;
    std::vector<Live>                                live;
    std::vector<int>                                 byTile;  // rules with sources
    std::unordered_map<std::string, std::vector<int>> byBlock; // Bedrock name -> rules
    bool                                             anyOverlay = false;
};
std::mutex                      gSnapMutex;
std::shared_ptr<Snapshot const> gSnap;
std::atomic<bool>               gActive{false};
std::atomic<uint64_t>           gGeneration{1};

std::shared_ptr<Snapshot const> snapshot() {
    std::lock_guard lock(gSnapMutex);
    return gSnap;
}

UvRect rectOf(std::string const& name) {
    try {
        auto const set = BlockGraphics::getTextureUVCoordinateSet(name, 0, 0);
        UvRect     r{set._u0, set._v0, set._u1, set._v1};
        if (r.u0 > r.u1) std::swap(r.u0, r.u1);
        if (r.v0 > r.v1) std::swap(r.v0, r.v1);
        return r;
    } catch (...) {
        return {};
    }
}

// The atlas's "missing texture" rect, so unknown names are not taken as real.
UvRect missingRect() { return rectOf("tessera_definitely_not_a_texture"); }

std::vector<std::string> bedrockNamesOf(core::BlockMatch const& m) {
    std::set<std::string> out;
    auto const&           table = java::mappings();
    auto                  it    = table.find(m.name);
    if (it == table.end()) return {};
    for (auto const& [props, row] : it->second) {
        // Keep rows whose Java properties satisfy the match's property filters.
        bool ok = true;
        for (auto const& [key, values] : m.props) {
            std::string const needle = key + "=";
            auto              at     = props.find(needle);
            if (at != std::string::npos && at != 0 && props[at - 1] != ',') at = std::string::npos;
            if (at == std::string::npos) continue; // the table row does not mention it
            auto const end   = props.find(',', at);
            auto const value = props.substr(at + needle.size(), end == std::string::npos ? std::string::npos : end - at - needle.size());
            if (std::find(values.begin(), values.end(), value) == values.end()) ok = false;
        }
        if (ok && !row.name.empty()) out.insert(row.name);
    }
    return {out.begin(), out.end()};
}

// A Java texture named after its block ("oak_log", "oak_log_top", "furnace_side"):
// the texture the Bedrock block shows on that face. Covers textures whose Bedrock
// names differ (logs keep their sides under one shared, variant-indexed name).
UvRect blockFaceRect(std::string const& javaTexture) {
    struct Try {
        std::string_view suffix;
        int              face;
    };
    static constexpr Try tries[] = {{"_top", 1}, {"_bottom", 0}, {"_side", 2}, {"_front", 2}, {"", 2}};
    for (auto const& t : tries) {
        if (!std::string_view(javaTexture).ends_with(t.suffix)) continue;
        std::string const block = javaTexture.substr(0, javaTexture.size() - t.suffix.size());
        auto const&       table = java::mappings();
        auto              it    = table.find(block);
        if (it == table.end() || it->second.empty()) continue;
        std::string bedrock = it->second.begin()->second.name;
        for (auto const& [props, row] : it->second) // prefer the default-looking row
            if (props.find("axis=y") != std::string::npos) bedrock = row.name;
        try {
            Block const& b = BlockTypeRegistry::get().getDefaultBlockState(HashedString("minecraft:" + bedrock), false);
            auto const*  g = BlockGraphics::getForBlock(b);
            if (!g) continue;
            auto const& set = g->getTexture(uint64(t.face), 0);
            UvRect      r{set._u0, set._v0, set._u1, set._v1};
            if (r.u0 > r.u1) std::swap(r.u0, r.u1);
            if (r.v0 > r.v1) std::swap(r.v0, r.v1);
            if (r.valid()) return r;
        } catch (...) {}
    }
    return {};
}

// ---------------------------------------------------------------------------
// Per-thread caches
// ---------------------------------------------------------------------------
struct ThreadCache {
    uint64_t                                         generation = 0;
    std::unordered_map<Block const*, std::string>    names;
};
thread_local ThreadCache tCache;

std::string const& nameOf(Block const& block) {
    auto [it, inserted] = tCache.names.try_emplace(&block);
    if (inserted) {
        try {
            auto const& nbt = block.mSerializationId.get();
            if (nbt.contains("name") && nbt.at("name").hold(Tag::String)) it->second = nbt.at("name").get<StringTag>();
            if (it->second.starts_with("minecraft:")) it->second = it->second.substr(10);
        } catch (...) {}
    }
    return it->second;
}

UvRect faceTexture(Block const& block, int face) {
    try {
        auto const* g = BlockGraphics::getForBlock(block);
        if (!g) return {};
        auto const& set = g->getTexture(uint64(face), 0);
        UvRect      r{set._u0, set._v0, set._u1, set._v1};
        if (r.u0 > r.u1) std::swap(r.u0, r.u1);
        if (r.v0 > r.v1) std::swap(r.v0, r.v1);
        return r;
    } catch (...) {
        return {};
    }
}

// Face direction -> its image axes in the world (u to the right, v upwards on screen),
// matching faceUv's layout, and the outward normal.
struct Axes {
    int right[3], up[3], normal[3];
};
Axes axesOf(core::Dir d) {
    switch (d) {
    case core::Dir::Up: return {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    case core::Dir::Down: return {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
    case core::Dir::North: return {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}};
    case core::Dir::South: return {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    case core::Dir::West: return {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}};
    default: return {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}};
    }
}

core::Dir dirOf(Side s) {
    switch (s) {
    case Side::Top: return core::Dir::Up;
    case Side::Bottom: return core::Dir::Down;
    case Side::North: return core::Dir::North;
    case Side::South: return core::Dir::South;
    case Side::West: return core::Dir::West;
    default: return core::Dir::East;
    }
}

bool sameRect(UvRect const& a, UvRect const& b) {
    constexpr float e = 1e-5f;
    return std::abs(a.u0 - b.u0) < e && std::abs(a.v0 - b.v0) < e && std::abs(a.u1 - b.u1) < e && std::abs(a.v1 - b.v1) < e;
}

// Does the block at p count as "the same" for this face?
bool same(Live const& live, Block const& self, UvRect const& source, Block const& other, core::Dir face) {
    switch (live.rule->connect) {
    case core::Connect::State: return &other == &self;
    case core::Connect::Tile:
        return sameRect(faceTexture(other, int(face)), source) || nameOf(other) == nameOf(self);
    default: return nameOf(other) == nameOf(self);
    }
}

bool isPaneLike(std::string const& name) { return name.ends_with("glass_pane") || name == "iron_bars"; }

uint8_t neighbours(Live const& live, Block const& self, UvRect const& source, BlockPos const& pos, core::Dir face,
                   BlockSource& region) {
    Axes const a = axesOf(face);
    struct Off {
        int     du, dv;
        uint8_t bit;
    };
    static constexpr Off offs[8] = {
        {-1, 0, core::Left},    {1, 0, core::Right},   {0, 1, core::Up},       {0, -1, core::Down},
        {-1, 1, core::UpLeft},  {1, 1, core::UpRight}, {-1, -1, core::DownLeft}, {1, -1, core::DownRight},
    };
    bool const thin = isPaneLike(nameOf(self));
    uint8_t    bits = 0;
    for (auto const& o : offs) {
        BlockPos const np{pos.x + a.right[0] * o.du + a.up[0] * o.dv, pos.y + a.right[1] * o.du + a.up[1] * o.dv,
                          pos.z + a.right[2] * o.du + a.up[2] * o.dv};
        Block const& nb = region.getBlock(np);
        if (!same(live, self, source, nb, face)) continue;
        if (!live.rule->innerSeams && !thin) {
            // The neighbour's face is hidden by a matching block in front of it. Not for
            // thin blocks: at a pane wall's corner the "block in front" is the other wing.
            BlockPos const front{np.x + a.normal[0], np.y + a.normal[1], np.z + a.normal[2]};
            if (same(live, self, source, region.getBlock(front), face)) continue;
        }
        bits |= o.bit;
    }
    return bits;
}

// ---------------------------------------------------------------------------
// Writing quads
// ---------------------------------------------------------------------------
struct Streams {
    mce::MeshData& md;
    size_t         count;
};

// Attributes of the quad at q interpolated at image position (s, t) in 0..1 of its own
// image rectangle; corner k sits at (sk[k], tk[k]) (each 0 or 1).
struct QuadAttr {
    glm::vec3 pos;
    glm::vec2 uv1;
    uint32_t  color;
    bool      hasUv1, hasColor;
};

uint32_t lerpColor(std::array<uint32_t, 4> const& c, std::array<float, 4> const& w) {
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float v = 0;
        for (int k = 0; k < 4; ++k) v += w[size_t(k)] * float((c[size_t(k)] >> shift) & 0xFF);
        out |= uint32_t(std::clamp(int(std::lround(v)), 0, 255)) << shift;
    }
    return out;
}

void writeQuads(Tessellator& tess, size_t q, V3 const& origin, Side facing, std::vector<std::pair<core::Range, UvRect>> const& parts) {
    auto& md        = tess.mMeshData.get();
    auto& positions = md.mPositions.get();
    auto& uv0       = md.mTextureUVs[0].get();
    auto& uv1       = md.mTextureUVs[1].get();
    auto& colors    = md.mColors.get();
    auto& normals   = md.mNormals.get();
    auto& pbr       = md.mPBRTextureIndices.get();

    // The quad's corners in its face image (0..1 of the block face).
    std::array<V2, 4>    img;
    float                iu0 = INFINITY, iv0 = INFINITY, iu1 = -INFINITY, iv1 = -INFINITY;
    for (size_t k = 0; k < 4; ++k) {
        auto const& p = positions[q + k];
        img[k]        = faceUv(facing, {p.x - origin.x, p.y - origin.y, p.z - origin.z});
        iu0 = std::min(iu0, img[k].x), iu1 = std::max(iu1, img[k].x), iv0 = std::min(iv0, img[k].y), iv1 = std::max(iv1, img[k].y);
    }
    if (iu1 - iu0 < 1e-6f || iv1 - iv0 < 1e-6f) return;
    std::array<int, 4> sk, tk;
    for (size_t k = 0; k < 4; ++k) {
        sk[k] = img[k].x > (iu0 + iu1) * 0.5f;
        tk[k] = img[k].y > (iv0 + iv1) * 0.5f;
    }
    std::array<glm::vec3, 4> P;
    std::array<glm::vec2, 4> L;
    std::array<uint32_t, 4>  C{};
    for (size_t k = 0; k < 4; ++k) {
        P[k] = positions[q + k];
        L[k] = uv1.size() > q + k ? uv1[q + k] : glm::vec2{};
        C[k] = colors.size() > q + k ? colors[q + k] : 0xFFFFFFFFu;
    }
    glm::vec4 const normal = normals.size() > q ? normals[q] : glm::vec4{};
    ushort const    pbrIdx = pbr.size() > q ? pbr[q] : ushort(0);

    auto weights = [&](float s, float t) {
        std::array<float, 4> w{};
        for (size_t k = 0; k < 4; ++k) w[k] = (sk[k] ? s : 1 - s) * (tk[k] ? t : 1 - t);
        return w;
    };

    bool first = true;
    for (auto const& [range, rect] : parts) {
        // This part of the face, clipped to the quad.
        float const a0 = std::max(range.u0, iu0), a1 = std::min(range.u1, iu1);
        float const b0 = std::max(range.v0, iv0), b1 = std::min(range.v1, iv1);
        if (a1 - a0 < 1e-6f || b1 - b0 < 1e-6f) continue;
        std::array<glm::vec3, 4> np;
        std::array<glm::vec2, 4> nl;
        std::array<uint32_t, 4>  nc;
        std::array<glm::vec2, 4> nu;
        for (size_t k = 0; k < 4; ++k) {
            float const iu = sk[k] ? a1 : a0, iv = tk[k] ? b1 : b0;
            float const s = (iu - iu0) / (iu1 - iu0), t = (iv - iv0) / (iv1 - iv0);
            auto const  w = weights(s, t);
            glm::vec3   p{0, 0, 0};
            glm::vec2   l{0, 0};
            for (size_t j = 0; j < 4; ++j) p += w[j] * P[j], l += w[j] * L[j];
            np[k] = p;
            nl[k] = l;
            nc[k] = lerpColor(C, w);
            nu[k] = {rect.u0 + iu * (rect.u1 - rect.u0), rect.v0 + iv * (rect.v1 - rect.v0)};
        }
        if (first) { // the original quad becomes the first part
            for (size_t k = 0; k < 4; ++k) {
                positions[q + k] = np[k];
                uv0[q + k]       = nu[k];
                if (uv1.size() > q + k) uv1[q + k] = nl[k];
                if (colors.size() > q + k) colors[q + k] = nc[k];
            }
            first = false;
            continue;
        }
        // Further parts are new quads, emitted through the tessellator so its own
        // bookkeeping (indices, face sorting) stays right.
        auto const& off = tess.mPostTransformOffset.get();
        auto const& sc  = tess.mPostTransformScale.get();
        for (size_t k = 0; k < 4; ++k) {
            tess.mNextUV[0].get() = nu[k];
            if (!uv1.empty()) tess.mNextUV[1].get() = nl[k];
            if (!colors.empty()) tess.mNextColor.get() = nc[k];
            if (!normals.empty()) tess.mNextNormal.get() = normal;
            if (!pbr.empty()) tess.mNextPBRTextureIdx.get() = pbrIdx;
            tess.vertex(np[k].x / sc.x - off.x, np[k].y / sc.y - off.y, np[k].z / sc.z - off.z);
        }
    }
}

// Java states a Bedrock block has no counterpart for, worked out from the world.
// Unknown ones are taken as matching.
bool javaStatesMatch(core::BlockMatch const& m, BlockSource& region, BlockPos const& p) {
    for (auto const& [key, values] : m.props) {
        std::string actual;
        if (key == "snowy") { // grass, podzol, mycelium: snow (or a snow layer) right above
            auto const& above = nameOf(region.getBlock(BlockPos{p.x, p.y + 1, p.z}));
            actual = (above == "snow_layer" || above == "snow" || above == "minecraft:snow_layer" || above == "minecraft:snow")
                       ? "true"
                       : "false";
        } else continue;
        if (std::find(values.begin(), values.end(), actual) == values.end()) return false;
    }
    return true;
}

bool statesOk(std::set<std::string> const& any, std::map<std::string, std::vector<core::BlockMatch const*>> const& states,
              std::string const& name, BlockSource& region, BlockPos const& p) {
    if (any.contains(name)) return true;
    auto it = states.find(name);
    if (it == states.end()) return true; // matched another way (e.g. by texture)
    for (auto const* m : it->second)
        if (javaStatesMatch(*m, region, p)) return true;
    return false;
}

uint8_t overlayNeighbours(Live const& live, Block const& self, BlockPos const& pos, core::Dir face, BlockSource& region) {
    Axes const a = axesOf(face);
    struct Off {
        int     du, dv;
        uint8_t bit;
    };
    static constexpr Off offs[8] = {
        {-1, 0, core::Left},    {1, 0, core::Right},   {0, 1, core::Up},       {0, -1, core::Down},
        {-1, 1, core::UpLeft},  {1, 1, core::UpRight}, {-1, -1, core::DownLeft}, {1, -1, core::DownRight},
    };
    uint8_t bits = 0;
    for (auto const& o : offs) {
        BlockPos const np{pos.x + a.right[0] * o.du + a.up[0] * o.dv, pos.y + a.right[1] * o.du + a.up[1] * o.dv,
                          pos.z + a.right[2] * o.du + a.up[2] * o.dv};
        Block const& nb = region.getBlock(np);
        if (&nb == &self) continue;
        bool hit = live.connectBlocks.contains(nameOf(nb))
                && statesOk(live.connectAny, live.connectStates, nameOf(nb), region, np);
        if (!hit && !live.connectTiles.empty()) {
            UvRect const t = faceTexture(nb, int(face));
            for (auto const& r : live.connectTiles)
                if (sameRect(r, t)) hit = true;
        }
        if (!hit && live.connectBlocks.empty() && live.connectTiles.empty())
            hit = nameOf(nb) != nameOf(self) && nameOf(nb) != "air";
        if (hit) bits |= o.bit;
    }
    return bits;
}

// Overlays draw with transparent pixels: blocks that get them move to an alpha-tested
// render layer (their own pixels are opaque, so they look the same).
std::mutex                                       gLayerMutex;
std::unordered_map<BlockType*, BlockRenderLayer> gOriginalLayers;

// Grass blocks: their texture's alpha is the biome tint mask (opaque layer, nothing is see-
// through), so they cannot move to an alpha-tested layer nor take see-through overlays.
bool tintMasked(Block const& block) {
    auto const& n = nameOf(block);
    return n == "grass_block" || n == "grass"
        || BlockRenderLayer(block.getBlockType().mRenderLayer) == BlockRenderLayer::RenderlayerSeasonsOpaque;
}

void ensureCutout(Block const& block) {
    auto& type = const_cast<BlockType&>(block.getBlockType());
    auto  now  = BlockRenderLayer(type.mRenderLayer);
    if (now != BlockRenderLayer::RenderlayerOpaque || tintMasked(block)) return;
    {
        std::lock_guard lock(gLayerMutex);
        gOriginalLayers.try_emplace(&type, now);
    }
    type.mRenderLayer = BlockRenderLayer::RenderlayerAlphatestSingleSide;
    game::requestChunkRebuild();
    if (gLog) gLog->info("CTM: {} now draws in the alpha-tested layer (overlays)", nameOf(block));
}

void restoreLayers() {
    std::lock_guard lock(gLayerMutex);
    for (auto const& [type, layer] : gOriginalLayers) type->mRenderLayer = layer;
    gOriginalLayers.clear();
}

// Multiplies a mesh colour (ABGR: red in the low byte) by a tint.
uint32_t tinted(uint32_t c, mce::Color const& t) {
    auto ch = [&](int shift, float f) {
        float v = float((c >> shift) & 0xFF) * std::clamp(f, 0.f, 1.f);
        return uint32_t(std::clamp(v + 0.5f, 0.f, 255.f)) << shift;
    };
    return (c & 0xFF000000u) | ch(0, t.r) | ch(8, t.g) | ch(16, t.b);
}

// The biome colour an overlay asks for with tintBlock/tintIndex (OptiFine): grass for now.
std::optional<mce::Color> overlayTint(core::Rule const& r, Block const& block, BlockSource& region, BlockPos const& pos) {
    if (r.tintIndex < 0 || r.tintBlock.empty()) return std::nullopt;
    auto const& b = r.tintBlock;
    if (b != "grass_block" && b != "grass" && b != "short_grass" && b != "tall_grass" && b != "fern" && b != "large_fern")
        return std::nullopt;
    // The policy keeps no state; its colour comes from the biomes around pos.
    alignas(16) std::byte policy[64]{};
    return reinterpret_cast<BiomeColorSampling::GrassTessellationPolicy const*>(policy)->$get(block, region, pos, nullptr);
}

// An overlay: the quad again, a hair in front of the face, showing the tile.
void appendOverlay(Tessellator& tess, size_t q, V3 const& origin, Side facing, core::Dir face, UvRect const& tile,
                   std::optional<mce::Color> const& tint = std::nullopt) {
    auto& md        = tess.mMeshData.get();
    auto& positions = md.mPositions.get();
    auto& uv1       = md.mTextureUVs[1].get();
    auto& colors    = md.mColors.get();
    auto& normals   = md.mNormals.get();
    auto& pbr       = md.mPBRTextureIndices.get();
    auto const& off = tess.mPostTransformOffset.get();
    auto const& sc  = tess.mPostTransformScale.get();
    Axes const  a   = axesOf(face);
    constexpr float lift = 0.001f; // blocks
    std::array<glm::vec3, 4> p;
    std::array<glm::vec2, 4> l;
    std::array<uint32_t, 4>  c;
    std::array<glm::vec2, 4> u;
    for (size_t k = 0; k < 4; ++k) {
        p[k]          = positions[q + k];
        l[k]          = uv1.size() > q + k ? uv1[q + k] : glm::vec2{};
        c[k]          = colors.size() > q + k ? colors[q + k] : 0xFFFFFFFFu;
        if (tint) c[k] = tinted(c[k], *tint);
        V2 const img  = faceUv(facing, {p[k].x - origin.x, p[k].y - origin.y, p[k].z - origin.z});
        u[k]          = {tile.u0 + img.x * (tile.u1 - tile.u0), tile.v0 + img.y * (tile.v1 - tile.v0)};
    }
    glm::vec4 const normal = normals.size() > q ? normals[q] : glm::vec4{};
    ushort const    pbrIdx = pbr.size() > q ? pbr[q] : ushort(0);
    for (size_t k = 0; k < 4; ++k) {
        tess.mNextUV[0].get() = u[k];
        if (!uv1.empty()) tess.mNextUV[1].get() = l[k];
        if (!colors.empty()) tess.mNextColor.get() = c[k];
        if (!normals.empty()) tess.mNextNormal.get() = normal;
        if (!pbr.empty()) tess.mNextPBRTextureIdx.get() = pbrIdx;
        tess.vertex(
            p[k].x / sc.x - off.x + a.normal[0] * lift, p[k].y / sc.y - off.y + a.normal[1] * lift,
            p[k].z / sc.z - off.z + a.normal[2] * lift
        );
    }
}


bool heightOk(core::Rule const& r, int y) {
    if (r.heights.empty()) return true;
    for (auto const& [lo, hi] : r.heights)
        if (y >= lo && y <= hi) return true;
    return false;
}

std::atomic<int> gDiag{0};
std::atomic<int> gPaneDiag{0}; // per-quad lines for panes (set by resolve)

// Pane diagnostics go to their own file, flushed per line: the game log can lose its
// tail when the game crashes on exit.
std::mutex            gDiagFileMutex;
std::filesystem::path gDiagPath;
std::ofstream         gDiagFile;

template <class... Args>
void paneLog(std::format_string<Args...> fmt, Args&&... args) {
    std::lock_guard lock(gDiagFileMutex);
    if (!gDiagFile.is_open()) {
        if (gDiagPath.empty()) return;
        gDiagFile.open(gDiagPath, std::ios::out | std::ios::trunc);
    }
    gDiagFile << std::format(fmt, std::forward<Args>(args)...) << "\n";
    gDiagFile.flush();
}

} // namespace

void setLogger(ll::io::Logger* logger) { gLog = logger; }

void setDiagFile(std::filesystem::path const& path) {
    std::lock_guard lock(gDiagFileMutex);
    gDiagPath = path;
}

std::vector<Sprite> collect(ResourcePackManager& packs) {
    std::set<std::string> files;
    try {
        packs.iteratePacks([&](PackInstance const& instance) {
            ResourcePack const& pack = *instance.mPack;
            pack.forEachIn(
                Core::Path{std::string(kCtmDir.substr(0, kCtmDir.size() - 1))},
                [&](Core::Path const& file) {
                    std::string path = file.value;
                    std::replace(path.begin(), path.end(), '\\', '/');
                    auto const at = path.find("optifine/ctm/");
                    if (at == std::string::npos || !path.ends_with(".properties")) return;
                    files.insert(std::string(kCtmDir) + path.substr(at + 13));
                },
                instance.mSubpackIndex, true
            );
        });
    } catch (...) {
        if (gLog) gLog->warn("CTM: listing pack files failed");
    }

    auto loaded = std::make_shared<Loaded>();
    std::vector<Sprite> sprites;
    // OptiFine: files in alphabetical order of their file name.
    std::vector<std::string> ordered(files.begin(), files.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](std::string const& a, std::string const& b) {
        return a.substr(a.rfind('/') + 1) < b.substr(b.rfind('/') + 1);
    });
    int failed = 0;
    for (auto const& path : ordered) {
        auto text = readPack(packs, path);
        if (!text) continue;
        std::string error;
        auto        rule = core::parse(path, *text, error);
        if (!rule) {
            ++failed;
            if (gLog) gLog->warn("CTM: {}: {}", path, error);
            continue;
        }
        for (auto const& w : rule->warnings)
            if (gLog) gLog->warn("CTM: {}: {}", path, w);
        for (auto const& tile : rule->tiles) {
            if (tile.kind != core::Tile::Kind::Image || loaded->tileNames.contains(tile.path)) continue;
            auto png = readPack(packs, tile.path);
            if (!png) {
                if (gLog) gLog->warn("CTM: {}: tile {} not found", path, tile.path);
                continue;
            }
            std::string const name = std::format("tessera_ctm_{:016x}", fnv(tile.path));
            loaded->tileNames[tile.path] = name;
            if (auto img = java::decodePng(*png)) { // keep cut-out edges clean in the mipmaps
                java::bleedTransparent(*img);
                *png = java::encodePng(*img);
            }
            sprites.push_back({name, std::string(kSpriteDir) + name, std::move(*png)});
            // The tile's labPBR maps (x_n.png, x_s.png), for shader mods: served beside it.
            if (tile.path.ends_with(".png"))
                for (char const* suffix : {"_n", "_s"}) {
                    std::string const mapPath = tile.path.substr(0, tile.path.size() - 4) + suffix + ".png";
                    if (auto map = readPack(packs, mapPath))
                        sprites.push_back({name + suffix, std::string(kSpriteDir) + name + suffix, std::move(*map)});
                }
        }
        loaded->rules.push_back(std::move(*rule));
    }
    if (gLog && (!loaded->rules.empty() || failed))
        gLog->info("CTM: {} rule(s), {} tile image(s) from {} file(s)", loaded->rules.size(), sprites.size(), files.size());
    {
        std::lock_guard lock(gLoadMutex);
        gLoaded = loaded->rules.empty() ? nullptr : loaded;
    }
    return sprites;
}

void resolve() {
    std::shared_ptr<Loaded const> loaded;
    {
        std::lock_guard lock(gLoadMutex);
        loaded = gLoaded;
    }
    if (!loaded) {
        restoreLayers();
        std::lock_guard lock(gSnapMutex);
        gSnap = nullptr;
        gActive.store(false);
        return;
    }
    restoreLayers();
    auto       snap    = std::make_shared<Snapshot>();
    snap->loaded       = loaded;
    UvRect const miss  = missingRect();
    auto valid = [&](UvRect const& r) { return r.valid() && !sameRect(r, miss); };
    std::map<std::string, int> methodsSkipped;
    int                        order = 0;
    for (auto const& rule : loaded->rules) {
        switch (rule.method) {
        case core::Method::Compact:
        case core::Method::Fixed:
        case core::Method::Random:
        case core::Method::Repeat:
        case core::Method::Top:
        case core::Method::Overlay:
        case core::Method::OverlayFixed:
        case core::Method::OverlayRandom:
        case core::Method::OverlayRepeat: break;
        default: ++methodsSkipped[core::methodName(rule.method)]; continue;
        }
        Live live;
        live.rule  = &rule;
        live.order = order++;
        for (auto const& tile : rule.tiles) {
            UvRect r{};
            if (tile.kind == core::Tile::Kind::Image)
                if (auto it = loaded->tileNames.find(tile.path); it != loaded->tileNames.end()) r = rectOf(it->second);
            live.tiles.push_back(valid(r) ? r : UvRect{});
        }
        auto texturesOf = [&](std::string const& name, std::vector<UvRect>& out) {
            bool found = false;
            for (auto const& bedrock : java::bedrockTextureNames(name)) {
                UvRect const r = rectOf(bedrock);
                if (valid(r)) {
                    out.push_back(r);
                    found = true;
                    break;
                }
            }
            if (!found)
                if (UvRect const r = blockFaceRect(name); valid(r)) out.push_back(r);
            // Textures a pack's own models use end up in Tessera's baked images.
            for (auto const& baked : javapacks::bakedTexturesUsing(name)) {
                UvRect const r = rectOf(baked);
                if (valid(r)) out.push_back(r);
            }
        };
        // Names that match nothing are tiles of rules from packs not in the stack (OptiFine
        // skips them): only a rule left with no texture at all is worth a warning.
        std::vector<std::string> unknown;
        for (auto const& name : rule.matchTiles) {
            size_t const before = live.sources.size();
            texturesOf(name, live.sources);
            if (live.sources.size() == before) unknown.push_back(name);
        }
        if (!unknown.empty() && live.sources.empty() && gLog)
            gLog->warn("CTM: {}: textures {} not found in Bedrock", rule.file, unknown.front());
        for (auto const& name : rule.connectTiles) texturesOf(name, live.connectTiles);
        for (auto const& m : rule.connectBlocks)
            for (auto const& b : bedrockNamesOf(m)) {
                live.connectBlocks.insert(b);
                if (m.props.empty()) live.connectAny.insert(b);
                else live.connectStates[b].push_back(&m);
            }
        live.overlay = rule.method == core::Method::Overlay || rule.method == core::Method::OverlayFixed
                    || rule.method == core::Method::OverlayRandom || rule.method == core::Method::OverlayRepeat;
        for (auto const& m : rule.matchBlocks)
            for (auto const& b : bedrockNamesOf(m)) {
                live.blocks.insert(b);
                if (m.props.empty()) live.matchAny.insert(b);
                else live.matchStates[b].push_back(&m);
            }
        if (live.sources.empty() && live.blocks.empty()) {
            if (gLog) gLog->warn("CTM: {}: matches nothing in Bedrock", rule.file);
            continue;
        }
        snap->anyOverlay |= live.overlay;
        int const index = int(snap->live.size());
        if (!live.sources.empty()) snap->byTile.push_back(index);
        for (auto const& b : live.blocks) snap->byBlock[b].push_back(index);
        snap->live.push_back(std::move(live));
    }
    if (gLog) {
        gLog->info("CTM: {} rule(s) active ({} by texture, {} block name(s))", snap->live.size(), snap->byTile.size(),
                   snap->byBlock.size());
        for (auto const& [m, n] : methodsSkipped) gLog->warn("CTM: method {} is not supported yet ({} rule(s) left out)", m, n);
    }
    gGeneration.fetch_add(1);
    std::lock_guard lock(gSnapMutex);
    gSnap = snap->live.empty() ? nullptr : std::shared_ptr<Snapshot const>(snap);
    gActive.store(gSnap != nullptr);
    gDiag.store(debugLogging() ? 40 : 0);
    gPaneDiag.store(0); // per-quad pane lines: raise to debug panes again
}

bool active() { return gActive.load(std::memory_order_relaxed); }

void apply(Tessellator& tess, size_t begin, size_t end, Block const& block, BlockPos const& pos, BlockSource& region) {
    auto const snap = snapshot();
    if (!snap) return;
    auto& md        = tess.mMeshData.get();
    auto& positions = md.mPositions.get();
    auto& uv0       = md.mTextureUVs[0].get();
    end             = std::min({end, positions.size(), uv0.size()});
    if (end <= begin) return;

    std::string const& name   = nameOf(block);
    auto const         byName = snap->byBlock.find(name);

    auto const& off    = tess.mPostTransformOffset.get();
    auto const& sc     = tess.mPostTransformScale.get();
    V3 const    origin = {(pos.x + off.x) * sc.x, (pos.y + off.y) * sc.y, (pos.z + off.z) * sc.z};

    for (size_t q = begin; q + 4 <= end; q += 4) {
        V3 p[4];
        float cu = 0, cv = 0;
        for (size_t k = 0; k < 4; ++k) {
            p[k] = {positions[q + k].x, positions[q + k].y, positions[q + k].z};
            cu += uv0[q + k].x * 0.25f, cv += uv0[q + k].y * 0.25f;
        }
        auto const facing = quadFacing(p, origin);
        if (facing.tilted || facing.side == Side::Center) continue;
        core::Dir const face = dirOf(facing.side);

        // Panes and bars stacked on the same kind: Java hides the edge strips between
        // them (so a seamless wall has a frame only at its top); Bedrock draws them.
        // Collapse those quads to a point.
        bool const paneDiag = isPaneLike(name) && gPaneDiag.load(std::memory_order_relaxed) > 0;
        if (paneDiag && gPaneDiag.fetch_sub(1) > 0) {
            V3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
            for (auto const& v : p)
                mn = {std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z)},
                mx = {std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z)};
            paneLog("pane {} @({},{},{}) q{} face {} local [{:.3f},{:.3f},{:.3f}]-[{:.3f},{:.3f},{:.3f}] uv [{:.5f},{:.5f}]", name,
                       pos.x, pos.y, pos.z, q - begin, int(face), (mn.x - origin.x) / sc.x, (mn.y - origin.y) / sc.y,
                       (mn.z - origin.z) / sc.z, (mx.x - origin.x) / sc.x, (mx.y - origin.y) / sc.y, (mx.z - origin.z) / sc.z, cu, cv);
        }
        if ((face == core::Dir::Up || face == core::Dir::Down) && isPaneLike(name)) {
            float const ly = (p[0].y - origin.y) / sc.y;
            bool const  up = face == core::Dir::Up;
            if (std::abs(ly - (up ? 1.0f : 0.0f)) < 1e-3f) {
                BlockPos const other{pos.x, pos.y + (up ? 1 : -1), pos.z};
                if (nameOf(region.getBlock(other)) == name) {
                    for (size_t k = 1; k < 4; ++k) positions[q + k] = positions[q];
                    if (paneDiag) paneLog("  -> edge hidden");
                    continue;
                }
            }
        }

        // Candidates: texture rules first (file order), then the block's; highest weight
        // wins. Overlays are collected separately: each one that matches is drawn.
        Live const*              best = nullptr;
        UvRect                   source{};
        std::vector<Live const*> overlays;
        auto consider = [&](Live const& l, UvRect const& src) {
            if (!(l.rule->faces & core::dirBit(face)) || !heightOk(*l.rule, pos.y)) return;
            if (l.overlay) {
                if (std::find(overlays.begin(), overlays.end(), &l) == overlays.end()) overlays.push_back(&l);
                return;
            }
            if (!best || l.rule->weight > best->rule->weight) best = &l, source = src;
        };
        for (int i : snap->byTile)
            for (auto const& r : snap->live[size_t(i)].sources)
                if (r.contains(cu, cv)) {
                    consider(snap->live[size_t(i)], r);
                    break;
                }
        if (byName != snap->byBlock.end())
            for (int i : byName->second) {
                auto const& l = snap->live[size_t(i)];
                if (!statesOk(l.matchAny, l.matchStates, nameOf(block), region, pos)) continue;
                consider(l, faceTexture(block, int(face)));
            }
        // Overlays first: they copy the quad as it is now (before a split moves it).
        // Not on grass blocks: their alpha is a tint mask, so an overlay's see-through
        // pixels cannot be drawn there.
        if (tintMasked(block)) overlays.clear();
        for (auto const* ov : overlays) {
            auto const& r = *ov->rule;
            std::vector<int> tiles;
            switch (r.method) {
            case core::Method::Overlay: tiles = core::overlayTiles(overlayNeighbours(*ov, block, pos, face, region)); break;
            case core::Method::OverlayFixed: tiles = {0}; break;
            case core::Method::OverlayRandom: tiles = {core::randomTile(r, pos.x, pos.y, pos.z, face)}; break;
            case core::Method::OverlayRepeat: {
                Axes const a = axesOf(face);
                tiles = {core::repeatTile(r, pos.x * a.right[0] + pos.y * a.right[1] + pos.z * a.right[2],
                                          -(pos.x * a.up[0] + pos.y * a.up[1] + pos.z * a.up[2]))};
                break;
            }
            default: break;
            }
            bool                      drew = false;
            std::optional<mce::Color> tint;
            bool                      tintDone = false;
            for (int t : tiles)
                if (t >= 0 && size_t(t) < ov->tiles.size() && ov->tiles[size_t(t)].valid()) {
                    if (!tintDone) {
                        tintDone = true;
                        try {
                            tint = overlayTint(r, block, region, pos);
                        } catch (...) {}
                    }
                    appendOverlay(tess, q, origin, facing.side, face, ov->tiles[size_t(t)], tint);
                    drew = true;
                }
            if (drew) ensureCutout(block);
        }
        if (!best) continue;
        auto const& rule = *best->rule;

        std::vector<std::pair<core::Range, UvRect>> parts;
        auto whole = [&](int tile) {
            if (tile < 0 || size_t(tile) >= best->tiles.size() || !best->tiles[size_t(tile)].valid()) return;
            parts.push_back({{0, 0, 1, 1}, best->tiles[size_t(tile)]});
        };
        switch (rule.method) {
        case core::Method::Compact: {
            uint8_t const nb       = neighbours(*best, block, source, pos, face, region);
            auto const    quarters = core::compactQuarters(nb);
            if (paneDiag)
                paneLog("  -> compact neighbours {:08b} quarters {} {} {} {}", nb, quarters[0], quarters[1], quarters[2], quarters[3]);
            bool const uniform  = std::all_of(quarters.begin(), quarters.end(), [&](int t) { return t == quarters[0]; });
            if (uniform) whole(quarters[0]);
            else
                for (size_t i = 0; i < 4; ++i) {
                    int const t = quarters[i];
                    if (size_t(t) < best->tiles.size() && best->tiles[size_t(t)].valid())
                        parts.push_back({core::kQuarters[i], best->tiles[size_t(t)]});
                }
            break;
        }
        case core::Method::Fixed: whole(0); break;
        case core::Method::Random: whole(core::randomTile(rule, pos.x, pos.y, pos.z, face)); break;
        case core::Method::Repeat: {
            Axes const a  = axesOf(face);
            int const  cu = pos.x * a.right[0] + pos.y * a.right[1] + pos.z * a.right[2];
            int const  cv = -(pos.x * a.up[0] + pos.y * a.up[1] + pos.z * a.up[2]); // image v grows downwards
            whole(core::repeatTile(rule, cu, cv));
            break;
        }
        case core::Method::Top: {
            if (face == core::Dir::Up || face == core::Dir::Down) break;
            BlockPos const above{pos.x, pos.y + 1, pos.z};
            if (same(*best, block, source, region.getBlock(above), face)) whole(0);
            break;
        }
        default: break;
        }
        if (parts.empty()) continue;
        if (parts.size() > 1 && tess.mApplyTransform) parts.resize(1); // cannot add quads through a transform
        if (gLog && gDiag.load(std::memory_order_relaxed) > 0 && gDiag.fetch_sub(1) > 0)
            gLog->info("CTM: {} @({},{},{}) face {} rule {} -> {} part(s)", name, pos.x, pos.y, pos.z, int(face), rule.file,
                       parts.size());
        writeQuads(tess, q, origin, facing.side, parts);
    }
}

void ensureGeometryLayer(Block const& block, bool cutout, bool translucent) {
    auto& type = const_cast<BlockType&>(block.getBlockType());
    // Blocks with a block entity (cauldrons...) pick their draw pass their own way: a moved
    // layer makes them vanish.
    if (int(type.mBlockEntityType) != 0) return;
    auto const now   = BlockRenderLayer(type.mRenderLayer);
    bool const blend = now == BlockRenderLayer::RenderlayerBlend || now == BlockRenderLayer::RenderlayerBlendToOpaque;
    bool const opaque = now == BlockRenderLayer::RenderlayerOpaque || now == BlockRenderLayer::RenderlayerSeasonsOpaque;
    // Overlapping model parts in a sorted (blended) layer show through each other; but
    // half see-through textures (stained glass) need that layer, or they turn clear.
    if (blend && translucent) return;
    if (!blend && !(cutout && opaque)) return;
    {
        std::lock_guard lock(gLayerMutex);
        if (!gOriginalLayers.try_emplace(&type, now).second) return;
    }
    type.mRenderLayer = BlockRenderLayer::RenderlayerAlphatestSingleSide;
    game::requestChunkRebuild();
    if (gLog) gLog->info("P5: {} now draws in the alpha-tested layer", nameOf(block));
}

} // namespace tessera::game::ctm
