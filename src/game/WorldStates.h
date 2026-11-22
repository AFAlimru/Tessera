// Tessera - world-derived states ("tessera:note", "tessera:has_record", other mods' "mymod:foo").
//
// Values that do not live in a block's own states: block-entity data and values
// other mods push through the public API. They are written on the client thread
// and read by chunk-build threads from a snapshot, so no build thread ever
// touches live block entities (they crash when read off-thread).
#pragma once

#include "core/Rules.h"

#include <cstdint>
#include <string_view>

class BlockSource;
namespace ll::io {
class Logger;
}
class RenderChunkCoordinator;

namespace tessera::game::world {

// Blocks whose block entity Tessera reads itself (note block, jukebox, comparators).
bool hasBuiltinEntityStates(std::string_view blockName);

// Chunk-build threads: merge every known world state at `pos` into `out` and
// return a hash of them (part of the render cache key). For blocks with
// built-in entity states, an unknown position is queued for the client thread
// and defaults are used until it is read; the chunk is rebuilt then.
uint64_t collect(int dimension, int x, int y, int z, std::string_view blockName, StateMap& out);

// Client thread, once per RenderChunkCoordinator::tick: read queued and tracked
// block entities, apply pushed values, and rebuild chunks whose values changed.
void tick(RenderChunkCoordinator& coordinator);

// Public API backends (any thread). Keys must contain ':'; "tessera:" and
// "minecraft:" are reserved.
bool push(int dimension, int x, int y, int z, std::string_view key, StateValue value);
bool clear(int dimension, int x, int y, int z, std::string_view key);
// Any thread: rebuild the chunk holding this position at the next client tick.
void requestRebuild(int dimension, int x, int y, int z);

// A note block played (tuned, powered, clicked): the block event carries its
// instrument and pitch. Any thread.
void noteEvent(int dimension, int x, int y, int z, int instrument, int note);

void reset();
void setLogger(ll::io::Logger* logger);

} // namespace tessera::game::world
