#include "game/JavaPacks.h"
#include "core/java/ZipAssets.h"

#include "core/java/JavaPack.h"
#include "game/Ctm.h"
#include "game/Items.h"

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/renderer/TextureGroup.h"
#include "mc/client/renderer/texture/TextureAtlas.h"
#include "mc/client/renderer/texture/TextureAtlasResourceCallbacks.h"
#include "mc/deps/core/container/Blob.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/resource/ResourceLocationPair.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/core_graphics/TextureSetImageContainer.h"
#include "mc/deps/json/Value.h"
#include "mc/deps/core/file/Path.h"
#include "mc/deps/core/resource/PackOrigin.h"
#include "mc/resources/Pack.h"
#include "mc/resources/PackInstance.h"
#include "mc/scripting/modules/PackManifest.h"
#include "mc/resources/ResourcePack.h"
#include "mc/resources/ResourcePackLoader.h"
#include "mc/resources/ResourcePackManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

namespace tessera::game::javapacks {
namespace {

ll::io::Logger* gLog = nullptr;

// Where the baked images live, as far as the atlas is concerned: no pack holds
// them, the load hooks below hand them out.
constexpr std::string_view kSpriteDir = "textures/tessera_java/";

std::mutex                                   gMutex;
std::unordered_map<std::string, std::string> gPng;   // sprite path (no extension) -> PNG bytes

// Replace the served images whose names start with `prefix` (one kind: baked faces,
// CTM tiles or CIT icons; the atlases are built at different times). Lock held.
void replaceSprites(std::string_view prefix, std::unordered_map<std::string, std::string> fresh) {
    std::string const start = std::string(kSpriteDir) + std::string(prefix);
    for (auto it = gPng.begin(); it != gPng.end();) {
        if (it->first.starts_with(start)) it = gPng.erase(it);
        else ++it;
    }
    for (auto& [k, v] : fresh) gPng[k] = std::move(v);
}
std::string                                  gRules; // generated rules JSON
std::unordered_map<std::string, std::vector<std::string>> gByTexture; // Java texture -> baked images using it

ResourceLocation userPath(std::string const& path) { return ResourceLocation{Core::PathView{path}, ResourceFileSystem::UserPackage}; }

uint64_t fnv(void const* data, size_t size, uint64_t h = 1469598103934665603ull) {
    auto const* p = static_cast<unsigned char const*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

constexpr std::string_view kBlockstates = "assets/minecraft/blockstates/";

// The active pack stack, seen as one Java pack (the highest pack holding a file wins).
// Diagnostics: when data/dump_fallback.txt exists, stand-in textures are written to data/fallback/.
std::filesystem::path gDumpDir;

class StackAssets : public java::Assets {
public:
    explicit StackAssets(ResourcePackManager& packs) : mPacks(packs) {}

    std::optional<std::string> read(std::string const& path) const override {
        std::string out;
        try {
            if (static_cast<ResourceLoader const&>(mPacks).load(userPath(path), out)) return out;
        } catch (...) {}
        return std::nullopt;
    }

    // Every pack's copy of a blockstate file, highest pack first. Only files that list()
    // saw in more than one pack are looked up pack by pack.
    std::vector<std::string> readAll(std::string const& path) const override {
        std::vector<std::string> out;
        auto const               top = read(path);
        if (!top) return out;
        out.push_back(*top);
        if (!path.starts_with(kBlockstates)) return out;
        (void)list(std::string(kBlockstates));
        {
            std::lock_guard lock(mListMutex);
            if (auto it = mCopies.find(path); it == mCopies.end() || it->second < 2) return out;
        }
        try {
            mPacks.iteratePacks([&](PackInstance const& instance) {
                std::string text;
                if (instance.mPack->getResource(Core::PathView{path}, text, instance.mSubpackIndex)
                    && std::find(out.begin(), out.end(), text) == out.end())
                    out.push_back(std::move(text));
            });
        } catch (...) {
            if (gLog) gLog->warn("Java packs: reading every copy of {} failed", path);
        }
        return out;
    }

    // Files under assets/minecraft/<blockstates|models|textures>/ of every pack in the stack
    // (blockstates directly in that folder; models and textures in any subfolder).
    std::vector<std::string> list(std::string const& prefix) const override {
        if (!prefix.starts_with("assets/minecraft/") || !prefix.ends_with("/")) return {};
        {
            std::lock_guard lock(mListMutex);
            if (auto it = mListed.find(prefix); it != mListed.end()) return it->second;
        }
        bool const            flat = prefix == kBlockstates;
        std::set<std::string>      names;
        std::map<std::string, int> copies; // blockstate file -> packs holding it
        int                        packs = 0;
        try {
            mPacks.iteratePacks([&](PackInstance const& instance) {
                ++packs;
                ResourcePack const& pack = *instance.mPack;
                pack.forEachIn(
                    Core::Path{prefix.substr(0, prefix.size() - 1)},
                    [&](Core::Path const& file) {
                        std::string path = file.value;
                        std::replace(path.begin(), path.end(), '\\', '/');
                        auto const at = path.rfind(prefix);
                        if (at == std::string::npos) return;
                        std::string const name = path.substr(at + prefix.size());
                        if (flat && name.find('/') != std::string::npos) return;
                        if (name.ends_with(".json") || name.ends_with(".png")) {
                            names.insert(name);
                            if (flat) ++copies[prefix + name];
                        }
                    },
                    instance.mSubpackIndex, !flat
                );
            });
        } catch (...) {
            if (gLog) gLog->warn("Java packs: listing pack files failed");
        }
        std::vector<std::string> out;
        for (auto const& n : names) out.push_back(prefix + n);
        if (gLog && flat && !out.empty()) gLog->info("Java packs: {} blockstate file(s) in {} pack(s)", out.size(), packs);
        std::lock_guard lock(mListMutex);
        mListed[prefix] = out;
        if (flat) mCopies = std::move(copies);
        return out;
    }

    // A Bedrock texture for a Java name that a player-installed pack (not the game's own)
    // holds: the pack's pictures win over the vanilla Java ones.
    std::optional<java::Image> userBedrockTexture(std::string const& javaName) const {
        {
            std::lock_guard lock(mListMutex);
            if (!mUserTextures) {
                mUserTextures.emplace();
                try {
                    mPacks.iteratePacks([&](PackInstance const& instance) {
                        ResourcePack const& pack = *instance.mPack;
                        PackManifest const* m    = nullptr;
                        try {
                            m = pack.mImpl.get()->mPack.get()->mManifest.get();
                        } catch (...) {}
                        if (!m) return;
                        auto const origin = PackOrigin(m->mPackOrigin);
                        if (origin != PackOrigin::User && origin != PackOrigin::Dev) return;
                        for (char const* dir : {"textures/blocks", "textures/items"})
                            pack.forEachIn(
                                Core::Path{std::string(dir)},
                                [&](Core::Path const& file) {
                                    std::string p = file.value;
                                    std::replace(p.begin(), p.end(), '\\', '/');
                                    auto const at = p.find(dir);
                                    if (at == std::string::npos) return;
                                    p = p.substr(at);
                                    if (auto dot = p.rfind('.'); dot != std::string::npos && dot > p.rfind('/')) p.resize(dot);
                                    mUserTextures->insert(p);
                                },
                                instance.mSubpackIndex, true
                            );
                    });
                } catch (...) {}
            }
            std::string const dir = javaName.starts_with("item/") ? "textures/items/" : "textures/blocks/";
            bool              any = false;
            for (auto const& name : java::bedrockTextureNames(javaName)) any |= mUserTextures->contains(dir + name);
            if (!any) return std::nullopt;
        }
        return bedrockTexture(javaName);
    }

    std::optional<java::Image> bedrockTexture(std::string const& javaName) const {
        // Java "item/x" textures live under Bedrock's textures/items/.
        std::string const dir = javaName.starts_with("item/") ? "textures/items/" : "textures/blocks/";
        for (auto const& name : java::bedrockTextureNames(javaName)) {
            try {
                mce::Image image =
                    static_cast<ResourceLoader const&>(mPacks).loadTexture(userPath(dir + name));
                if (image.isEmpty() || image.imageFormat != mce::ImageFormat::RGBA8Unorm || image.mDepth > 1) continue;
                size_t const n = size_t(image.mWidth) * size_t(image.mHeight);
                if (image.mImageBytes.size() < n * 4) continue;
                java::Image out;
                out.width  = int(image.mWidth);
                out.height = int(image.mHeight);
                out.pixels.resize(n);
                std::memcpy(out.pixels.data(), image.mImageBytes.data(), n * 4);
                // Bedrock's dust line runs east-west; Java's line0/line1 run north-south.
                if ((javaName.ends_with("redstone_dust_line0") || javaName.ends_with("redstone_dust_line1"))
                    && name == "redstone_dust_line" && out.width == out.height) {
                    java::Image turned = out;
                    int const   w      = out.width;
                    for (int y = 0; y < w; ++y)
                        for (int x = 0; x < w; ++x) turned.pixels[size_t(y) * size_t(w) + size_t(x)] = out.pixels[size_t(x) * size_t(w) + size_t(w - 1 - y)];
                    out = std::move(turned);
                }
                if (!gDumpDir.empty()) {
                    std::string file = javaName;
                    std::replace(file.begin(), file.end(), '/', '_');
                    std::ofstream(gDumpDir / (file + "__" + name + ".png"), std::ios::binary) << java::encodePng(out);
                }
                return out;
            } catch (...) {}
        }
        return std::nullopt;
    }

private:
    ResourcePackManager&                            mPacks;
    mutable std::mutex                                        mListMutex;
    mutable std::map<std::string, std::vector<std::string>> mListed;
    mutable std::map<std::string, int>                      mCopies;
    mutable std::optional<std::set<std::string>>            mUserTextures; // textures/blocks|items/<name> in user packs
};

// ---------------------------------------------------------------------------
// Cache: a bake is kept on disk with a fingerprint of everything it read (files,
// listings, stand-in textures). The next atlas build re-reads those inputs (cheap)
// and reuses the bake when nothing changed.
// ---------------------------------------------------------------------------
struct Input {
    char        kind; // 'r' read, 'l' list, 't' stand-in texture
    std::string key;
    uint64_t    hash;
};

uint64_t hashOf(std::optional<std::string> const& bytes) { return bytes ? fnv(bytes->data(), bytes->size()) | 1 : 0; }
uint64_t hashOf(std::vector<std::string> const& list) {
    uint64_t h = 1469598103934665603ull;
    for (auto const& s : list) h = fnv(s.data(), s.size() + 1, h); // includes the terminating 0 as a separator
    return h | 1;
}
uint64_t hashOf(std::optional<java::Image> const& img) {
    if (!img) return 0;
    uint64_t h = fnv(&img->width, sizeof(int));
    h          = fnv(&img->height, sizeof(int), h);
    return fnv(img->pixels.data(), img->pixels.size() * 4, h) | 1;
}

class Recorder : public java::Assets {
public:
    explicit Recorder(StackAssets const& inner) : mInner(inner) {}
    std::optional<std::string> read(std::string const& path) const override {
        auto out = mInner.read(path);
        std::lock_guard lock(mMutex);
        mInputs.push_back({'r', path, hashOf(out)});
        return out;
    }
    std::vector<std::string> list(std::string const& prefix) const override {
        auto out = mInner.list(prefix);
        std::lock_guard lock(mMutex);
        mInputs.push_back({'l', prefix, hashOf(out)});
        return out;
    }
    std::vector<std::string> readAll(std::string const& path) const override {
        auto out = mInner.readAll(path);
        std::lock_guard lock(mMutex);
        mInputs.push_back({'a', path, hashOf(out)});
        return out;
    }
    std::optional<java::Image> texture(std::string const& name) const {
        auto out = mInner.bedrockTexture(name);
        std::lock_guard lock(mMutex);
        mInputs.push_back({'t', name, hashOf(out)});
        return out;
    }
    std::optional<java::Image> userTexture(std::string const& name) const {
        auto out = mInner.userBedrockTexture(name);
        std::lock_guard lock(mMutex);
        mInputs.push_back({'u', name, hashOf(out)});
        return out;
    }
    std::vector<Input> inputs() const {
        std::lock_guard lock(mMutex);
        return mInputs;
    }
    void note(char kind, std::string key, uint64_t hash) const {
        std::lock_guard lock(mMutex);
        mInputs.push_back({kind, std::move(key), hash});
    }

private:
    StackAssets const&         mInner;
    mutable std::mutex         mMutex;
    mutable std::vector<Input> mInputs;
};

struct Baked {
    std::string                                       version;
    std::vector<Input>                                inputs;
    std::string                                       rules;
    std::vector<std::pair<std::string, std::string>> pngs;    // texture name -> PNG
    std::vector<std::pair<std::string, std::string>> sources; // texture name -> a Java texture drawn into it
};

void put(std::string& out, uint64_t v) { out.append(reinterpret_cast<char const*>(&v), 8); }
void put(std::string& out, std::string const& s) {
    put(out, uint64_t(s.size()));
    out += s;
}
bool get(std::string_view& in, uint64_t& v) {
    if (in.size() < 8) return false;
    std::memcpy(&v, in.data(), 8);
    in.remove_prefix(8);
    return true;
}
bool get(std::string_view& in, std::string& s) {
    uint64_t n = 0;
    if (!get(in, n) || in.size() < n) return false;
    s.assign(in.data(), size_t(n));
    in.remove_prefix(size_t(n));
    return true;
}

constexpr std::string_view kMagic = "TesseraJavaBake2";

std::string serialise(Baked const& b) {
    std::string out(kMagic);
    put(out, b.version);
    put(out, uint64_t(b.inputs.size()));
    for (auto const& i : b.inputs) {
        out += i.kind;
        put(out, i.key);
        put(out, i.hash);
    }
    put(out, b.rules);
    put(out, uint64_t(b.pngs.size()));
    for (auto const& [name, png] : b.pngs) {
        put(out, name);
        put(out, png);
    }
    put(out, uint64_t(b.sources.size()));
    for (auto const& [name, source] : b.sources) {
        put(out, name);
        put(out, source);
    }
    return out;
}

std::optional<Baked> deserialise(std::string_view in) {
    if (!in.starts_with(kMagic)) return std::nullopt;
    in.remove_prefix(kMagic.size());
    Baked    b;
    uint64_t n = 0;
    if (!get(in, b.version) || !get(in, n)) return std::nullopt;
    for (uint64_t k = 0; k < n; ++k) {
        Input i;
        if (in.empty()) return std::nullopt;
        i.kind = in.front();
        in.remove_prefix(1);
        if (!get(in, i.key) || !get(in, i.hash)) return std::nullopt;
        b.inputs.push_back(std::move(i));
    }
    if (!get(in, b.rules) || !get(in, n)) return std::nullopt;
    for (uint64_t k = 0; k < n; ++k) {
        std::string name, png;
        if (!get(in, name) || !get(in, png)) return std::nullopt;
        b.pngs.emplace_back(std::move(name), std::move(png));
    }
    if (!get(in, n)) return std::nullopt;
    for (uint64_t k = 0; k < n; ++k) {
        std::string name, source;
        if (!get(in, name) || !get(in, source)) return std::nullopt;
        b.sources.emplace_back(std::move(name), std::move(source));
    }
    return b;
}

std::filesystem::path gCacheFile;
// Vanilla Java assets from the player's own Java client jar (models and blockstates packs
// build on); null when there is none.
std::unique_ptr<java::ZipAssets> gVanilla;
std::filesystem::path            gVanillaPath;
uint64_t                         gVanillaStamp = 0; // size and write time: part of the bake's inputs

std::optional<Baked> loadCache(StackAssets const& assets) {
    if (gCacheFile.empty()) return std::nullopt;
    std::ifstream in(gCacheFile, std::ios::binary);
    if (!in) return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto        b = deserialise(bytes);
    if (!b || b->version != java::bakerVersion()) return std::nullopt;
    for (auto const& i : b->inputs) {
        uint64_t now = 0;
        switch (i.kind) {
        case 'r': now = hashOf(assets.read(i.key)); break;
        case 'l': now = hashOf(assets.list(i.key)); break;
        case 't': now = hashOf(assets.bedrockTexture(i.key)); break;
        case 'u': now = hashOf(assets.userBedrockTexture(i.key)); break;
        case 'v': now = gVanillaPath.string() == i.key ? gVanillaStamp : 0; break;
        case 'a': now = hashOf(assets.readAll(i.key)); break;
        default: return std::nullopt;
        }
        if (now != i.hash) {
            if (gLog) gLog->info("Java packs: cached bake is out of date ({} changed)", i.key);
            return std::nullopt;
        }
    }
    return b;
}

void saveCache(Baked const& b) {
    if (gCacheFile.empty()) return;
    try {
        std::filesystem::create_directories(gCacheFile.parent_path());
        std::ofstream out(gCacheFile, std::ios::binary | std::ios::trunc);
        out << serialise(b);
    } catch (...) {
        if (gLog) gLog->warn("Java packs: could not write the bake cache");
    }
}

// Bakes the stack's Java assets (or reuses the cached bake); returns the atlas
// entries to add (texture name -> sprite path).
std::vector<std::pair<std::string, std::string>> bakeStack(ResourcePackManager& packs) {
    auto const  start = std::chrono::steady_clock::now();
    auto        ms    = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    };
    StackAssets assets(packs);
    if (assets.list(std::string(kBlockstates)).empty()
        && (!gVanilla || (assets.list("assets/minecraft/models/").empty() && assets.list("assets/minecraft/textures/").empty()))) {
        std::lock_guard lock(gMutex);
        replaceSprites("tessera_java_", {});
        gRules.clear();
        gByTexture.clear();
        return {};
    }

    if (!gDumpDir.empty()) // diagnostics: Bedrock's own textures of interest
        for (char const* n : {"block/grass_side", "block/grass_carried", "block/grass_top"}) (void)assets.bedrockTexture(n);
    std::optional<Baked> baked = loadCache(assets);
    bool const           cached = baked.has_value();
    if (!baked) {
        Recorder          recorder(assets);
        java::BakeOptions options;
        options.pack          = &recorder;
        options.vanilla       = gVanilla.get();
        options.texturePrefix = "tessera_java_";
        if (gVanilla) recorder.note('v', gVanillaPath.string(), gVanillaStamp);
        std::set<std::string> missing;
        options.packBedrockTexture = [&](std::string const& name) { return recorder.userTexture(name); };
        options.fallbackTexture = [&](std::string const& name) {
            auto image = recorder.texture(name);
            if (!image) missing.insert(name);
            return image;
        };
        java::BakeResult result = java::bake(options);
        baked.emplace();
        baked->version = java::bakerVersion();
        baked->rules   = result.ruleCount ? result.rulesJson : std::string{};
        for (auto const& [name, image] : result.textures) baked->pngs.emplace_back(name, java::encodePng(image));
        for (auto const& [name, image] : result.pbrTextures) baked->pngs.emplace_back(name, java::encodePng(image));
        for (auto const& [name, list] : result.textureSources)
            for (auto const& s : list) baked->sources.emplace_back(name, s);
        baked->inputs = recorder.inputs();
        saveCache(*baked);
        if (gLog) {
            gLog->info("Java packs: baked {} rule(s), {} image(s) for {} block(s) in {} ms", result.ruleCount,
                       result.textures.size(), result.rulesPerBlock.size(), ms());
            for (size_t i = 0; i < result.problems.size() && i < 12; ++i) gLog->warn("Java packs: {}", result.problems[i]);
            if (result.problems.size() > 12) gLog->warn("Java packs: ... {} more problem(s)", result.problems.size() - 12);
            if (!missing.empty()) {
                std::string list;
                for (auto const& m : missing) list += (list.empty() ? "" : ", ") + m;
                gLog->warn("Java packs: textures found nowhere (left out): {}", list);
            }
        }
    } else if (gLog) {
        gLog->info("Java packs: reused the cached bake ({} image(s), {} inputs checked) in {} ms", baked->pngs.size(),
                   baked->inputs.size(), ms());
    }
    (void)cached;

    std::unordered_map<std::string, std::string>      png;
    std::vector<std::pair<std::string, std::string>> entries;
    std::set<std::string> withMaps; // images with shader maps: they get a texture set
    for (auto& [name, bytes] : baked->pngs) {
        if (name.ends_with("_n") || name.ends_with("_s")) { // shader maps (labPBR): served, not atlas tiles
            if (name.ends_with("_n")) withMaps.insert(name.substr(0, name.size() - 2));
            png.emplace(std::string(kSpriteDir) + name, std::move(bytes));
            continue;
        }
        std::string path = std::string(kSpriteDir) + name;
        png.emplace(path, std::move(bytes));
        entries.emplace_back(name, std::move(path));
    }
    std::unordered_map<std::string, std::vector<std::string>> byTexture;
    for (auto const& [name, source] : baked->sources) {
        std::string key = source;
        if (key.starts_with("block/")) key = key.substr(6);
        byTexture[key].push_back(name);
    }
    for (auto const& base : withMaps) {
        std::string const n = base.substr(base.rfind('/') + 1);
        png.emplace(std::string(kSpriteDir) + base + ".texture_set.json",
                    "{\"format_version\":\"1.16.100\",\"minecraft:texture_set\":{\"color\":\"" + n + "\",\"normal\":\"" + n
                        + "_normal\",\"metalness_emissive_roughness\":\"" + n + "_mers\"}}");
    }
    {
        std::lock_guard lock(gMutex);
        replaceSprites("tessera_java_", std::move(png));
        gRules     = std::move(baked->rules);
        gByTexture = std::move(byTexture);
    }
    return entries;
}

std::string pathOf(ResourceLocation const& location) { return location.mPath->value; }

std::string const* pngIn(gsl::span<std::string const> extensions) {
    for (auto const& e : extensions)
        if (e == ".png") return &e;
    return nullptr;
}

bool isOurs(std::string path) {
    if (!path.starts_with(kSpriteDir)) return false;
    if (path.ends_with(".png")) path.resize(path.size() - 4);
    std::lock_guard lock(gMutex);
    if (gPng.contains(path)) return true;
    for (std::string_view end : {std::string_view("_normal"), std::string_view("_mers")})
        if (path.ends_with(end)) return gPng.contains(path.substr(0, path.size() - end.size()) + (end == "_normal" ? "_n" : "_s"));
    return false;
}

void hookLocationLoad(ResourcePackManager& packs);

LL_TYPE_INSTANCE_HOOK(
    AtlasHook, ll::memory::HookPriority::Highest, TextureAtlas, &TextureAtlas::_createAtlas, TextureAtlasResourceCallbacks,
    Json::Value const& root, std::function<void(TextureAtlasStatus const&)> status, std::function<void()> const finished,
    std::shared_ptr<mce::TextureGroup> group, cg::MipMapSupport const mip, ResourcePackManager& packs,
    std::shared_ptr<cg::ImageBuffer> outFinal,
    std::shared_ptr<std::unordered_map<ResourceLocation, cg::TextureSetImageContainer>> outSources
) {
    bool terrain = false, items = false;
    try {
        auto const name = group ? root["texture_name"].asString("") : std::string{};
        terrain         = name == "atlas.terrain";
        items           = name == "atlas.items";
    } catch (...) {}
    if (items) { // custom item textures (CIT)
        Json::Value copy(root);
        try {
            auto                                         sprites = items::collect(packs);
            std::unordered_map<std::string, std::string> fresh;
            for (auto& s : sprites) {
                // Shader maps and texture sets of the icons are only served, not atlas entries.
                bool const served = s.name.ends_with("_n") || s.name.ends_with("_s") || s.path.ends_with(".texture_set.json");
                if (!served) copy["texture_data"][s.name]["textures"] = Json::Value(s.path);
                fresh[s.path] = std::move(s.png);
            }
            std::lock_guard lock(gMutex);
            replaceSprites("tessera_cit_", std::move(fresh));
        } catch (...) {
            if (gLog) gLog->error("CIT: preparing item icons failed");
            copy = root;
        }
        return origin(copy, std::move(status), finished, std::move(group), mip, packs, std::move(outFinal), std::move(outSources));
    }
    if (!terrain)
        return origin(root, std::move(status), finished, std::move(group), mip, packs, std::move(outFinal), std::move(outSources));
    try {
        hookLocationLoad(packs);
    } catch (...) {}
    Json::Value copy(root);
    try {
        Json::Value const grassSide = root["texture_data"]["grass_side"]["textures"];
        for (auto const& [name, path] : bakeStack(packs)) {
            // Grass block sides: Bedrock tints them in the atlas, one variant per biome colour.
            if (name.ends_with("_grass") && grassSide.isArray() && grassSide.size() > 1) {
                Json::Value list(Json::ValueType::ArrayValue);
                for (uint i = 0; i < grassSide.size(); ++i) {
                    Json::Value v(Json::ValueType::ObjectValue);
                    v["path"] = Json::Value(path);
                    if (grassSide[i].isObject() && grassSide[i].isMember("overlay_color"))
                        v["overlay_color"] = grassSide[i]["overlay_color"];
                    list.append(v);
                }
                copy["texture_data"][name]["textures"] = list;
                // Held: the item's tint (Java's plains grass colour).
                Json::Value held(Json::ValueType::ObjectValue);
                held["path"]          = Json::Value(path);
                held["overlay_color"] = Json::Value("#7cbd6b");
                copy["texture_data"][name + "_item"]["textures"] = held;
            } else copy["texture_data"][name]["textures"] = Json::Value(path);
        }
        // Connected-texture tiles go in as they are (no baking).
        auto sprites = ctm::collect(packs);
        std::unordered_map<std::string, std::string> fresh;
        for (auto& s : sprites) {
            bool const map = s.name.ends_with("_n") || s.name.ends_with("_s"); // shader maps: served only
            if (!map) copy["texture_data"][s.name]["textures"] = Json::Value(s.path);
            fresh[s.path] = std::move(s.png);
        }
        std::lock_guard lock(gMutex);
        replaceSprites("tessera_ctm_", std::move(fresh));
    } catch (std::exception const& e) {
        if (gLog) gLog->error("Java packs: baking failed: {}", e.what());
        copy = root;
    } catch (...) {
        if (gLog) gLog->error("Java packs: baking failed");
        copy = root;
    }
    return origin(copy, std::move(status), finished, std::move(group), mip, packs, std::move(outFinal), std::move(outSources));
}

// The atlas asks which pack holds each image before loading it; for ours, answer
// with the pack of a texture every stack has (the index only has to be valid).
using PackIndex = std::pair<int, std::string_view>;
LL_TYPE_INSTANCE_HOOK(
    PackIndexHook, ll::memory::HookPriority::Normal, ResourcePackLoader, &ResourcePackLoader::$getPackStackIndexOfResource,
    PackIndex, ResourceLocation const& location, gsl::span<std::string const> extensions
) {
    auto result = origin(location, extensions);
    if (result.first >= 0) return result;
    // Texture sets of Tessera's images (their maps are answered as pngs below).
    if (std::string const path = pathOf(location); path.starts_with(kSpriteDir))
        for (auto const& e : extensions) {
            bool have = false;
            {
                std::lock_guard lock(gMutex);
                have = gPng.contains(path + e) || (e == ".json" && gPng.contains(path + ".texture_set.json"));
            }
            if (!have) continue;
            static std::string const pngExt[] = {".png"};
            auto const known = origin(userPath("textures/blocks/stone"), gsl::span<std::string const>(pngExt));
            if (known.first >= 0) return {known.first, std::string_view(e)};
        }
    auto const* png = pngIn(extensions);
    if (!png || !isOurs(pathOf(location))) return result;
    auto const known = origin(userPath("textures/blocks/stone"), extensions);
    if (known.first >= 0) result = {known.first, std::string_view(*png)}; // the view points at the caller's entry
    return result;
}

// Fluids have no model: Java packs restyle them by replacing these four textures, which
// Bedrock keeps under other names. Both store the animation as a strip of square frames.
struct FluidTexture {
    std::string_view bedrock, java;
    bool             flow;
};
constexpr FluidTexture kFluids[] = {
    {"textures/blocks/water_still_grey", "block/water_still", false},
    {"textures/blocks/water_flow_grey", "block/water_flow", true},
    {"textures/blocks/lava_still", "block/lava_still", false},
    {"textures/blocks/lava_flow", "block/lava_flow", true},
};
std::unordered_map<std::string, std::pair<uint64_t, std::string>> gFluidPng; // Bedrock path -> source hash, PNG
std::atomic<bool> gFlowSidesMiddle[2]; // water, lava: the pack's flow .mcmeta asks for centred sides

// The Java texture of the pack stack as Bedrock wants it, or nullopt when no pack has it.
// Frames follow the .mcmeta frame list.
std::optional<std::string> fluidTexture(ResourceLoader const& loader, FluidTexture const& fluid) {
    std::string const base = "assets/minecraft/textures/" + std::string(fluid.java) + ".png";
    std::string       source, meta;
    try {
        if (fluid.flow) gFlowSidesMiddle[fluid.java == "block/lava_flow"].store(false, std::memory_order_release);
        if (!loader.load(userPath(base), source) || source.empty()) return std::nullopt;
        (void)loader.load(userPath(base + ".mcmeta"), meta);
    } catch (...) {
        return std::nullopt;
    }
    if (fluid.flow)
        if (auto j = nlohmann::json::parse(meta, nullptr, false, true); j.is_object())
            if (auto t = j.find("tessera"); t != j.end() && t->is_object() && t->value("flow_sides", std::string()) == "middle")
                gFlowSidesMiddle[fluid.java == "block/lava_flow"].store(true, std::memory_order_release);
    uint64_t const hash = fnv(meta.data(), meta.size(), fnv(source.data(), source.size()));
    {
        std::lock_guard lock(gMutex);
        if (auto it = gFluidPng.find(std::string(fluid.bedrock)); it != gFluidPng.end() && it->second.first == hash) return it->second.second;
    }
    auto image = java::decodePng(source);
    if (!image || image->width <= 0 || image->height % image->width != 0) return std::nullopt;
    int const frame = image->width, count = image->height / frame;
    std::vector<int> order;
    if (auto j = nlohmann::json::parse(meta, nullptr, false, true); j.is_object())
        if (auto a = j.find("animation"); a != j.end() && a->is_object())
            if (auto f = a->find("frames"); f != a->end() && f->is_array())
                for (auto const& e : *f) {
                    int const index = e.is_number_integer() ? e.get<int>() : e.is_object() ? e.value("index", -1) : -1;
                    if (index >= 0 && index < count) order.push_back(index);
                }
    if (order.empty())
        for (int i = 0; i < count; ++i) order.push_back(i);
    if (order.size() > 256) order.resize(256);
    // Flowing: Bedrock's texture is one tile repeated 2x2 (it keeps a tile of half the width).
    // A block face shows the middle of that repeat, where four tiles meet, and in Java the
    // middle of the frame: so the tile is the frame's middle with its halves swapped.
    bool const  tiled = fluid.flow && frame >= 4;
    int const   half  = frame / 2;
    java::Image out;
    out.width  = frame;
    out.height = frame * int(order.size());
    out.pixels.resize(size_t(out.width) * size_t(out.height));
    auto from = [&](int v) {
        if (!tiled) return v;
        int const t = v % half;
        return t < frame / 4 ? t + half : t;
    };
    for (size_t k = 0; k < order.size(); ++k)
        for (int y = 0; y < frame; ++y)
            for (int x = 0; x < frame; ++x)
                out.pixels[(k * size_t(frame) + size_t(y)) * size_t(frame) + size_t(x)] = image->at(from(x), order[k] * frame + from(y));
    int const size = frame;
    std::string png = java::encodePng(out);
    if (gLog) gLog->info("Java packs: {} drawn from the pack's {} ({} frame(s) of {} px)", fluid.bedrock, fluid.java, order.size(), size);
    std::lock_guard lock(gMutex);
    gFluidPng[std::string(fluid.bedrock)] = {hash, png};
    return png;
}

using LoadPair = bool (ResourcePackLoader::*)(ResourceLocationPair const&, std::string&, gsl::span<std::string const>) const;
// One of Tessera's served images by path without extension: as stored, or a Bedrock texture-set
// layer ("<x>_normal", "<x>_mers") converted from the labPBR maps it keeps ("<x>_n", "<x>_s").
// Caller holds gMutex.
std::optional<std::string> ownImage(std::string const& key) {
    if (auto it = gPng.find(key); it != gPng.end()) return it->second;
    bool const normal = key.ends_with("_normal"), mers = key.ends_with("_mers");
    if (!normal && !mers) return std::nullopt;
    std::string const base = key.substr(0, key.size() - (normal ? 7 : 5)) + (normal ? "_n" : "_s");
    auto it = gPng.find(base);
    if (it == gPng.end()) return std::nullopt;
    auto img = java::decodePng(it->second);
    if (!img) return std::nullopt;
    for (auto& p : img->pixels) p = normal ? java::labToBedrockNormal(p) : java::labToBedrockMers(p);
    std::string png = java::encodePng(*img);
    gPng.emplace(key, png);
    return png;
}

// Tessera's own files: baked images, their shader maps and texture sets, fluids. true when
// `stream` was filled. `loader` reads the pack stack (for the fluids' Java sources).
bool serveOwn(ResourceLoader const& loader, ResourceLocationPair const& pair, std::string& stream, gsl::span<std::string const> extensions) {

    {
        std::string const full = pathOf(*pair.mResourceLocation);
        if (full.starts_with(kSpriteDir)) { // texture sets of Tessera's images (and diagnostics)
            std::string want = full;
            if (!want.ends_with(".texture_set.json"))
                for (auto const& e : extensions)
                    if (e == ".texture_set.json" || e == ".json") want = full + e;
            if (want.ends_with(".texture_set.json")) {
                std::lock_guard lock(gMutex);
                if (auto it = gPng.find(want); it != gPng.end()) {
                    stream = it->second;
                    return true;
                }
            }
        }
    }
    if (pngIn(extensions)) {
        std::string path = pathOf(*pair.mResourceLocation);
        if (path.starts_with(kSpriteDir)) {
            if (path.ends_with(".png")) path.resize(path.size() - 4);
            std::lock_guard lock(gMutex);
            if (auto png = ownImage(path)) {
                stream = std::move(*png);
                return true;
            }
        } else if (path.starts_with("textures/blocks/")) {
            if (auto dot = path.rfind('.'); dot != std::string::npos && dot > path.rfind('/')) path.resize(dot);
            for (auto const& fluid : kFluids)
                if (path == fluid.bedrock) {
                    if (auto png = fluidTexture(loader, fluid)) {
                        stream = std::move(*png);
                        return true;
                    }
                    break;
                }
        }
    }
    return false;
}

LL_TYPE_INSTANCE_HOOK(
    LoadHook, ll::memory::HookPriority::Normal, ResourcePackLoader, static_cast<LoadPair>(&ResourcePackLoader::$load),
    bool, ResourceLocationPair const& pair, std::string& stream, gsl::span<std::string const> extensions
) {
    if (serveOwn(*static_cast<ResourceLoader const*>(this), pair, stream, extensions)) return true;
    return origin(pair, stream, extensions);
}

// load(ResourceLocation, string&, extensions): not exported, reached through the loader's
// vtable (slot 2). Shader mods (Arke) read texture sets and their maps this way.
using LocationLoad = bool (*)(ResourceLoader const*, ResourceLocation const&, std::string&, gsl::span<std::string const>);
LocationLoad gLocationLoadOrigin = nullptr;
void*        gLocationLoadTarget = nullptr;
bool locationLoad(ResourceLoader const* self, ResourceLocation const& location, std::string& stream, gsl::span<std::string const> extensions) {
    try {
        std::string const full = pathOf(location);
        if (full.starts_with(kSpriteDir)) {
            std::string key = full;
            for (std::string_view end : {std::string_view(".png"), std::string_view(".texture_set.json")})
                if (key.ends_with(end)) key.resize(key.size() - end.size());
            std::lock_guard lock(gMutex);
            bool wantSet = full.ends_with(".texture_set.json"), wantPng = full.ends_with(".png");
            for (auto const& e : extensions) wantSet |= e == ".texture_set.json", wantPng |= e == ".png";
            if (wantSet)
                if (auto it = gPng.find(key + ".texture_set.json"); it != gPng.end()) {
                    stream = it->second;
                    return true;
                }
            if (wantPng)
                if (auto png = ownImage(key)) {
                    stream = std::move(*png);
                    return true;
                }
        }
    } catch (...) {}
    return gLocationLoadOrigin(self, location, stream, extensions);
}
void hookLocationLoad(ResourcePackManager& packs) {
    if (gLocationLoadTarget) return;
    void* const* vt = *reinterpret_cast<void* const* const*>(static_cast<ResourceLoader const*>(&packs));
    void*        fn = vt[2];
    if (ll::memory::hook(fn, reinterpret_cast<void*>(&locationLoad), reinterpret_cast<void**>(&gLocationLoadOrigin),
                         ll::memory::HookPriority::Normal)
        == 0) {
        gLocationLoadTarget = fn;
        if (gLog) gLog->info("Java packs: hooked the resource loader's location load");
    }
}

// The stack's own override: the atlas reads texture-set layers through it.
using ManagerLoadPair = bool (ResourcePackManager::*)(ResourceLocationPair const&, std::string&, gsl::span<std::string const>) const;
LL_TYPE_INSTANCE_HOOK(
    ManagerLoadHook, ll::memory::HookPriority::Normal, ResourcePackManager, static_cast<ManagerLoadPair>(&ResourcePackManager::$load),
    bool, ResourceLocationPair const& pair, std::string& stream, gsl::span<std::string const> extensions
) {
    if (serveOwn(*static_cast<ResourceLoader const*>(this), pair, stream, extensions)) return true;
    return origin(pair, stream, extensions);
}

bool gInstalled = false;

} // namespace

std::vector<std::string> bakedTexturesUsing(std::string const& javaTexture) {
    std::string key = javaTexture;
    if (key.starts_with("minecraft:")) key = key.substr(10);
    if (key.starts_with("block/")) key = key.substr(6);
    std::lock_guard lock(gMutex);
    auto            it = gByTexture.find(key);
    return it == gByTexture.end() ? std::vector<std::string>{} : it->second;
}


std::optional<std::string> readPackFile(ResourcePackManager& packs, std::string const& path) {
    StackAssets assets(packs);
    return assets.read(path);
}

std::optional<java::Image> loadPackTexture(ResourcePackManager& packs, std::string const& path) {
    StackAssets       assets(packs);
    java::BakeOptions options;
    options.pack            = &assets;
    options.fallbackTexture = [&](std::string const& name) { return assets.bedrockTexture(name); };
    return java::loadTexture(options, path);
}

std::optional<java::Image> renderPackItemIcon(ResourcePackManager& packs, std::string const& model, bool flatOnly) {
    StackAssets       assets(packs);
    java::BakeOptions options;
    options.pack            = &assets;
    options.fallbackTexture = [&](std::string const& name) { return assets.bedrockTexture(name); };
    return java::renderItemIcon(options, model, flatOnly);
}

struct ItemSession::Impl {
    explicit Impl(ResourcePackManager& packs) : assets(packs) {
        options.pack            = &assets;
        options.vanilla         = gVanilla.get();
        options.fallbackTexture = [this](std::string const& name) { return assets.bedrockTexture(name); };
        icons.emplace(options);
    }
    StackAssets                    assets;
    java::BakeOptions              options;
    std::optional<java::ItemIcons> icons;
};
ItemSession::ItemSession(ResourcePackManager& packs) : mImpl(std::make_unique<Impl>(packs)) {}
ItemSession::~ItemSession() = default;
java::ItemIcons::Info ItemSession::info(std::string const& model) { return mImpl->icons->info(model); }
std::optional<java::Image> ItemSession::icon(std::string const& model, std::vector<uint32_t> const& tints, bool flatOnly) {
    return mImpl->icons->icon(model, tints, flatOnly);
}
std::optional<std::pair<java::Image, java::Image>> ItemSession::flatPbr(std::string const& model) { return mImpl->icons->flatPbr(model); }
std::optional<java::HandMesh> ItemSession::handMesh(std::string const& model, std::vector<uint32_t> const& tints) {
    return mImpl->icons->handMesh(model, tints);
}
std::optional<std::string> ItemSession::read(std::string const& path) { return mImpl->assets.read(path); }
std::optional<java::Image> ItemSession::texture(std::string const& path) { return java::loadTexture(mImpl->options, path); }
std::vector<std::pair<std::string, std::string>> ItemSession::vanillaDefinitions() {
    std::vector<std::pair<std::string, std::string>> out;
    if (!gVanilla) return out;
    constexpr std::string_view dir = "assets/minecraft/items/";
    for (auto const& path : gVanilla->list(std::string(dir))) {
        if (!path.ends_with(".json") || path.find('/', dir.size()) != std::string::npos) continue;
        if (auto text = gVanilla->read(path)) out.emplace_back(path, std::move(*text));
    }
    return out;
}

bool flowSidesMiddle(bool lava) { return gFlowSidesMiddle[lava].load(std::memory_order_acquire); }

std::string rulesJson() {
    std::lock_guard lock(gMutex);
    return gRules;
}

bool install(ll::io::Logger& logger, std::filesystem::path const& cacheDir) {
    if (gInstalled) return true;
    gLog         = &logger;
    gCacheFile   = cacheDir.empty() ? std::filesystem::path{} : cacheDir / "java_bake.cache";
    if (!cacheDir.empty() && std::filesystem::exists(cacheDir / "dump_fallback.txt")) {
        gDumpDir = cacheDir / "fallback";
        std::error_code ec;
        std::filesystem::create_directories(gDumpDir, ec);
    }
    // Vanilla Java assets: data/java_vanilla.txt names a jar ("off" for none); otherwise the
    // newest Java client under %APPDATA%\.minecraft\versions.
    try {
        std::filesystem::path jar;
        bool                  off = false;
        if (std::ifstream cfg(cacheDir / "java_vanilla.txt"); cfg) {
            std::string line;
            std::getline(cfg, line);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line == "off") off = true;
            else if (!line.empty()) jar = std::filesystem::path(std::u8string(line.begin(), line.end()));
        }
        if (!off && jar.empty())
            if (char const* appdata = std::getenv("APPDATA")) jar = java::findJavaClientJar(std::filesystem::path(appdata) / ".minecraft");
        if (!off && !jar.empty()) {
            auto zip = std::make_unique<java::ZipAssets>(jar);
            if (zip->ok()) {
                std::error_code ec;
                gVanillaStamp = std::filesystem::file_size(jar, ec) * 1000003ull
                              ^ uint64_t(std::filesystem::last_write_time(jar, ec).time_since_epoch().count());
                gVanillaPath = jar;
                logger.info("Java packs: vanilla assets from {} ({} files)", jar.string(), zip->size());
                gVanilla = std::move(zip);
            } else logger.warn("Java packs: {} could not be read as a Java client jar", jar.string());
        } else if (!off) logger.info("Java packs: no Java client jar found; packs that only change vanilla models are left out");
    } catch (...) {}
    int failures = 0;
    failures += AtlasHook::hook() != 0;
    failures += PackIndexHook::hook() != 0;
    failures += LoadHook::hook() != 0;
    failures += ManagerLoadHook::hook() != 0;
    if (failures) logger.error("Java packs: {} hook(s) failed to install", failures);
    gInstalled = true;
    return failures == 0;
}

void uninstall() {
    if (!gInstalled) return;
    if (gLocationLoadTarget) ll::memory::unhook(gLocationLoadTarget, reinterpret_cast<void*>(&locationLoad)), gLocationLoadTarget = nullptr;
    ManagerLoadHook::unhook();
    LoadHook::unhook();
    PackIndexHook::unhook();
    AtlasHook::unhook();
    gInstalled = false;
}

} // namespace tessera::game::javapacks
