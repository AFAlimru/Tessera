// Tessera - custom item textures (OptiFine CIT format) in game.
#pragma once

#include "game/Ctm.h" // Sprite

#include <filesystem>
#include <memory>

class ItemStack;
namespace tessera::java {
struct HandMesh;
}

namespace ll::io {
class Logger;
}

namespace tessera::game::items {

bool install(ll::io::Logger& logger, std::filesystem::path const& dataDir);
void uninstall();

// Item atlas build: reads assets/minecraft/optifine/cit/**.properties from the pack stack,
// draws each rule's icon and returns the images to add to the item atlas.
std::vector<ctm::Sprite> collect(ResourcePackManager& packs);

// The 3D model Tessera draws for a stack (held and dropped), or null when the game's own
// look is used (no pack model with elements). Reads the snapshot only: any thread.
std::shared_ptr<java::HandMesh const> itemModel(ItemStack const& stack);

} // namespace tessera::game::items
