// Tessera - Java-layout resource packs read in game (no conversion step).
#pragma once

#include <filesystem>
#include "core/java/JavaPack.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class ResourcePackManager;

namespace ll::io {
class Logger;
}

namespace tessera::game::javapacks {

// When the terrain atlas is built, the active packs are searched for Java assets
// (assets/minecraft/blockstates, models, textures). Their blocks are baked into
// images that are added to the atlas, and into rules (see rulesJson).
// cacheDir: where the last bake is kept (empty: no cache).
bool install(ll::io::Logger& logger, std::filesystem::path const& cacheDir);
void uninstall();

// The rules made from Java assets at the last terrain atlas build, in the
// state_textures.json format; empty when no active pack has Java assets.
std::string rulesJson();

// Baked images (atlas texture names) that a Java texture ("glass", "block/glass") was
// drawn into, for connected-texture rules naming textures a pack's model uses.
std::vector<std::string> bakedTexturesUsing(std::string const& javaTexture);

// Reads through the pack stack (highest pack first), for other Java-format features.
std::optional<std::string> readPackFile(ResourcePackManager& packs, std::string const& path);
// A texture by pack path or Java name, falling back to the Bedrock texture of that name.
std::optional<java::Image> loadPackTexture(ResourcePackManager& packs, std::string const& path);
// An item model drawn as an inventory icon (see java::renderItemIcon).
std::optional<java::Image> renderPackItemIcon(ResourcePackManager& packs, std::string const& model, bool flatOnly = false);


// Item icons from the pack stack (and the player's Java client jar, when there is one),
// keeping what it loaded for as long as it lives. Use one while collecting icons.
class ItemSession {
public:
    explicit ItemSession(ResourcePackManager& packs);
    ~ItemSession();
    java::ItemIcons::Info      info(std::string const& model);
    std::optional<java::Image> icon(std::string const& model, std::vector<uint32_t> const& tints, bool flatOnly);
    std::optional<std::pair<java::Image, java::Image>> flatPbr(std::string const& model);
    std::optional<java::HandMesh> handMesh(std::string const& model, std::vector<uint32_t> const& tints);
    std::optional<std::string> read(std::string const& path);
    std::optional<java::Image> texture(std::string const& path);
    // Item definitions (assets/minecraft/items/x.json) of the Java client jar: path and text.
    std::vector<std::pair<std::string, std::string>> vanillaDefinitions();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace tessera::game::javapacks
