#include "game/Engine.h"
#include "game/ApiSelfTest.h"
#include "game/Providers.h"
#include "game/Ctm.h"
#include "game/Items.h"
#include "game/JavaPacks.h"
#include "game/PackMenu.h"
#include "game/WorldStates.h"

#include "core/Remap.h"
#include "core/Rules.h"

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/common/client/renderer/helpers/MeshHelpers.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/client/components/ClientSkin.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/renderer/block/BlockGraphics.h"
#include "mc/client/renderer/block/BlockTessellator.h"
#include "mc/client/renderer/chunks/RenderChunkBuilder.h"
#include "mc/client/renderer/chunks/RenderChunkCoordinator.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/client/renderer/texture/TextureAtlas.h"
#include "mc/client/renderer/texture/TextureAtlasResourceCallbacks.h"
#include "mc/client/renderer/texture/TextureUVCoordinateSet.h"
#include "mc/client/world/level/biome/biome_color_sampling/GrassTessellationPolicy.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/core_graphics/TextureSetImageContainer.h"
#include "mc/deps/json/Value.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTexture.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/world/actor/player/SerializedSkinRef.h"
#include "mc/world/level/storage/Experiments.h"
#include "mc/world/level/storage/LevelData.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/minecraft_renderer/renderer/MeshData.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/resources/ResourcePackListener.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/NoteBlock.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <glm/gtc/matrix_transform.hpp>
#include <numeric>
#include <variant>
#include <set>
#include <array>
#include <thread>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tessera::game {
namespace {

ll::io::Logger* gLog = nullptr;

constexpr char const* kRulesFile = "state_textures.json";

// ---------------------------------------------------------------------------
// Rule snapshot (swapped on resource (re)load) and caches keyed by generation.
// ---------------------------------------------------------------------------
std::mutex                      gSnapMutex;
std::shared_ptr<RuleSet const>  gRules;
std::atomic<uint64_t>           gGeneration{1};
std::atomic<int>                gDiagBudget{0}; // detailed logs for the first N rule hits (debug only)
bool                            gDebugLogs = false; // data/debug.txt exists
std::mutex                           gDiagMutex;
std::unordered_map<std::string, int> gDiagCounts;
std::atomic<int>                gBuildDiagBudget{0};
std::atomic<bool>               gRebuildAllChunks{false}; // set when resources reload while a world is open
// In-world reload: 0 idle, 1 asked the game to reload (waiting for the terrain atlas),
// 2 atlas finished (block textures still point at the old atlas).
std::atomic<int>                gHotStage{0};
std::atomic<bool>               gReloadRequested{false}; // from the pack menu
std::atomic<bool>               gRebuildQuiet{false};    // e.g. connected textures changed a block's render layer

// Textures the game uploads from memory (player skins, capes, maps) rather than
// reads from packs. A reload drops them and has no file to bring them back from,
// so a copy is kept and uploaded again afterwards.
struct Uploaded {
    ResourceLocation location;
    cg::ImageBuffer  image;
    size_t           bytes;
    void const*      data = nullptr; // the texture object the upload produced (identity only)
    Uploaded(ResourceLocation const& l, cg::ImageBuffer const& i, size_t b) : location(l), image(i), bytes(b) {}
};
using TextureData = std::shared_ptr<BedrockTextureData>;
// The colour layer's shared pointer inside a BedrockTexture (same layout as the member).
TextureData& colourOf(BedrockTexture& texture) {
    return *reinterpret_cast<TextureData*>(std::addressof(texture.mBedrockTextureData));
}
std::mutex                                              gUploadMutex;
std::unordered_map<uint64_t, std::unique_ptr<Uploaded>> gUploaded; // by ResourceLocation full hash
// Copies of uploaded textures, for F3+R to put back (experimental). Off: the SDK's
// ImageBuffer has no destructor, so the copies could never be freed (each reload leaked
// up to the limit), and freeing them through mce::Blob aborts the game.
constexpr bool kRecordUploads = false;
size_t                                                  gUploadedBytes = 0;
constexpr size_t                                        kUploadedLimit = 96u << 20;
thread_local bool                                       tReplayingUploads = false;
std::atomic<int>                                        gUploadDiag{40};

std::shared_ptr<RuleSet const> rulesSnapshot() {
    std::lock_guard lock(gSnapMutex);
    return gRules;
}

void loadRules(ResourcePackManager& packs, char const* reason) {
    std::string text;
    ResourceLocation const location{Core::PathView{kRulesFile}, ResourceFileSystem::UserPackage};
    bool       found = packs.load(location, text);
    // Rules baked from Java-layout packs follow the pack's own rules (which win).
    size_t     javaCount = 0;
    if (std::string const java = javapacks::rulesJson(); !java.empty()) {
        auto generated = nlohmann::json::parse(java, nullptr, false);
        if (!generated.is_discarded() && generated["rules"].is_array()) {
            javaCount = generated["rules"].size();
            if (found) {
                auto own = nlohmann::json::parse(text, nullptr, false, true);
                if (!own.is_discarded() && own.is_object() && own["rules"].is_array()) {
                    for (auto& r : generated["rules"]) own["rules"].push_back(std::move(r));
                    text = own.dump();
                }
            } else {
                text  = java;
                found = true;
            }
        }
    }
    ctm::resolve(); // block textures are in place: find the CTM tiles in the atlas
    auto set = std::make_shared<RuleSet>(found ? RuleSet::parse(text) : RuleSet{});
    if (gLog) {
        if (!found) gLog->info("[{}] no {} in the resource pack stack; Tessera is idle", reason, kRulesFile);
        else gLog->info("[{}] loaded {} rule(s) ({} from Java packs)", reason, set->rules.size(), javaCount);
        for (auto const& w : set->warnings) gLog->warn("{}: {}", kRulesFile, w);
    }
    {
        std::lock_guard lock(gSnapMutex);
        gRules = std::move(set);
    }
    gGeneration.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(gDiagMutex);
        gDiagCounts.clear();
    }
    gDiagBudget.store(gDebugLogs ? 500 : 0);
}

// ---------------------------------------------------------------------------
// Texture lookup through the game's terrain atlas.
// ---------------------------------------------------------------------------
struct ResolvedTexture {
    UvRect   rect;
    uint16_t pbr{0};
    short    textureSet{-1};
    bool     ok{false};
};

ResolvedTexture lookupTexture(TextureRef const& ref) {
    auto const     set = BlockGraphics::getTextureUVCoordinateSet(ref.name, ref.variant, 0);
    ResolvedTexture out;
    out.rect       = {set._u0, set._v0, set._u1, set._v1};
    out.pbr        = set.mPBRTextureDataHandle;
    out.textureSet = set.textureSetTranslationIndex;
    out.ok         = out.rect.valid();
    return out;
}

std::mutex                                        gTexMutex;
uint64_t                                          gTexGeneration = 0;
std::unordered_map<std::string, ResolvedTexture>  gTexCache;
UvRect                                            gMissingRect;

ResolvedTexture resolveTexture(TextureRef const& ref, uint64_t generation) {
    std::string key = ref.name + '#' + std::to_string(ref.variant);
    std::lock_guard lock(gTexMutex);
    if (gTexGeneration != generation) {
        gTexCache.clear();
        gTexGeneration = generation;
        gMissingRect   = lookupTexture({"missing", 0}).rect;
    }
    if (auto it = gTexCache.find(key); it != gTexCache.end()) return it->second;
    auto tex = lookupTexture(ref);
    if (tex.ok && tex.rect == gMissingRect && ref.name != "missing") tex.ok = false;
    if (gLog) {
        if (tex.ok && gDebugLogs)
            gLog->info(
                "texture '{}' -> uv [{:.5f},{:.5f}]-[{:.5f},{:.5f}] pbrHandle={} textureSet={}",
                key, tex.rect.u0, tex.rect.v0, tex.rect.u1, tex.rect.v1, tex.pbr, tex.textureSet
            );
        else if (!tex.ok) gLog->warn("texture '{}' is not in the terrain atlas (add it to textures/terrain_texture.json)", key);
    }
    gTexCache.emplace(std::move(key), tex);
    return tex;
}

// ---------------------------------------------------------------------------
// Per-Block (permutation) compiled entries, cached per thread.
// ---------------------------------------------------------------------------
struct GeoDraw {
    std::array<float, 12> pos;
    std::array<float, 8>  uv;
    UvRect                rect;
    int8_t                dir, cull;
    bool                  shade;
    int8_t                tint;
    int32_t               tintColor;
    uint16_t              pbr = 0; // the texture's shader-map handle
    // Faces reaching outside the block lie exactly on the same face of the next block (the
    // same model there): they are moved this much along their normal, one way or the other
    // by the block's position, so the two never share a plane.
    std::array<float, 3>  nudge{};
};

struct BlockEntry {
    std::string              name;
    std::string              statesText;
    std::vector<Replacement> replacements;
    std::vector<GeoDraw>     geometry; // P5: drawn instead of the block's own shape
    std::vector<GeoDraw>     guiGeometry; // item form in a slot: geometry seen as Java's display.gui shows it
    bool                     cutout{false};
    bool                     translucent{false};
};

// The item shape as Java's inventory shows it, expressed in the block space the slot views:
// its own display.gui is applied and the slot's view (rotation 30 45 0, scale 0.625) undone.
std::vector<GeoDraw> guiPose(std::vector<GeoDraw> const& geometry, std::array<float, 9> const& t) {
    auto rot = [](float x, float y, float z) { // Java: rotationXYZ, a vector turns about z, then y, then x
        glm::mat4 m(1.f);
        m = glm::rotate(m, glm::radians(x), glm::vec3{1, 0, 0});
        m = glm::rotate(m, glm::radians(y), glm::vec3{0, 1, 0});
        m = glm::rotate(m, glm::radians(z), glm::vec3{0, 0, 1});
        return glm::mat3(m);
    };
    glm::mat3 const own     = rot(t[0], t[1], t[2]);
    // Bedrock's slot view of a block is Java's block view turned half round (rotation 30 45 0).
    glm::mat3 const undo    = glm::inverse(rot(30.f, 45.f, 0.f));
    glm::vec3 const move    = glm::vec3{t[3], t[4], t[5]} / 16.f;
    glm::vec3 const scale   = glm::vec3{t[6], t[7], t[8]} / 0.625f;
    glm::vec3 const centre{0.5f, 0.5f, 0.5f};
    std::vector<GeoDraw> out = geometry;
    for (auto& g : out) {
        for (int k = 0; k < 4; ++k) {
            glm::vec3 p{g.pos[size_t(k * 3)], g.pos[size_t(k * 3 + 1)], g.pos[size_t(k * 3 + 2)]};
            p = centre + undo * ((own * ((p - centre) * scale)) + move / 0.625f);
            g.pos[size_t(k * 3)] = p.x, g.pos[size_t(k * 3 + 1)] = p.y, g.pos[size_t(k * 3 + 2)] = p.z;
        }
        // Turned: shaded by the way it now faces.
        glm::vec3 const p0{g.pos[0], g.pos[1], g.pos[2]}, p1{g.pos[3], g.pos[4], g.pos[5]}, p3{g.pos[9], g.pos[10], g.pos[11]};
        glm::vec3 const n = glm::cross(p3 - p0, p1 - p0);
        static constexpr float kAxes[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
        int   best = -1;
        float most = 0.f;
        for (int d = 0; d < 6; ++d)
            if (float const v = n.x * kAxes[d][0] + n.y * kAxes[d][1] + n.z * kAxes[d][2]; v > most) most = v, best = d;
        g.dir   = int8_t(best);
        g.cull  = -1;
        g.nudge = {};
    }
    return out;
}

StateMap readStates(CompoundTag const& nbt, std::string& name) {
    StateMap states;
    if (nbt.contains("name") && nbt.at("name").hold(Tag::String)) name = nbt.at("name").get<StringTag>();
    if (!nbt.contains("states") || !nbt.at("states").hold(Tag::Compound)) return states;
    for (auto const& [key, value] : nbt.at("states").get<CompoundTag>()) {
        StateValue v;
        switch (value.getId()) {
        case Tag::Byte:
            v.kind = StateValue::Kind::Bool;
            v.i    = value.get<ByteTag>().data != 0;
            break;
        case Tag::Short: v.i = value.get<ShortTag>().data; break;
        case Tag::Int: v.i = value.get<IntTag>().data; break;
        case Tag::Int64: v.i = value.get<Int64Tag>().data; break;
        case Tag::String:
            v.kind = StateValue::Kind::String;
            v.s    = value.get<StringTag>();
            break;
        default: continue;
        }
        states.emplace(key, std::move(v));
    }
    return states;
}

// What a Block permutation is, read once per thread from its serialization NBT.
struct BlockInfo {
    std::string name;
    StateMap    states;
    bool        candidate{false}; // some rule names this block
    bool        usesVirtual{false};
    bool        usesPower{false}; // a rule reads tessera:powered / tessera:power
    bool        usesSignal{false}; // a rule reads tessera:signal
    bool        usesLinks{false};  // a rule reads tessera:redstone_<side> / tessera:tripwire_<side>
    std::vector<std::string> providerKeys; // keys of other mods' providers that rules read
    bool        solid{false};     // full solid block: carries redstone power to its neighbours
    uint32_t    pickModulus{0};   // rules pick alternatives by position: what the seed is taken modulo
};

uint8_t parseFacing(std::string_view f) {
    if (f == "up" || f == "top") return sideBit(Side::Top);
    if (f == "down" || f == "bottom") return sideBit(Side::Bottom);
    if (f == "north") return sideBit(Side::North);
    if (f == "south") return sideBit(Side::South);
    if (f == "east") return sideBit(Side::East);
    if (f == "west") return sideBit(Side::West);
    if (f == "side" || f == "sides")
        return sideBit(Side::North) | sideBit(Side::South) | sideBit(Side::East) | sideBit(Side::West);
    if (gLog) gLog->warn("unknown facing '{}' in a wildcard source", f);
    return kAllSides;
}

std::shared_ptr<BlockEntry const>
compileEntry(BlockInfo const& info, StateMap const& states, RuleSet const& rules, uint64_t generation, bool itemForm, uint32_t pick) {
    auto const& name  = info.name;
    auto        entry = std::make_shared<BlockEntry>();
    entry->name       = name;
    for (auto const& [k, v] : states) entry->statesText += std::format("{}={} ", k, v.toString());

    for (auto index : rules.rulesFor(name)) {
        auto const& rule = rules.rules[index];
        if (rule.item && !itemForm) continue;
        if (rule.pickTotal) {
            uint32_t const at = pick % rule.pickTotal;
            if (at < rule.pickLo || at >= rule.pickHi) continue;
        }
        bool pass = true;
        for (auto const& c : rule.when)
            if (!c.matches(states)) {
                pass = false;
                break;
            }
        if (!pass) continue;
        if (rule.geometry) {
            entry->geometry.clear();
            for (auto const& q : *rule.geometry) {
                auto t = resolveTexture(parseTextureRef(q.texture), generation);
                if (!t.ok) continue;
                GeoDraw draw{q.pos, q.uv, t.rect, q.dir, q.cull, q.shade, q.tint, q.tintColor};
                draw.pbr = t.pbr;
                bool    outside = false;
                for (float v : q.pos) outside |= v < -1e-4f || v > 1.0001f;
                if (outside) {
                    glm::vec3 const p0{q.pos[0], q.pos[1], q.pos[2]}, p1{q.pos[3], q.pos[4], q.pos[5]}, p3{q.pos[9], q.pos[10], q.pos[11]};
                    glm::vec3 const n = glm::cross(p1 - p0, p3 - p0);
                    if (float const len = glm::length(n); len > 1e-8f) draw.nudge = {n.x / len * 0.002f, n.y / len * 0.002f, n.z / len * 0.002f};
                }
                entry->geometry.push_back(draw);
            }
            entry->cutout = rule.cutout;
            entry->translucent = rule.translucent;
            entry->guiGeometry.clear();
            if (rule.gui && itemForm) entry->guiGeometry = guiPose(entry->geometry, *rule.gui);
        }
        std::vector<Replacement> ruleReplacements;
        for (auto const& [src, dstTpl] : rule.replace) {
            bool templateOk = true;
            auto dstName    = expandTemplate(dstTpl, states, &templateOk);
            if (!templateOk && gLog) gLog->warn("{}: template '{}' uses a state this block does not have", name, dstTpl);
            Replacement r;
            if (src.name.starts_with('*')) { // "*" or "*:up" / "*:north" ...
                r.anySource = true;
                std::string_view spec = src.name;
                if (auto at = spec.find('@'); at != std::string_view::npos) { // "*:east@0.125"
                    float plane = -1.0f;
                    auto  text  = spec.substr(at + 1);
                    if (std::from_chars(text.data(), text.data() + text.size(), plane).ec == std::errc{}) r.plane = plane;
                    spec = spec.substr(0, at);
                }
                if (auto colon = spec.find(':'); colon != std::string_view::npos)
                    r.facingMask = parseFacing(spec.substr(colon + 1));
            }
            ResolvedTexture s;
            if (!r.anySource) s = resolveTexture(src, generation);
            auto d = resolveTexture(parseTextureRef(dstName), generation);
            if ((!r.anySource && !s.ok) || !d.ok) continue;
            r.src       = s.rect;
            r.dst       = d.rect;
            r.sideMask  = rule.sideMask;
            r.rotX      = rule.rotX;
            r.rotY      = rule.rotY;
            r.tint      = rule.tint;
            r.tintRgba  = rule.tintRgba;
            r.pbrHandle = d.pbr;
            r.hasPbr    = d.pbr != 0; // the target's own shader maps (Vibrant Visuals, shader mods)
            // Held (item form): Bedrock draws a grass block with its pre-coloured "carried"
            // images and no tint; Java tints the item with its plains grass colour (#7CBD6B).
            bool const grassItem = itemForm && name == "minecraft:grass_block";
            if (grassItem && r.anySource && r.facingMask == sideBit(Side::Top) && !dstName.ends_with("_grass")) {
                r.tint     = TintMode::Color;
                r.tintRgba = 0xFF6BBD7Cu; // R | G << 8 | B << 16
            }
            if (dstName.ends_with("_grass"))
                for (int v = 0; v < 32; ++v) {
                    // Quietly up to the last variant (lookupTexture: no "not in the atlas" log).
                    auto const from = lookupTexture({"grass_side", v});
                    auto const to   = lookupTexture({dstName, v});
                    if (!from.ok || !to.ok || from.rect == gMissingRect || to.rect == gMissingRect
                        || (v > 0 && from.rect == r.variants.front().first))
                        break;
                    r.variants.emplace_back(from.rect, to.rect);
                }
            if (dstName.ends_with("_grass") && itemForm) { // the held side: carried image -> item-tinted one
                auto const from = lookupTexture({"grass_side_carried", 0});
                auto const to   = lookupTexture({dstName + "_item", 0});
                if (from.ok && to.ok && from.rect != gMissingRect && to.rect != gMissingRect) r.variants.emplace_back(from.rect, to.rect);
            }
            ruleReplacements.push_back(r);
        }
        // Plane-specific wildcards must be tried before the rule's catch-all ones
        // (JSON object keys arrive sorted, "*:east" before "*:east@0.125").
        std::stable_partition(ruleReplacements.begin(), ruleReplacements.end(), [](Replacement const& x) {
            return x.anySource && x.plane >= 0.0f;
        });
        entry->replacements.insert(entry->replacements.end(), ruleReplacements.begin(), ruleReplacements.end());
    }
    return entry; // may be empty: "no rule applies in this state" is cached too
}

struct EntryKey {
    Block const* block;
    uint64_t     virt; // packed / hashed values of the computed states
    bool         operator==(EntryKey const&) const = default;
};
struct EntryKeyHash {
    size_t operator()(EntryKey const& k) const noexcept {
        return std::hash<void const*>{}(k.block) ^ size_t(k.virt * 0x9E3779B97F4A7C15ull);
    }
};

struct ThreadCache {
    uint64_t                                                                      generation{0};
    std::shared_ptr<RuleSet const>                                                rules;
    std::unordered_map<Block const*, BlockInfo>                                   infos;
    std::unordered_map<EntryKey, std::shared_ptr<BlockEntry const>, EntryKeyHash> entries;
    size_t                                                                        tessellateCalls{0};
};
thread_local ThreadCache tCache;

BlockInfo const& infoFor(Block const& block) {
    auto [it, inserted] = tCache.infos.try_emplace(&block);
    if (inserted) {
        auto& info        = it->second;
        info.states       = readStates(block.mSerializationId.get(), info.name);
        auto const& r     = *tCache.rules;
        info.candidate    = !info.name.empty() && r.hasBlock(info.name);
        info.usesVirtual  = info.candidate && r.blockUsesVirtual(info.name);
        info.usesSignal   = info.usesVirtual && r.blockUsesKey(info.name, "tessera:signal");
        if (info.usesVirtual)
            for (auto const& key : providers::keys())
                if (r.blockUsesKey(info.name, key)) info.providerKeys.push_back(key);
        info.usesPower    = info.usesVirtual
                         && (r.blockUsesKey(info.name, "tessera:powered") || r.blockUsesKey(info.name, "tessera:power"));
        info.usesLinks    = info.usesVirtual
                         && (r.blockUsesKey(info.name, "tessera:redstone_north") || r.blockUsesKey(info.name, "tessera:tripwire_north")
                             || r.blockUsesKey(info.name, "tessera:pane_north"));
        info.solid        = block.mCachedComponentData->mIsSolid;
        if (info.candidate) {
            uint32_t m = 1;
            for (auto index : r.rulesFor(info.name))
                if (uint32_t const t = r.rules[index].pickTotal) m = std::lcm(m, t);
            info.pickModulus = m > 1 ? std::min<uint32_t>(m, 840) : 0;
        }
    }
    return it->second;
}

int64_t intState(StateMap const& states, std::string_view key, int64_t fallback = -1) {
    auto it = states.find(key);
    return it == states.end() || it->second.kind == StateValue::Kind::String ? fallback : it->second.i;
}

// Computed ("tessera:") states. Only plain block reads through the tessellator's own
// region, which is safe on chunk-build threads (the game reads neighbours the same way).
// ---------------------------------------------------------------------------
// tessera:powered / tessera:power - is this block receiving redstone power?
// Bedrock keeps that in the server's circuit system, which the client never
// sees, so it is worked out from the redstone parts around the block, using
// only their block states: direct sources next to it, and solid blocks next to
// it that are themselves powered. Good for displays; it is an estimate, not the
// game's own answer (see docs/api.md for what it does not cover).
// ---------------------------------------------------------------------------
constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}; // D U N S W E
constexpr int kOpposite[6] = {1, 0, 3, 2, 5, 4};
constexpr char const* kDirName[6] = {"down", "up", "north", "south", "west", "east"};

int dirFromName(std::string_view s) {
    for (int i = 0; i < 6; ++i)
        if (s == kDirName[i]) return i;
    return -1;
}
std::string_view stringState(StateMap const& states, std::string_view key) {
    auto it = states.find(key);
    return it == states.end() || it->second.kind != StateValue::Kind::String ? std::string_view{} : it->second.s;
}
bool sameAxis(int a, int b) { return a >= 0 && b >= 0 && a / 2 == b / 2; }

// Power the block `s` gives to the block next to it in direction `toTarget`.
// `intoSolid`: the target is a solid block being powered (stricter than a
// mechanism reading its neighbours: a redstone block or a torch beside it does not count).
int deliveredPower(BlockInfo const& s, int toTarget, bool intoSolid) {
    std::string_view n = s.name;
    if (n.starts_with("minecraft:")) n.remove_prefix(10);
    auto signal = [&] { return int(intState(s.states, "redstone_signal", 0)); };
    auto bit    = [&](std::string_view key) { return intState(s.states, key, 0) != 0; };

    if (n == "redstone_block") return intoSolid ? 0 : 15;
    if (n == "redstone_torch") { // lit; the unlit one is a different block
        auto facing   = stringState(s.states, "torch_facing_direction");
        int  attached = facing == "top" || facing == "unknown" || facing.empty() ? 0 : dirFromName(facing);
        if (toTarget == attached) return 0;                 // never powers the block it sits on
        return intoSolid ? (toTarget == 1 ? 15 : 0) : 15;   // strongly powers only the block above
    }
    if (n == "lever") return bit("open_bit") ? 15 : 0;
    if (n.ends_with("_button")) return bit("button_pressed_bit") ? 15 : 0;
    if (n.ends_with("pressure_plate")) {
        int p = signal();
        return p > 0 && (!intoSolid || toTarget == 0) ? p : 0;
    }
    if (n == "redstone_wire") {
        int p = signal();
        if (p <= 0) return 0;
        if (toTarget == 0) return p; // the block the dust lies on
        if (toTarget == 1) return 0;
        bool any = false;
        for (int d = 2; d < 6; ++d)
            if (auto c = stringState(s.states, std::string("redstone_") + kDirName[d]); !c.empty() && c != "none") any = true;
        auto toward = stringState(s.states, std::string("redstone_") + kDirName[toTarget]);
        return !any || (!toward.empty() && toward != "none") ? p : 0; // a lone dot powers all sides
    }
    if (n == "powered_repeater" || n == "powered_comparator")
        return sameAxis(dirFromName(stringState(s.states, "minecraft:cardinal_direction")), toTarget) ? 15 : 0;
    if (n == "observer")
        return bit("powered_bit") && sameAxis(dirFromName(stringState(s.states, "minecraft:facing_direction")), toTarget)
                 ? 15
                 : 0;
    if (intoSolid) return 0; // the rest only drive mechanisms and dust beside them
    if (n == "daylight_detector" || n == "daylight_detector_inverted") return signal();
    if (n == "tripwire_hook" || n == "lectern" || n == "lightning_rod") return bit("powered_bit") ? 15 : 0;
    if (n == "detector_rail") return bit("rail_data_bit") ? 15 : 0;
    if (n == "sculk_sensor" || n == "calibrated_sculk_sensor") return intState(s.states, "sculk_sensor_phase", 0) == 1 ? 15 : 0;
    return 0;
}

int estimatePower(BlockSource& region, BlockPos const& pos) {
    int best = 0;
    for (int d = 0; d < 6 && best < 15; ++d) {
        BlockPos const np{pos.x + kDir[d][0], pos.y + kDir[d][1], pos.z + kDir[d][2]};
        auto const&    n = infoFor(region.getBlock(np));
        best             = std::max(best, deliveredPower(n, kOpposite[d], false));
        if (!n.solid || best >= 15) continue;
        // A powered solid block activates what touches it.
        for (int e = 0; e < 6 && best < 15; ++e) {
            if (e == kOpposite[d]) continue; // that is the block being asked about
            BlockPos const sp{np.x + kDir[e][0], np.y + kDir[e][1], np.z + kDir[e][2]};
            best = std::max(best, deliveredPower(infoFor(region.getBlock(sp)), kOpposite[e], true));
        }
    }
    return best;
}

// Does redstone dust beside `s` (it lies toward `toward` from the dust) turn to it? As Java's
// RedStoneWireBlock.shouldConnectTo: dust; a repeater along its axis; an observer's back;
// any other signal source.
bool dustConnects(BlockInfo const& s, int toward) {
    std::string_view n = s.name;
    if (!n.starts_with("minecraft:")) return false;
    n.remove_prefix(10);
    if (n == "redstone_wire") return true;
    if (n == "powered_repeater" || n == "unpowered_repeater")
        return sameAxis(dirFromName(stringState(s.states, "minecraft:cardinal_direction")), toward);
    if (n == "observer") return intState(s.states, "minecraft:facing_direction") == toward;
    if (toward < 0) return false;
    static std::set<std::string_view> const sources = {"lever", "redstone_torch", "unlit_redstone_torch", "redstone_block",
                                                         "daylight_detector", "daylight_detector_inverted", "powered_comparator",
                                                         "unpowered_comparator", "target", "tripwire_hook", "trapped_chest",
                                                         "detector_rail", "lectern", "sculk_sensor", "calibrated_sculk_sensor",
                                                         "lightning_rod"};
    return sources.contains(n) || n.ends_with("_button") || n.ends_with("pressure_plate");
}

// tessera:redstone_<side> (none / side / up) and tessera:tripwire_<side> (true / false): the
// connections Java keeps as states and Bedrock works out while drawing, worked out as Java does.
uint64_t computeLinks(BlockSource& region, BlockInfo const& info, BlockPos const& pos, StateMap& out) {
    static constexpr int kSides[4] = {2, 5, 3, 4}; // north east south west (kDir order)
    auto at = [&](int x, int y, int z) -> BlockInfo const& { return infoFor(region.getBlock(BlockPos{x, y, z})); };
    uint64_t packed = 0;
    if (info.name == "minecraft:redstone_wire") {
        bool const aboveOpen = !at(pos.x, pos.y + 1, pos.z).solid;
        int        link[4]   = {0, 0, 0, 0}; // 0 none, 1 side, 2 up
        for (int i = 0; i < 4; ++i) {
            int const   d  = kSides[i];
            int const   x  = pos.x + kDir[d][0], z = pos.z + kDir[d][2];
            auto const& nb = at(x, pos.y, z);
            if (aboveOpen && nb.solid && at(x, pos.y + 1, z).name == "minecraft:redstone_wire") link[i] = 2;
            else if (dustConnects(nb, d) || (!nb.solid && at(x, pos.y - 1, z).name == "minecraft:redstone_wire")) link[i] = 1;
        }
        // As Java draws it: a line runs on through, a lone dust is a cross.
        bool const noNS = !link[0] && !link[2], noEW = !link[1] && !link[3];
        if (noNS) link[1] = link[1] ? link[1] : 1, link[3] = link[3] ? link[3] : 1;
        if (noEW) link[0] = link[0] ? link[0] : 1, link[2] = link[2] ? link[2] : 1;
        static constexpr char const* kName[4]  = {"tessera:redstone_north", "tessera:redstone_east", "tessera:redstone_south", "tessera:redstone_west"};
        static constexpr char const* kValue[3] = {"none", "side", "up"};
        for (int i = 0; i < 4; ++i) {
            StateValue v;
            v.kind = StateValue::Kind::String;
            v.s    = kValue[link[i]];
            out.insert_or_assign(kName[i], v);
            packed |= uint64_t(link[i]) << (2 * i);
        }
    } else if (info.name.ends_with("_pane")) {
        // Java's IronBarsBlock.attachsTo: panes, bars and walls, and full blocks (glass
        // included), but not leaves.
        static constexpr char const* kName[4] = {"tessera:pane_north", "tessera:pane_east", "tessera:pane_south", "tessera:pane_west"};
        for (int i = 0; i < 4; ++i) {
            int const   d  = kSides[i];
            auto const& nb = at(pos.x + kDir[d][0], pos.y, pos.z + kDir[d][2]);
            bool const  link =
                nb.name.ends_with("_pane") || nb.name == "minecraft:iron_bars" || nb.name.ends_with("_wall") || nb.name.ends_with("glass")
                || (nb.solid && !nb.name.ends_with("_leaves") && nb.name != "minecraft:barrier");
            StateValue v;
            v.kind = StateValue::Kind::Bool;
            v.i    = link;
            out.insert_or_assign(kName[i], v);
            packed |= uint64_t(link) << i;
        }
    } else if (info.name == "minecraft:trip_wire") {
        static constexpr char const* kName[4] = {"tessera:tripwire_north", "tessera:tripwire_east", "tessera:tripwire_south", "tessera:tripwire_west"};
        for (int i = 0; i < 4; ++i) {
            int const   d    = kSides[i];
            auto const& nb   = at(pos.x + kDir[d][0], pos.y, pos.z + kDir[d][2]);
            // Java: string, or a hook facing back along this line.
            bool        link = nb.name == "minecraft:trip_wire";
            if (nb.name == "minecraft:tripwire_hook") {
                static constexpr int kHook[4] = {3, 4, 2, 5}; // Bedrock direction 0..3: south west north east
                int64_t const        hd       = intState(nb.states, "direction");
                link = hd >= 0 && hd < 4 && sameAxis(kHook[hd], d);
            }
            StateValue v;
            v.kind = StateValue::Kind::Bool;
            v.i    = link;
            out.insert_or_assign(kName[i], v);
            packed |= uint64_t(link) << i;
        }
    }
    return packed;
}

uint64_t computeVirtual(BlockSource& region, BlockInfo const& info, BlockPos const& pos, StateMap& out) {
    uint64_t packed = 0;
    if (info.usesLinks) packed |= computeLinks(region, info, pos, out) << 13;
    if (info.usesPower) {
        int const  power = estimatePower(region, pos);
        StateValue p;
        p.i = power;
        out.insert_or_assign("tessera:power", p);
        StateValue on;
        on.kind = StateValue::Kind::Bool;
        on.i    = power > 0;
        out.insert_or_assign("tessera:powered", on);
        packed |= uint64_t(power + 1) << 1;
    }
    // tessera:extended - a piston whose arm is out. Bedrock places a
    // (sticky_)piston_arm_collision block, with the piston's own facing, in front of it.
    if (info.name == "minecraft:piston" || info.name == "minecraft:sticky_piston") {
        int64_t const facing   = intState(info.states, "facing_direction");
        bool          extended = false;
        if (facing >= 0 && facing <= 5) {
            // Up/down are unambiguous; for horizontal facings accept either sign on the
            // axis (cheap insurance against Bedrock's piston facing quirks).
            int const axis = facing <= 1 ? 1 : facing <= 3 ? 2 : 0;
            for (int sign : {-1, 1}) {
                if (facing == 0 && sign > 0) continue;
                if (facing == 1 && sign < 0) continue;
                BlockPos n = pos;
                (axis == 0 ? n.x : axis == 1 ? n.y : n.z) += sign;
                auto const& ni = infoFor(region.getBlock(n));
                if (ni.name.ends_with("piston_arm_collision") && intState(ni.states, "facing_direction") == facing) {
                    extended = true;
                    break;
                }
            }
        }
        StateValue v;
        v.kind = StateValue::Kind::Bool;
        v.i    = extended;
        out.insert_or_assign("tessera:extended", v);
        packed |= uint64_t(extended);
    }
    // tessera:door_open / tessera:door_hinge - Bedrock keeps a door's open state on its lower
    // half and its hinge side on the upper half; each half gets both.
    if (info.name.ends_with("_door") && !info.name.ends_with("trapdoor")) {
        bool const  upper = intState(info.states, "upper_block_bit", 0) != 0;
        BlockInfo const& lower = upper ? infoFor(region.getBlock(BlockPos{pos.x, pos.y - 1, pos.z})) : info;
        BlockInfo const& top   = upper ? info : infoFor(region.getBlock(BlockPos{pos.x, pos.y + 1, pos.z}));
        bool const  open  = intState(lower.states, "open_bit", 0) != 0;
        bool const  right = intState(top.states, "door_hinge_bit", 0) != 0;
        StateValue  o;
        o.kind = StateValue::Kind::Bool;
        o.i    = open;
        out.insert_or_assign("tessera:door_open", o);
        StateValue h;
        h.kind = StateValue::Kind::String;
        h.s    = right ? "right" : "left";
        out.insert_or_assign("tessera:door_hinge", h);
        packed |= (uint64_t(open) << 11) | (uint64_t(right) << 12);
    }
    // tessera:signal - what a signal source (target, sculk sensors) puts out, read
    // from the redstone dust beside it (dust next to a source carries its strength).
    if (info.usesSignal) {
        int64_t signal = 0;
        static constexpr int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (auto const& o : d) {
            auto const& ni = infoFor(region.getBlock(BlockPos{pos.x + o[0], pos.y + o[1], pos.z + o[2]}));
            if (ni.name == "minecraft:redstone_wire") signal = std::max(signal, intState(ni.states, "redstone_signal", 0));
        }
        StateValue v;
        v.i = signal;
        out.insert_or_assign("tessera:signal", v);
        packed |= uint64_t(signal + 1) << 6;
    }
    // Other mods' providers (called here, on the chunk-build thread).
    for (auto const& key : info.providerKeys)
        if (auto v = providers::call(key, int(region.getDimensionId()), pos.x, pos.y, pos.z, info.name)) {
            uint64_t h = 1469598103934665603ull;
            for (unsigned char c : key + '=' + v->toString()) h = (h ^ c) * 1099511628211ull;
            packed ^= h * 0x9E3779B97F4A7C15ull;
            out.insert_or_assign(key, std::move(*v));
        }
    // Block-entity values and other mods' pushed values, from the client-thread snapshot.
    uint64_t const world = world::collect(int(region.getDimensionId()), pos.x, pos.y, pos.z, info.name, out);
    return packed ^ (world * 0x100000001B3ull);
}

thread_local int tItemForm = 0; // inside BlockTessellator::appendTessellatedBlock

BlockEntry const* entryFor(Block const& block, BlockPos const& pos, BlockSource* region) {
    auto const generation = gGeneration.load(std::memory_order_acquire);
    if (tCache.generation != generation) {
        tCache.entries.clear();
        tCache.infos.clear();
        tCache.rules      = rulesSnapshot();
        tCache.generation = generation;
    }
    if (!tCache.rules || tCache.rules->rules.empty()) return nullptr;
    auto const& info = infoFor(block);
    if (!info.candidate) return nullptr;

    StateMap virt;
    uint64_t packed = 0;
    if (info.usesVirtual && region) packed = computeVirtual(*region, info, pos, virt);

    bool const itemForm = tItemForm > 0; // drawn as an item: its item-form rules apply too
    if (itemForm) packed ^= 0x6974656D666F726Dull;
    // Alternatives picked by position, with Java's position seed (items take the first).
    uint32_t pick = 0;
    if (info.pickModulus && !itemForm) {
        int64_t seed = int64_t(pos.x) * 3129871LL ^ int64_t(pos.z) * 116129781LL ^ int64_t(pos.y);
        seed         = seed * seed * 42317861LL + seed * 11LL;
        pick         = uint32_t(uint64_t(seed >> 16) % info.pickModulus);
        packed ^= (uint64_t(pick) + 1) * 0xD6E8FEB86659FD93ull;
    }
    auto [it, inserted] = tCache.entries.try_emplace(EntryKey{&block, packed});
    if (inserted) {
        if (virt.empty()) it->second = compileEntry(info, info.states, *tCache.rules, generation, itemForm, pick);
        else {
            StateMap all = info.states;
            for (auto& [k, v] : virt) all.insert_or_assign(k, v);
            it->second = compileEntry(info, all, *tCache.rules, generation, itemForm, pick);
        }
    }
    auto const* e = it->second.get();
    return e && (!e->replacements.empty() || !e->geometry.empty()) ? e : nullptr;
}

// A few detailed logs per block name, so one busy block cannot use up the budget.
bool takeDiag(std::string const& name) {
    std::lock_guard lock(gDiagMutex);
    if (gDiagBudget.load() <= 0) return false;
    if (++gDiagCounts[name] > 2) return false;
    gDiagBudget.fetch_sub(1);
    return true;
}

void applyEntry(Tessellator& tess, size_t begin, size_t end, BlockEntry const& entry, BlockPos const& pos, Block const* blockPtr = nullptr) {
    auto& md        = tess.mMeshData.get();
    auto& positions = md.mPositions.get();
    auto& uvs       = md.mTextureUVs[0].get();
    auto& colors    = md.mColors.get();
    auto& pbr       = md.mPBRTextureIndices.get();

    VertexStreams s;
    s.count      = std::min(positions.size(), uvs.size());
    s.positions  = reinterpret_cast<V3*>(positions.data());
    s.uv0        = reinterpret_cast<V2*>(uvs.data());
    s.colors     = colors.size() >= s.count ? colors.data() : nullptr;
    s.pbrIndices = pbr.size() >= s.count ? pbr.data() : nullptr;

    bool const diag = gLog && takeDiag(entry.name + ' ' + entry.statesText);
    if (diag) {
        gLog->info(
            "{} @({},{},{}) [{}] vertices {}..{} ({}), uv/color/pbr sizes {}/{}/{}",
            entry.name, pos.x, pos.y, pos.z, entry.statesText, begin, end, end - begin, uvs.size(), colors.size(),
            pbr.size()
        );
        if ((end - begin) % 4 != 0) gLog->warn("  vertex count is not a multiple of 4; quads may be misaligned");
    }
    auto report = [&](QuadReport const& q) {
        gLog->info(
            "  quad v{} on={} faces={}{} local [{:.3f},{:.3f},{:.3f}]-[{:.3f},{:.3f},{:.3f}] uv [{:.5f},{:.5f}]-[{:.5f},{:.5f}] -> {}",
            q.firstVertex, sideName(q.side), sideName(q.facing), q.tilted ? "(tilted)" : "", q.localMin.x, q.localMin.y, q.localMin.z, q.localMax.x, q.localMax.y,
            q.localMax.z, q.uv.u0, q.uv.v0, q.uv.u1, q.uv.v1, q.replacement
        );
    };
    std::function<void(QuadReport const&)> reporter;
    if (diag) reporter = report;
    // The tessellator stores (world position + offset) * scale.
    auto const& off  = tess.mPostTransformOffset.get();
    auto const& sc   = tess.mPostTransformScale.get();
    V3 const    hint = {(pos.x + off.x) * sc.x, (pos.y + off.y) * sc.y, (pos.z + off.z) * sc.z};
    if (diag) gLog->info("  cell corner in vertex space: ({:.3f},{:.3f},{:.3f})", hint.x, hint.y, hint.z);
    size_t changed = remapRange(s, begin, end, entry.replacements, reporter, &hint);
    if (diag) gLog->info("  remapped {} quad(s)", changed);
}

std::filesystem::path gDataDir;

// P5: the block's own quads [begin, end) are folded to points and the entry's geometry is
// drawn instead, lit like the block's own faces (their colour and light, per direction).
void applyGeometry(Tessellator& tess, size_t begin, size_t end, BlockEntry const& entry, Block const& block,
                   BlockPos const& pos, BlockSource* region, bool inSlot = false);

void applyGeometry(Tessellator& tess, size_t begin, size_t end, BlockEntry const& entry, Block const& block,
                   BlockPos const& pos, BlockSource* region, bool inSlot) {
    auto const& shapes = inSlot && !entry.guiGeometry.empty() ? entry.guiGeometry : entry.geometry;
    auto& md        = tess.mMeshData.get();
    auto& positions = md.mPositions.get();
    auto& uv1       = md.mTextureUVs[1].get();
    auto& colors    = md.mColors.get();
    auto& normals   = md.mNormals.get();
    auto& pbr       = md.mPBRTextureIndices.get();
    end             = std::min(end, positions.size());
    static constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

    // Light and colour of the block's own faces, per direction (and overall).
    struct Acc {
        double r = 0, g = 0, b = 0, a = 0, l0 = 0, l1 = 0;
        int    n = 0;
        glm::vec4 normal{};
    } acc[7];
    // In an inventory slot the tessellator turns every vertex into the slot's view (its
    // transform matrix, which also mirrors): directions are compared in that space, and there
    // is no depth buffer, so only faces toward the viewer are drawn, far ones first.
    bool const      slotView = tess.mApplyTransform;
    glm::mat3 const view     = slotView ? glm::mat3(tess.mTransformMatrix.get()) : glm::mat3(1.f);
    bool const      mirrored = slotView && glm::determinant(view) < 0;
    glm::vec3       dirs[6];
    for (int d = 0; d < 6; ++d) dirs[d] = glm::normalize(view * glm::vec3{float(kDir[d][0]), float(kDir[d][1]), float(kDir[d][2])});
    for (size_t q = begin; q + 4 <= end; q += 4) {
        // Which way the face looks: front faces wind so that (v1 - v0) x (v3 - v0) points out
        // (the order applyGeometry emits). The mesh's own normals are not reliable for this.
        glm::vec3 nv = glm::cross(positions[q + 1] - positions[q], positions[q + 3] - positions[q]);
        if (mirrored) nv = -nv;
        int   best    = -1;
        float bestDot = 0.5f * glm::length(nv);
        for (int d = 0; d < 6; ++d) {
            float const dot = glm::dot(nv, dirs[d]);
            if (dot > bestDot) bestDot = dot, best = d;
        }
        for (size_t k = 0; k < 4; ++k) {
            for (int slot : {best < 0 ? 6 : best, 6}) {
                auto& a = acc[slot];
                if (colors.size() > q + k) {
                    uint32_t const c = colors[q + k];
                    a.r += c & 0xFF, a.g += (c >> 8) & 0xFF, a.b += (c >> 16) & 0xFF, a.a += c >> 24;
                }
                if (uv1.size() > q + k) a.l0 += uv1[q + k].x, a.l1 += uv1[q + k].y;
                ++a.n;
                if (normals.size() > q) a.normal = normals[q];
                if (slot == 6) break;
            }
        }
    }
    glm::vec3 const firstOwn = end > begin ? positions[begin] : glm::vec3{};
    // The block's own faces are not drawn.
    static bool const noFold = std::filesystem::exists(gDataDir / "p5_nofold.txt");
    if (!noFold)
        for (size_t q = begin; q + 4 <= end; q += 4)
            for (size_t k = 1; k < 4; ++k) positions[q + k] = positions[q];

    ushort const pbrIdx = pbr.size() > begin ? pbr[begin] : ushort(0);
    // Tints for faces the model marks with a tintindex.
    std::optional<mce::Color>            grassTint;
    std::optional<std::array<double, 3>> ownTint;
    bool                                 anyTinted = false;
    for (auto const& g : shapes) anyTinted |= g.tint >= 0;
    if (anyTinted) {
        auto const& n = entry.name;
        bool const  grassy = n.find("grass") != std::string::npos || n.ends_with("fern") || n == "minecraft:reeds"
                         || n == "minecraft:sugar_cane";
        if (grassy && region) {
            try {
                alignas(16) std::byte policy[64]{};
                grassTint = reinterpret_cast<BiomeColorSampling::GrassTessellationPolicy const*>(policy)->$get(block, *region, pos, nullptr);
            } catch (...) {}
        }
        if (!grassTint) { // the hue of the block's own faces: their colour over their shade
            static constexpr double kOwnShade[6] = {0.5, 1.0, 0.8, 0.8, 0.6, 0.6};
            int best = -1;
            for (int d : {1, 2, 3, 4, 5, 0})
                if (acc[d].n) {
                    best = d;
                    break;
                }
            if (best >= 0) {
                auto const& a = acc[best];
                double const k = 255.0 * kOwnShade[best] * a.n;
                ownTint = std::array<double, 3>{std::min(1.0, a.r / k), std::min(1.0, a.g / k), std::min(1.0, a.b / k)};
            }
        }
    }
    float const nudgeWay = !region ? 0.f : ((pos.x + pos.y + pos.z) & 1) ? 1.f : -1.f;
    bool const  freshMesh = positions.empty(); // nothing drawn yet: this sets the mesh's streams
    std::vector<GeoDraw const*> order;
    order.reserve(shapes.size());
    for (auto const& g : shapes) order.push_back(&g);
    if (slotView) {
        auto at = [](GeoDraw const& g, int k) { return glm::vec3{g.pos[size_t(k * 3)], g.pos[size_t(k * 3 + 1)], g.pos[size_t(k * 3 + 2)]}; };
        std::vector<std::pair<float, GeoDraw const*>> seen;
        for (auto const* g : order) {
            glm::vec3 const out = glm::cross(at(*g, 3) - at(*g, 0), at(*g, 1) - at(*g, 0)); // outward, model space
            if ((view * out).z >= 0) continue;                                                // the viewer is toward -z
            glm::vec3 const centre = (at(*g, 0) + at(*g, 1) + at(*g, 2) + at(*g, 3)) * 0.25f;
            seen.emplace_back((view * centre).z, g);
        }
        std::stable_sort(seen.begin(), seen.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        order.clear();
        for (auto const& [z, g] : seen) order.push_back(g);
    }
    for (auto const* gp : order) {
        auto const& g = *gp;
        if (g.cull >= 0 && g.cull < 6 && region) {
            BlockPos const np{pos.x + kDir[g.cull][0], pos.y + kDir[g.cull][1], pos.z + kDir[g.cull][2]};
            Block const&   nb = region->getBlock(np);
            if (nb.mCachedComponentData->mIsSolid) continue;
        }
        // Brightness: Java's per-direction shade (Bedrock's own faces use the same values).
        // Tint: only faces the model marks with a tintindex; grass-coloured blocks take the
        // biome grass colour, others the hue of the block's own (tinted) faces.
        static constexpr double kShade[6] = {0.5, 1.0, 0.8, 0.8, 0.6, 0.6};
        double const f     = (g.shade && g.dir >= 0 && g.dir < 6) ? kShade[g.dir] : 1.0;
        double       tr = 1, tg = 1, tb = 1;
        if (g.tintColor >= 0) {
            tr = ((g.tintColor >> 16) & 0xFF) / 255.0, tg = ((g.tintColor >> 8) & 0xFF) / 255.0, tb = (g.tintColor & 0xFF) / 255.0;
        } else if (g.tint >= 0) {
            if (grassTint) tr = grassTint->r, tg = grassTint->g, tb = grassTint->b;
            else if (ownTint) tr = (*ownTint)[0], tg = (*ownTint)[1], tb = (*ownTint)[2];
        }
        auto ch = [&](double t) { return uint32_t(std::clamp(255.0 * f * t + 0.5, 0.0, 255.0)); };
        uint32_t const color = ch(tr) | (ch(tg) << 8) | (ch(tb) << 16) | 0xFF000000u;
        int const   slot  = (g.dir >= 0 && g.dir < 6 && acc[g.dir].n) ? g.dir : 6;
        auto const& a     = acc[slot];
        // Light: the block's own faces toward the same side (a torch in front lights the
        // front, not the back); else all its faces; no own faces at all (a shape drawn on its
        // own): the mesh's last vertex.
        glm::vec2 light = a.n ? glm::vec2{float(a.l0 / a.n), float(a.l1 / a.n)}
                        : !uv1.empty() ? uv1.back() : glm::vec2{1, 1};
        glm::vec4 normal = a.normal;
        if (slot == 6 && g.dir >= 0 && g.dir < 6) normal = {float(kDir[g.dir][0]), float(kDir[g.dir][1]), float(kDir[g.dir][2]), 0.f};
        for (int k : {0, 3, 2, 1}) { // Bedrock's front faces wind the other way round
            float const u = g.uv[size_t(k * 2)], v = g.uv[size_t(k * 2 + 1)];
            tess.mNextUV[0].get() = {g.rect.u0 + u * (g.rect.u1 - g.rect.u0), g.rect.v0 + v * (g.rect.v1 - g.rect.v0)};
            if (!uv1.empty()) tess.mNextUV[1].get() = light;
            if (!colors.empty() || freshMesh) tess.mNextColor.get() = color;
            if (!normals.empty()) tess.mNextNormal.get() = normal;
            if (!pbr.empty()) tess.mNextPBRTextureIdx.get() = g.pbr ? g.pbr : pbrIdx;
            tess.vertex(pos.x + g.pos[size_t(k * 3)] + g.nudge[0] * nudgeWay, pos.y + g.pos[size_t(k * 3 + 1)] + g.nudge[1] * nudgeWay,
                        pos.z + g.pos[size_t(k * 3 + 2)] + g.nudge[2] * nudgeWay);
        }
    }
    // The layer belongs to the block type, world and item alike: only a block in the world
    // decides it (an item form's shape must not change how the placed block draws).
    if (region) ctm::ensureGeometryLayer(block, entry.cutout, entry.translucent);
    static std::mutex                     geoDiagMutex;
    static std::unordered_map<std::string, int> geoDiag;
    bool                                  logIt = false;
    {
        std::lock_guard lock(geoDiagMutex);
        logIt = gDebugLogs && ++geoDiag[entry.name + entry.statesText] <= 1;
    }
    if (gLog && logIt) {
        std::string dirs;
        for (int d = 0; d < 7; ++d)
            if (acc[d].n)
                dirs += std::format(" d{}:n{} rgb({:.0f},{:.0f},{:.0f},{:.0f}) l({:.3f},{:.3f})", d, acc[d].n, acc[d].r / acc[d].n,
                                    acc[d].g / acc[d].n, acc[d].b / acc[d].n, acc[d].a / acc[d].n, acc[d].l0 / acc[d].n, acc[d].l1 / acc[d].n);
        gLog->info("P5 {} light:{}", entry.name, dirs);
    }
    if (gLog && logIt)
        gLog->info("P5 {} @({},{},{}) [{}]: own quads {} folded, {} model quads, layer {}, first own vertex ({:.2f},{:.2f},{:.2f}) "
                   "expected corner ({:.2f},{:.2f},{:.2f}) applyTransform {}",
                   entry.name, pos.x, pos.y, pos.z, entry.statesText, (end - begin) / 4, entry.geometry.size(),
                   int(block.getBlockType().mRenderLayer), firstOwn.x, firstOwn.y, firstOwn.z,
                   (pos.x + tess.mPostTransformOffset->x) * tess.mPostTransformScale->x,
                   (pos.y + tess.mPostTransformOffset->y) * tess.mPostTransformScale->y,
                   (pos.z + tess.mPostTransformOffset->z) * tess.mPostTransformScale->z, bool(tess.mApplyTransform));
}

// A line in the chat area, so a key press visibly did something. Client thread only.
void say(std::string const& text) {
    try {
        if (auto client = ll::service::getClientInstance()) client->getGuiData()->displayClientMessage(text, std::nullopt, false);
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
LL_TYPE_STATIC_HOOK(
    InitBlocksHook, ll::memory::HookPriority::Normal, BlockGraphics, &BlockGraphics::initBlocks, void,
    ResourcePackManager& packs, Experiments const& experiments
) {
    origin(packs, experiments);
    loadRules(packs, "initBlocks");
}

LL_TYPE_STATIC_HOOK(
    ReloadBlockTexturesHook, ll::memory::HookPriority::Normal, BlockGraphics, &BlockGraphics::reloadBlockTextures, void,
    ResourcePackManager& packs, Experiments const& experiments
) {
    origin(packs, experiments);
    loadRules(packs, "reloadBlockTextures");
    gRebuildAllChunks.store(true, std::memory_order_release);
}

thread_local int tInsideBlockHook = 0; // inside TessellateInWorldHook's origin()

// Lowest priority = innermost: our UV rewrite happens before any other mod's
// post-processing of the same call (e.g. Arke reading UVs for mc_midTexCoord).
LL_TYPE_INSTANCE_HOOK(
    TessellateInWorldHook, ll::memory::HookPriority::Lowest, BlockTessellator, &BlockTessellator::tessellateInWorld,
    bool, Tessellator& tess, Block const& block, BlockPos const& pos, bool useCache
) {
    ++tCache.tessellateCalls;
    auto const* entry  = entryFor(block, pos, mRegion);
    bool const  ctmOn  = ctm::active() && mRegion;
    if (!entry && !ctmOn) return origin(tess, block, pos, useCache);
    size_t const before = tess.mMeshData->mPositions->size();
    ++tInsideBlockHook;
    bool const result = origin(tess, block, pos, useCache);
    --tInsideBlockHook;
    size_t const after = tess.mMeshData->mPositions->size();
    if (after > before) {
        if (entry && !entry->geometry.empty()) {
            applyGeometry(tess, before, after, *entry, block, pos, mRegion);
            // Connected textures on the model's faces too, as OptiFine does (matchTiles finds
            // the baked images a model uses): Fogg05's seamless glass panes.
            if (ctmOn) ctm::apply(tess, after, tess.mMeshData->mPositions->size(), block, pos, *mRegion);
        } else {
            if (entry) applyEntry(tess, before, after, *entry, pos, &block);
            if (ctmOn) ctm::apply(tess, before, after, block, pos, *mRegion);
        }
    }
    return result;
}

// A block drawn as an item (in the hand, in a slot, dropped) is tessellated through here.
LL_TYPE_INSTANCE_HOOK(
    AppendBlockHook, ll::memory::HookPriority::Normal, BlockTessellator, &BlockTessellator::appendTessellatedBlock, void,
    Tessellator& tess, Block const& block
) {
    ++tItemForm;
    size_t const before = tess.mMeshData->mPositions->size();
    size_t const calls  = tCache.tessellateCalls;
    origin(tess, block);
    size_t const after = tess.mMeshData->mPositions->size();
    // Plain cubes are built here without the hooked in-world functions: apply the rules now.
    if (tCache.tessellateCalls == calls && after > before) {
        try {
            BlockPos const pos{0, 0, 0};
            if (auto const* entry = entryFor(block, pos, nullptr)) {
                if (!entry->geometry.empty()) applyGeometry(tess, before, after, *entry, block, pos, nullptr);
                else applyEntry(tess, before, after, *entry, pos, &block);
            }
        } catch (...) {}
    }
    --tItemForm;
}

// Blocks in inventory slots are drawn through here: tessellated into the screen's
// tessellator and drawn at once, without the hooked in-world functions. The rules are
// applied to what was tessellated just before it is drawn.
thread_local Block const* tGuiBlock = nullptr;
thread_local size_t       tGuiCalls = 0;
thread_local size_t       tGuiBegin = 0; // where this block's vertices start in the screen's tessellator

void guiMesh(Tessellator& tess) {
    Block const* block = tGuiBlock;
    if (!block || tCache.tessellateCalls != tGuiCalls) return;
    try {
        size_t const   n     = tess.mMeshData->mPositions->size();
        size_t const   begin = std::min(tGuiBegin, n);
        BlockPos const pos{0, 0, 0};
        auto const*    entry = entryFor(*block, pos, nullptr);
        // Shapes the game cannot draw in a slot (a lantern's, a torch's) leave nothing here:
        // the pack's item shape is then drawn on its own.
        if (n <= begin && !(entry && !entry->geometry.empty())) return;
        if (entry) {
            if (!entry->geometry.empty()) applyGeometry(tess, begin, n, *entry, *block, pos, nullptr, true);
            else applyEntry(tess, begin, n, *entry, pos, block);
        }
        tGuiBegin = tess.mMeshData->mPositions->size(); // done up to here
    } catch (...) {}
}

LL_TYPE_INSTANCE_HOOK(
    GuiBlockHook, ll::memory::HookPriority::Normal, BlockTessellator, &BlockTessellator::renderGuiBlock, void,
    ScreenContext& screenContext, Block const& block, BlockGraphics const& blockGraphics, mce::TexturePtr const& texture,
    float lightMultiplier, float alphaMultiplier, OffscreenCaptureDescription const& capture
) {
    Block const* const outer      = tGuiBlock;
    size_t const       outerBegin = tGuiBegin;
    Tessellator&       tess       = screenContext.tessellator;
    tGuiBlock = &block;
    tGuiCalls = tCache.tessellateCalls;
    tGuiBegin = tess.mMeshData->mPositions->size();
    ++tItemForm;
    origin(screenContext, block, blockGraphics, texture, lightMultiplier, alphaMultiplier, capture);
    guiMesh(tess); // not drawn yet (batched with other slots): shape it now
    --tItemForm;
    tGuiBlock = outer;
    tGuiBegin = outerBegin;
}

using MeshTexture = std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture>;
LL_STATIC_HOOK(
    RenderMeshHook, ll::memory::HookPriority::Normal,
    static_cast<void (*)(ScreenContext&, Tessellator&, mce::MaterialPtr const&, OffscreenCaptureDescription const&)>(
        &MeshHelpers::renderMeshImmediately
    ),
    void, ScreenContext& screenContext, Tessellator& tess, mce::MaterialPtr const& material,
    OffscreenCaptureDescription const& capture
) {
    guiMesh(tess);
    origin(screenContext, tess, material, capture);
}
LL_STATIC_HOOK(
    RenderMeshTexHook, ll::memory::HookPriority::Normal,
    static_cast<void (*)(ScreenContext&, Tessellator&, mce::MaterialPtr const&, MeshTexture const&, OffscreenCaptureDescription const&)>(
        &MeshHelpers::renderMeshImmediately
    ),
    void, ScreenContext& screenContext, Tessellator& tess, mce::MaterialPtr const& material, MeshTexture const& texture,
    OffscreenCaptureDescription const& capture
) {
    guiMesh(tess);
    origin(screenContext, tess, material, texture, capture);
}

// Plain cubes (droppers, note blocks, lamps...) are built through this path
// directly by the chunk builder, without tessellateInWorld. When it is reached
// from inside tessellateInWorld, the outer hook already handles the block.
LL_TYPE_INSTANCE_HOOK(
    TessellateBlockInWorldHook, ll::memory::HookPriority::Lowest, BlockTessellator,
    &BlockTessellator::tessellateBlockInWorld, bool, Tessellator& tess, Block const& block, BlockPos const& pos,
    std::bitset<6> const faces, AirAndSimpleBlockBits const* simple
) {
    if (tInsideBlockHook) return origin(tess, block, pos, faces, simple);
    ++tCache.tessellateCalls;
    auto const* entry  = entryFor(block, pos, mRegion);
    bool const  ctmOn  = ctm::active() && mRegion;
    if (!entry && !ctmOn) return origin(tess, block, pos, faces, simple);
    size_t const before = tess.mMeshData->mPositions->size();
    bool const   result = origin(tess, block, pos, faces, simple);
    size_t const after  = tess.mMeshData->mPositions->size();
    if (after > before) {
        if (entry && !entry->geometry.empty()) {
            applyGeometry(tess, before, after, *entry, block, pos, mRegion);
            // Connected textures on the model's faces too, as OptiFine does (matchTiles finds
            // the baked images a model uses): Fogg05's seamless glass panes.
            if (ctmOn) ctm::apply(tess, after, tess.mMeshData->mPositions->size(), block, pos, *mRegion);
        } else {
            if (entry) applyEntry(tess, before, after, *entry, pos, &block);
            if (ctmOn) ctm::apply(tess, before, after, block, pos, *mRegion);
        }
    }
    return result;
}

// Diagnostics only: tells whether chunk building goes through tessellateInWorld
// or the new data-driven pipeline (which would bypass the hook above).
LL_TYPE_INSTANCE_HOOK(
    ChunkBuildHook, ll::memory::HookPriority::Normal, RenderChunkBuilder, &RenderChunkBuilder::build, void,
    RenderChunkGeometry& geometry, bool transparentLeaves, BakedBlockLightType lightingType, bool forExport,
    mce::framebuilder::FrameLightingModelCapabilities const& caps
) {
    if (gBuildDiagBudget.load(std::memory_order_relaxed) <= 0) {
        origin(geometry, transparentLeaves, lightingType, forExport, caps);
        return;
    }
    size_t const callsBefore = tCache.tessellateCalls;
    origin(geometry, transparentLeaves, lightingType, forExport, caps);
    if (gLog && gBuildDiagBudget.fetch_sub(1) > 0)
        gLog->info(
            "chunk build: newTessellationPipeline={} tessellateInWorld calls={}",
            static_cast<bool>(mUseNewTessellationPipeline), tCache.tessellateCalls - callsBefore
        );
}

// Tuning a note block does not resend its block entity to the client, but the
// block event that plays the note carries instrument and pitch.
LL_TYPE_INSTANCE_HOOK(
    NoteEventHook, ll::memory::HookPriority::Normal, NoteBlock, &NoteBlock::$triggerEvent, void, BlockSource& region,
    BlockPos const& pos, int instrument, int note
) {
    origin(region, pos, instrument, note);
    try {
        world::noteEvent(int(region.getDimensionId()), pos.x, pos.y, pos.z, instrument, note);
    } catch (...) {}
}

// Client thread: refresh block-entity snapshots and rebuild chunks whose values changed.
LL_TYPE_INSTANCE_HOOK(
    CoordinatorTickHook, ll::memory::HookPriority::Normal, RenderChunkCoordinator, &RenderChunkCoordinator::tick, void
) {
    origin();
    try {
        world::tick(*this);
    } catch (...) {}
    // Textures and rules were reloaded with the world open: every chunk mesh still
    // carries the old atlas positions.
    if (gRebuildQuiet.exchange(false, std::memory_order_acq_rel)) {
        _setAllDirty(false, false);
        // Diagnostics: how often every chunk is rebuilt (should be rare).
        static int  quiet = 0;
        static auto last  = std::chrono::steady_clock::now();
        ++quiet;
        if (auto now = std::chrono::steady_clock::now(); now - last > std::chrono::seconds(5)) {
            if (gLog) gLog->info("all-chunk rebuilds in the last {} s: {}", std::chrono::duration_cast<std::chrono::seconds>(now - last).count(), quiet);
            quiet = 0;
            last  = now;
        }
    }
    if (gRebuildAllChunks.exchange(false, std::memory_order_acq_rel)) {
        _setAllDirty(false, false);
        if (gLog) gLog->info("resources reloaded in-world: all chunks marked for rebuild");
        auto const rules = rulesSnapshot();
        say(std::format("Tessera: resource packs reloaded ({} rules)", rules ? rules->rules.size() : 0));
    }
}

LL_TYPE_INSTANCE_HOOK(
    UploadTextureHook, ll::memory::HookPriority::Normal, mce::TextureGroup,
    static_cast<BedrockTexture& (mce::TextureGroup::*)(ResourceLocation const&, cg::ImageBuffer)>(
        &mce::TextureGroup::uploadTexture
    ),
    BedrockTexture&, ResourceLocation const& location, cg::ImageBuffer image
) {
    if (kRecordUploads && !tReplayingUploads) {
        try {
            auto const&  desc  = *image.mImageDescription;
            size_t const bytes = size_t(desc.mWidth) * desc.mHeight * 4;
            if (gLog && gUploadDiag.fetch_sub(1) > 0)
                gLog->info("texture upload: '{}' {}x{}", location.mPath->value, uint32_t(desc.mWidth), uint32_t(desc.mHeight));
            std::lock_guard lock(gUploadMutex);
            uint64_t const  key = location.mFullHash;
            if (auto it = gUploaded.find(key); it != gUploaded.end()) {
                gUploadedBytes -= it->second->bytes;
                gUploaded.erase(it);
            }
            if (bytes && bytes <= (4u << 20) && gUploadedBytes + bytes <= kUploadedLimit) {
                gUploaded.emplace(key, std::make_unique<Uploaded>(location, image, bytes));
                gUploadedBytes += bytes;
            }
        } catch (...) {}
    }
    BedrockTexture& result = origin(location, std::move(image));
    if (kRecordUploads && !tReplayingUploads) {
        std::lock_guard lock(gUploadMutex);
        if (auto it = gUploaded.find(location.mFullHash); it != gUploaded.end()) it->second->data = colourOf(result).get();
    }
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    UnloadTextureHook, ll::memory::HookPriority::Normal, mce::TextureGroup, &mce::TextureGroup::unloadTexture, bool,
    ResourceLocation const& location, bool assertOnFailure
) {
    {
        std::lock_guard lock(gUploadMutex);
        if (auto it = gUploaded.find(location.mFullHash); it != gUploaded.end()) {
            gUploadedBytes -= it->second->bytes;
            gUploaded.erase(it);
        }
    }
    return origin(location, assertOnFailure);
}

// Put the remembered textures back after a reload. In place where the texture
// object survived; otherwise as a new upload, and then whoever still holds the old,
// emptied object (a player's skin) is pointed at the new one.
void replayUploads(mce::TextureGroup& group, ClientInstance* client) {
    std::vector<Uploaded*> items;
    {
        std::lock_guard lock(gUploadMutex);
        for (auto const& [key, item] : gUploaded) items.push_back(item.get());
    }
    std::unordered_map<void const*, TextureData> renewed; // old object -> its replacement
    int                                          inPlace = 0;
    tReplayingUploads                                    = true;
    for (auto* item : items) {
        try {
            if (group.updateTextureInPlace(item->location, cg::ImageBuffer(item->image))) {
                ++inPlace;
                continue;
            }
            TextureData const& fresh = colourOf(group.uploadTexture(item->location, cg::ImageBuffer(item->image)));
            if (item->data) renewed.emplace(item->data, fresh);
            if (gLog) gLog->info("hot reload: re-uploaded '{}' ({} -> {})", item->location.mPath->value, item->data, (void const*)fresh.get());
            item->data = fresh.get();
        } catch (...) {}
    }
    tReplayingUploads = false;

    int skins = 0, repointed = 0;
    try {
        auto* player = client ? client->getLocalPlayer() : nullptr;
        // Type-erased access: a typed view would need ClientSkin's constructors,
        // which the game does not export.
        auto* set = player ? player->getEntityContext().getRegistry().storage(entt::type_hash<ClientSkin>::value()) : nullptr;
        if (set) {
            for (auto const entity : *set) {
                auto* skin = static_cast<ClientSkin*>(set->value(entity));
                if (!skin) continue;
                ++skins;
                auto& info = *reinterpret_cast<ClientSkinInfoData*>(std::addressof(skin->mSkinInfoData));
                if (gLog)
                    gLog->info("hot reload: skin {} initialized={} loadingGeometry={} refs={} dirty={} valid={}", skins,
                               bool(skin->mInitialized), bool(skin->mIsLoadingGeometry), int(skin->mReferenceCount),
                               bool(info.mIsDirty), bool(info.mValid));
                auto  fix  = [&](BedrockTexture& texture, char const* what) {
                    TextureData& held  = colourOf(texture);
                    auto const   it    = renewed.find(held.get());
                    int const    state = held ? int(uchar(*reinterpret_cast<TextureLoadState*>(std::addressof(held->mTextureLoadState)))) : -1;
                    if (gLog)
                        gLog->info("hot reload: skin {} {} texture {} state {}{}", skins, what, (void const*)held.get(), state,
                                   it != renewed.end() ? " -> repointed" : "");
                    if (it != renewed.end()) {
                        held = it->second;
                        ++repointed;
                    }
                };
                fix(*reinterpret_cast<BedrockTexture*>(std::addressof(info.mSkinTexture)), "skin");
                fix(*reinterpret_cast<BedrockTexture*>(std::addressof(info.mCapeTexture)), "cape");
                fix(*reinterpret_cast<BedrockTexture*>(std::addressof(info.mLodSkinTexture)), "lod");
            }
        }
    } catch (...) {
        if (gLog) gLog->error("hot reload: repointing skins threw");
    }
    if (gLog)
        gLog->info("hot reload: uploaded textures restored: {} in place, {} as new; {} skin(s), {} texture(s) repointed",
                   inPlace, renewed.size(), skins, repointed);
}

// Player skins survive a reload as textures, but the reload re-reads the entity
// definitions and the skin's geometry and render setup go with the old ones, so the
// player turns invisible. The game sets a skin up again when it finds it marked
// uninitialised (and refreshes its data when marked dirty); its own
// ClientSkinSystem::reload, which would do this, is not exported.
std::chrono::steady_clock::time_point gSkinRefreshStart;
int                                   gSkinRefreshNext = 0; // 1..2 while passes are due (main thread only)

int refreshSkins(ClientInstance& client) {
    int   skins  = 0;
    auto* player = client.getLocalPlayer();
    auto* set = player ? player->getEntityContext().getRegistry().storage(entt::type_hash<ClientSkin>::value()) : nullptr;
    if (!set) return 0;
    for (auto const entity : *set) {
        auto* skin = static_cast<ClientSkin*>(set->value(entity));
        if (!skin) continue;
        ++skins;
        auto& info         = *reinterpret_cast<ClientSkinInfoData*>(std::addressof(skin->mSkinInfoData));
        info.mIsDirty      = true;
        skin->mInitialized = false;
    }
    return skins;
}

// The game rebuilds the terrain atlas when its packs change, but with a world open
// it stops there: block textures are only looked up again when a world loads.
LL_TYPE_INSTANCE_HOOK(
    CreateAtlasHook, ll::memory::HookPriority::Normal, TextureAtlas, &TextureAtlas::_createAtlas,
    TextureAtlasResourceCallbacks, Json::Value const& root, std::function<void(TextureAtlasStatus const&)> status,
    std::function<void()> const finished, std::shared_ptr<mce::TextureGroup> group, cg::MipMapSupport const mip,
    ResourcePackManager& packs, std::shared_ptr<cg::ImageBuffer> outFinal,
    std::shared_ptr<std::unordered_map<ResourceLocation, cg::TextureSetImageContainer>> outSources
) {
    bool terrain = false;
    try {
        terrain = group && root["texture_name"].asString("") == "atlas.terrain";
    } catch (...) {}
    if (!terrain || gHotStage.load(std::memory_order_acquire) != 1)
        return origin(root, std::move(status), finished, std::move(group), mip, packs, std::move(outFinal), std::move(outSources));
    std::function<void()> const done = [finished] {
        if (finished) finished();
        int expected = 1;
        gHotStage.compare_exchange_strong(expected, 2, std::memory_order_acq_rel);
    };
    return origin(root, std::move(status), done, std::move(group), mip, packs, std::move(outFinal), std::move(outSources));
}

// Reload resource packs without leaving the world (Java's F3+T; T opens chat
// here, so the key is F3+R). Experimental.
LL_TYPE_INSTANCE_HOOK(EndFrameHook, ll::memory::HookPriority::Normal, MinecraftGame, &MinecraftGame::endFrame, void) {
    origin();
    packmenu::frame();
    apitest::frame();
    static int waited = 0; // frames spent in the current stage
    int const  stage  = gHotStage.load(std::memory_order_acquire);
    if (stage == 0) waited = 0;
    else ++waited;
    if (stage == 1 && waited > 60 * 30) { // the atlas never came back
        gHotStage.store(0, std::memory_order_release);
        if (gLog) gLog->warn("hot reload: no terrain atlas after the reload request; giving up");
        say("Tessera: reload did not finish");
    }
    if (stage == 2 && waited > 3) { // a few frames after the atlas, as the textures settle
        gHotStage.store(0, std::memory_order_release);
        try {
            auto client = ll::service::getClientInstance();
            auto* level = client ? client->getLevel() : nullptr;
            if (level) {
                // Experiments adds no data to ExperimentStorage, which is what LevelData keeps.
                auto const& experiments = static_cast<Experiments const&>(*level->getLevelData().mExperiments);
                if (gLog) gLog->info("hot reload: terrain atlas ready, reloading block textures");
                BlockGraphics::reloadBlockTextures(getResourcePackManager(), experiments);
                if (auto group = getTextureGroup()) replayUploads(*group, &*client);
                packmenu::logStacks("after reload");
                gSkinRefreshStart = std::chrono::steady_clock::now();
                gSkinRefreshNext  = 1;
            } else if (gLog) {
                gLog->info("hot reload: atlas ready but no world is open");
            }
        } catch (...) {
            if (gLog) gLog->error("hot reload: reloading block textures threw");
        }
    }
    // Two passes, 2 s and 6 s after the reload: the entity definitions finish
    // reloading some time after the terrain atlas, and a pass before that is lost.
    if (gSkinRefreshNext >= 1 && gSkinRefreshNext <= 2
        && std::chrono::steady_clock::now() - gSkinRefreshStart > std::chrono::seconds(gSkinRefreshNext == 1 ? 2 : 6)) {
        int const pass = gSkinRefreshNext++;
        try {
            if (auto client = ll::service::getClientInstance()) {
                int const n = refreshSkins(*client);
                if (gLog) gLog->info("hot reload: skin refresh pass {}: {} skin(s)", pass, n);
            }
        } catch (...) {
            if (gLog) gLog->error("hot reload: skin refresh threw");
        }
    }
#ifdef _WIN32
    static bool wasDown = false;
    bool        down    = false;
    DWORD       pid     = 0;
    if (HWND const fg = GetForegroundWindow(); fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId())
        down = (GetAsyncKeyState(VK_F3) & 0x8000) && (GetAsyncKeyState('R') & 0x8000);
    bool const pressed   = down && !wasDown;
    wasDown              = down;
    bool const requested = gReloadRequested.exchange(false);
    if ((!pressed && !requested) || stage != 0) return;
    try {
        ResourcePackManager& packs = getResourcePackManager();
        if (gLog) gLog->info("hot reload requested, inGame={}", isInGame());
        say("Tessera: reloading resource packs...");
        gHotStage.store(1, std::memory_order_release);
        waited = 0;
        // The listener notification (onActiveResourcePacksChanged) returns without
        // doing anything in-world; this is the routine behind it.
        _onActiveResourcePacksChanged(packs, true);
    } catch (...) {
        gHotStage.store(0, std::memory_order_release);
        if (gLog) gLog->error("hot reload threw");
    }
#endif
}

bool gInstalled = false;

} // namespace

void requestHotReload() { gReloadRequested.store(true); }

bool blockHasItemShape(std::string const& blockName) {
    auto const rules = rulesSnapshot();
    return rules && rules->itemBlocks.contains(blockName);
}

bool debugLogging() { return gDebugLogs; }

bool blockHasRules(std::string const& blockName) {
    auto const rules = rulesSnapshot();
    return rules && rules->hasBlock(blockName);
}

void requestChunkRebuild() { gRebuildQuiet.store(true); }

void invalidateCaches() {
    gGeneration.fetch_add(1, std::memory_order_acq_rel);
    gRebuildQuiet.store(true);
}

void chat(std::string const& text) { say(text); }

bool install(ll::io::Logger& logger, std::filesystem::path const& dataDir) {
    if (gInstalled) return true;
    gLog = &logger;
    world::setLogger(&logger);
    ctm::setLogger(&logger);
    ctm::setDiagFile(dataDir / "ctm_diag.log");
    gDataDir = dataDir;
    {
        std::error_code ec;
        gDebugLogs = std::filesystem::exists(dataDir / "debug.txt", ec);
        gDiagBudget.store(gDebugLogs ? 500 : 0);
        gBuildDiagBudget.store(gDebugLogs ? 6 : 0);
        if (gDebugLogs) logger.info("debug logging on (data/debug.txt)");
    }
    int failures = 0;
    failures += InitBlocksHook::hook() != 0;
    failures += ReloadBlockTexturesHook::hook() != 0;
    failures += TessellateInWorldHook::hook() != 0;
    failures += TessellateBlockInWorldHook::hook() != 0;
    failures += AppendBlockHook::hook() != 0;
    failures += GuiBlockHook::hook() != 0;
    failures += RenderMeshHook::hook() != 0;
    failures += RenderMeshTexHook::hook() != 0;
    failures += ChunkBuildHook::hook() != 0;
    failures += CoordinatorTickHook::hook() != 0;
    failures += NoteEventHook::hook() != 0;
    failures += EndFrameHook::hook() != 0;
    failures += CreateAtlasHook::hook() != 0;
    failures += UploadTextureHook::hook() != 0;
    failures += UnloadTextureHook::hook() != 0;
    // The in-world pack menu (F3+P) cannot switch packs yet: only with data/pack_menu.txt.
    if (std::error_code ec; std::filesystem::exists(dataDir / "pack_menu.txt", ec)) failures += !packmenu::install(logger);
    failures += !javapacks::install(logger, dataDir);
    failures += !items::install(logger, dataDir);
    if (failures) logger.error("{} hook(s) failed to install", failures);
    gInstalled = true;
    return failures == 0;
}

void uninstall() {
    if (!gInstalled) return;
    items::uninstall();
    javapacks::uninstall();
    packmenu::uninstall();
    UnloadTextureHook::unhook();
    UploadTextureHook::unhook();
    CreateAtlasHook::unhook();
    EndFrameHook::unhook();
    NoteEventHook::unhook();
    CoordinatorTickHook::unhook();
    ChunkBuildHook::unhook();
    TessellateBlockInWorldHook::unhook();
    RenderMeshTexHook::unhook();
    RenderMeshHook::unhook();
    GuiBlockHook::unhook();
    AppendBlockHook::unhook();
    TessellateInWorldHook::unhook();
    ReloadBlockTexturesHook::unhook();
    InitBlocksHook::unhook();
    gInstalled = false;
}

} // namespace tessera::game
