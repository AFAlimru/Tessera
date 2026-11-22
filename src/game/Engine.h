// Tessera - Windows/LeviLamina game layer.
#pragma once

#include <filesystem>

#include <string>

namespace ll::io {
class Logger;
}

namespace tessera::game {

// Install / remove all game hooks. Safe to call once each.
// dataDir: the mod's data folder (caches).
bool install(ll::io::Logger& logger, std::filesystem::path const& dataDir);
void uninstall();

// Reload resource packs without leaving the world (what F3+R does). Main thread.
void requestHotReload();
// Does some rule give this block ("minecraft:x") its own shape when drawn as an item?
bool blockHasItemShape(std::string const& blockName);
// Some rule names this block (a pack changes how it looks).
bool blockHasRules(std::string const& blockName);

// Detailed diagnostic logs (rule hits, 3D shapes, textures): only with data/debug.txt next to
// the mod, so a player's log is not flooded.
bool debugLogging();

// Forget cached per-block rule results (e.g. a state provider was added).
void invalidateCaches();

// Rebuild every chunk mesh at the next coordinator tick (any thread).
void requestChunkRebuild();

// A line in the chat area. Main thread.
void chat(std::string const& text);

} // namespace tessera::game
