#include "game/WorldStates.h"

#include "ll/api/io/Logger.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/renderer/chunks/RenderChunkCoordinator.h"
#include "mc/deps/shared_types/v1_26_20/block/NoteBlockInstrument.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/actor/BlockActor.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/actor/BlockActorType.h"
#include "mc/world/level/block/actor/ComparatorBlockActor.h"
#include "mc/world/level/block/actor/JukeboxBlockActor.h"
#include "mc/world/level/block/actor/NoteBlockActor.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tessera::game::world {
namespace {

ll::io::Logger*  gLog = nullptr;
std::atomic<int> gReadLogBudget{24};

struct PosKey {
    int  dim, x, y, z;
    bool operator==(PosKey const&) const = default;
};
struct PosKeyHash {
    size_t operator()(PosKey const& k) const noexcept {
        uint64_t h = uint64_t(uint32_t(k.x)) * 0x9E3779B97F4A7C15ull;
        h ^= uint64_t(uint32_t(k.z)) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
        h ^= uint64_t(uint32_t(k.y) | (uint64_t(uint32_t(k.dim)) << 32)) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
        return size_t(h);
    }
};

struct Slot {
    StateMap builtin; // what rules see: block entity reads, corrected by block events
    StateMap polled;  // the block entity as last read (it can lag behind the server)
    StateMap pushed;  // set by other mods through the API
};

std::shared_mutex                              gSnapMutex;
std::unordered_map<PosKey, Slot, PosKeyHash>   gSnap;
std::mutex                                     gQueueMutex;
std::unordered_set<PosKey, PosKeyHash>         gWanted; // seen by a build thread, not read yet
std::vector<PosKey>                            gDirty;  // pushed values changed
// Client thread only.
std::vector<PosKey>                            gTracked;
std::unordered_set<PosKey, PosKeyHash>         gTrackedSet;
size_t                                         gPollCursor = 0;

constexpr std::string_view kNote      = "minecraft:noteblock";
constexpr std::string_view kJukebox   = "minecraft:jukebox";
constexpr std::string_view kCompOn    = "minecraft:powered_comparator";
constexpr std::string_view kCompOff   = "minecraft:unpowered_comparator";
constexpr size_t           kPollPerTick = 256;

// Java's names, so packs can be written the Java way; Bedrock-only ones keep Bedrock's.
constexpr char const* kInstruments[] = {
    "harp", "basedrum", "snare", "hat", "bass", "flute", "bell", "guitar", "chime", "xylophone",
    "iron_xylophone", "cow_bell", "didgeridoo", "bit", "banjo", "pling", "trumpet", "trumpet_exposed",
    "trumpet_weathered", "trumpet_oxidized", "zombie", "skeleton", "creeper", "dragon", "wither_skeleton",
    "piglin", "none"};

StateValue intValue(int64_t v) {
    StateValue s;
    s.kind = StateValue::Kind::Int;
    s.i    = v;
    return s;
}
StateValue boolValue(bool v) {
    StateValue s;
    s.kind = StateValue::Kind::Bool;
    s.i    = v;
    return s;
}
StateValue stringValue(std::string v) {
    StateValue s;
    s.kind = StateValue::Kind::String;
    s.s    = std::move(v);
    return s;
}

bool sameValue(StateValue const& a, StateValue const& b) {
    return a.kind == b.kind && a.i == b.i && a.s == b.s;
}
bool sameMap(StateMap const& a, StateMap const& b) {
    if (a.size() != b.size()) return false;
    for (auto ia = a.begin(), ib = b.begin(); ia != a.end(); ++ia, ++ib)
        if (ia->first != ib->first || !sameValue(ia->second, ib->second)) return false;
    return true;
}

bool isLeaves(std::string_view block) { return block.find("leaves") != std::string_view::npos; }

// Java's "logs" tag: what keeps leaves alive.
bool isLog(std::string_view block) {
    return block.ends_with("_log") || block.ends_with("_wood") || block.ends_with("_stem") || block.ends_with("_hyphae");
}

StateMap defaults(std::string_view block) {
    StateMap m;
    if (isLeaves(block)) m.emplace("tessera:distance", intValue(1));
    if (block == kNote) {
        m.emplace("tessera:note", intValue(0));
        m.emplace("tessera:instrument", stringValue("harp"));
    } else if (block == kJukebox) {
        m.emplace("tessera:has_record", boolValue(false));
    } else if (block == kCompOn || block == kCompOff) {
        m.emplace("tessera:output", intValue(0));
    }
    return m;
}

void hashInto(uint64_t& h, StateMap const& m) {
    for (auto const& [k, v] : m) {
        auto text = k + '=' + v.toString() + ';';
        for (unsigned char c : text) h = (h ^ c) * 0x100000001B3ull; // FNV-1a
    }
}

// Client thread: the values a block entity holds now, or none when it is gone.
// Leaves: steps to the nearest log through leaves, as Java's distance, but by Bedrock's
    // rule: a log within 4 steps keeps leaves alive (Java: 6). 7 = too far, the leaves decay.
constexpr int kLeafReach = 4;
int leavesDistance(BlockSource& region, BlockPos const& start) {
    std::vector<std::pair<BlockPos, int>> queue{{start, 0}};
    std::unordered_set<int64_t>          seen;
    auto key = [&](BlockPos const& p) { return (int64_t(p.x - start.x + 8) << 10) | (int64_t(p.y - start.y + 8) << 5) | (p.z - start.z + 8); };
    seen.insert(key(start));
    static constexpr int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (size_t i = 0; i < queue.size(); ++i) {
        auto const [p, steps] = queue[i];
        if (steps >= kLeafReach) continue;
        for (auto const& o : d) {
            BlockPos const n{p.x + o[0], p.y + o[1], p.z + o[2]};
            if (!seen.insert(key(n)).second) continue;
            std::string const& name = region.getBlock(n).getTypeName();
            if (isLog(name)) return steps + 1;
            if (isLeaves(name)) queue.push_back({n, steps + 1});
        }
    }
    return 7;
}

std::optional<StateMap> readEntity(BlockSource& region, BlockPos const& pos) {
    BlockActor* actor = region.getBlockEntity(pos);
    if (!actor) {
        if (!isLeaves(region.getBlock(pos).getTypeName())) return std::nullopt;
        StateMap m;
        m.emplace("tessera:distance", intValue(leavesDistance(region, pos)));
        return m;
    }
    StateMap m;
    switch (actor->getType()) {
    case BlockActorType::Music: {
        auto& note = static_cast<NoteBlockActor&>(*actor);
        m.emplace("tessera:note", intValue(int(note.mNote)));
        auto instrument = NoteBlockActor::getInstrument(region, pos);
        auto index      = instrument ? size_t(*instrument) : std::size(kInstruments) - 1;
        m.emplace("tessera:instrument", stringValue(index < std::size(kInstruments) ? kInstruments[index] : "none"));
        break;
    }
    case BlockActorType::Jukebox:
        m.emplace("tessera:has_record", boolValue(!static_cast<JukeboxBlockActor&>(*actor).mRecord.get().isNull()));
        break;
    case BlockActorType::Comparator:
        m.emplace("tessera:output", intValue(static_cast<ComparatorBlockActor&>(*actor).mOutput));
        break;
    default: return std::nullopt;
    }
    return m;
}

// Rebuild the block's chunk right away, like a block the player just changed.
// (onAreaChanged queues a low-priority rebuild that shows up 1-2 s later.)
void markDirty(RenderChunkCoordinator& coordinator, PosKey const& k) {
    BlockPos pos{k.x, k.y, k.z};
    coordinator._setDirty(pos, pos, /*immediate*/ true, /*changesVisibility*/ false, /*canInterlock*/ false);
}

} // namespace

void setLogger(ll::io::Logger* logger) { gLog = logger; }

bool hasBuiltinEntityStates(std::string_view block) {
    return block == kNote || block == kJukebox || block == kCompOn || block == kCompOff || isLeaves(block);
}

uint64_t collect(int dimension, int x, int y, int z, std::string_view blockName, StateMap& out) {
    PosKey   key{dimension, x, y, z};
    uint64_t h       = 0xCBF29CE484222325ull;
    bool     builtin = hasBuiltinEntityStates(blockName);
    {
        std::shared_lock lock(gSnapMutex);
        if (auto it = gSnap.find(key); it != gSnap.end()) {
            if (builtin && it->second.builtin.empty()) {
                auto d = defaults(blockName);
                hashInto(h, d);
                for (auto& [k, v] : d) out.insert_or_assign(k, v);
            }
            hashInto(h, it->second.builtin);
            hashInto(h, it->second.pushed);
            for (auto const& [k, v] : it->second.builtin) out.insert_or_assign(k, v);
            for (auto const& [k, v] : it->second.pushed) out.insert_or_assign(k, v);
            if (!builtin || !it->second.builtin.empty()) return h;
        } else if (builtin) {
            auto d = defaults(blockName);
            hashInto(h, d);
            for (auto& [k, v] : d) out.insert_or_assign(k, v);
        }
    }
    if (builtin) {
        std::lock_guard lock(gQueueMutex);
        gWanted.insert(key);
    }
    return h;
}

void tick(RenderChunkCoordinator& coordinator) {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    BlockSource* region = client->getRegion();
    if (!region) return;
    // A different world: values (polled or pushed by other mods) belong to the old one.
    static void const* lastLevel = nullptr;
    if (void const* level = client->getLevel(); level != lastLevel) {
        if (lastLevel) reset();
        lastLevel = level;
    }
    int const dim = int(region->getDimensionId());
    if (dim != int(coordinator.mDimensionId.get())) return;

    std::vector<PosKey> dirty;
    {
        std::lock_guard lock(gQueueMutex);
        for (auto it = gWanted.begin(); it != gWanted.end();) {
            if (it->dim == dim) {
                if (gTrackedSet.insert(*it).second) gTracked.push_back(*it);
                it = gWanted.erase(it);
            } else ++it;
        }
        for (auto it = gDirty.begin(); it != gDirty.end();) {
            if (it->dim == dim) {
                dirty.push_back(*it);
                it = gDirty.erase(it);
            } else ++it;
        }
    }

    // Poll tracked block entities round-robin; values change without a block change.
    size_t const n = std::min(gTracked.size(), kPollPerTick);
    for (size_t i = 0; i < n && !gTracked.empty(); ++i) {
        if (gPollCursor >= gTracked.size()) gPollCursor = 0;
        PosKey const k = gTracked[gPollCursor];
        if (k.dim != dim) {
            ++gPollCursor;
            continue;
        }
        auto values = readEntity(*region, BlockPos{k.x, k.y, k.z});
        if (!values) { // block entity gone (block broken or chunk unloaded): forget it
            gTrackedSet.erase(k);
            gTracked[gPollCursor] = gTracked.back();
            gTracked.pop_back();
            std::unique_lock lock(gSnapMutex);
            if (auto it = gSnap.find(k); it != gSnap.end()) {
                it->second.builtin.clear();
                if (it->second.pushed.empty()) gSnap.erase(it);
            }
            continue;
        }
        bool changed = false;
        {
            std::unique_lock lock(gSnapMutex);
            auto& slot = gSnap[k];
            // The client's block entity is not resent when a note block is tuned, so a
            // read only counts when the entity itself changed (load, chunk resend);
            // block events keep `builtin` current in between.
            if (!slot.polled.empty() && sameMap(slot.polled, *values)) {
                ++gPollCursor;
                continue;
            }
            slot.polled = *values;
            if (slot.builtin.empty()) {
                // First read: build threads used defaults; rebuild only if those were wrong.
                StateMap d;
                for (auto const& [key, v] : *values)
                    if (key == "tessera:note" || key == "tessera:instrument") d = defaults(kNote);
                    else if (key == "tessera:has_record") d = defaults(kJukebox);
                    else if (key == "tessera:output") d = defaults(kCompOn);
                changed = !sameMap(d, *values);
                slot.builtin = std::move(*values);
            } else if (!sameMap(slot.builtin, *values)) {
                slot.builtin = std::move(*values);
                changed      = true;
            }
            if (changed && gLog && gReadLogBudget.fetch_sub(1) > 0) {
                std::string text;
                for (auto const& [key, v] : slot.builtin) text += key + '=' + v.toString() + ' ';
                gLog->info("block entity @({},{},{}) dim {}: {}", k.x, k.y, k.z, k.dim, text);
            }
        }
        ++gPollCursor;
        if (changed) dirty.push_back(k);
    }
    for (auto const& k : dirty) markDirty(coordinator, k);
    { // Diagnostics: block-level rebuilds per 5 s (should be rare when nothing changes).
        static size_t marked = 0;
        static auto   last   = std::chrono::steady_clock::now();
        marked += dirty.size();
        if (auto now = std::chrono::steady_clock::now(); now - last > std::chrono::seconds(5)) {
            if (gLog && marked) gLog->info("world states: {} position rebuild(s) in the last 5 s, {} tracked", marked, gTracked.size());
            marked = 0;
            last   = now;
        }
    }
}

void noteEvent(int dimension, int x, int y, int z, int instrument, int note) {
    PosKey k{dimension, x, y, z};
    StateMap values;
    values.emplace("tessera:note", intValue(note));
    values.emplace("tessera:instrument",
                   stringValue(instrument >= 0 && size_t(instrument) < std::size(kInstruments) ? kInstruments[instrument] : "none"));
    {
        std::unique_lock lock(gSnapMutex);
        auto& slot = gSnap[k];
        if (sameMap(slot.builtin, values)) return;
        slot.builtin = std::move(values);
        if (gLog && gReadLogBudget.fetch_sub(1) > 0)
            gLog->info("note block event @({},{},{}) dim {}: note={} instrument={}", x, y, z, dimension, note, instrument);
    }
    std::lock_guard lock(gQueueMutex);
    gDirty.push_back(k);
}

bool push(int dimension, int x, int y, int z, std::string_view key, StateValue value) {
    if (key.find(':') == std::string_view::npos || key.starts_with("tessera:") || key.starts_with("minecraft:")) return false;
    PosKey k{dimension, x, y, z};
    {
        std::unique_lock lock(gSnapMutex);
        auto& pushed = gSnap[k].pushed;
        auto  it     = pushed.find(key);
        if (it != pushed.end() && sameValue(it->second, value)) return true;
        pushed.insert_or_assign(std::string(key), std::move(value));
    }
    std::lock_guard lock(gQueueMutex);
    gDirty.push_back(k);
    return true;
}

void requestRebuild(int dimension, int x, int y, int z) {
    std::lock_guard lock(gQueueMutex);
    gDirty.push_back(PosKey{dimension, x, y, z});
}

bool clear(int dimension, int x, int y, int z, std::string_view key) {
    PosKey k{dimension, x, y, z};
    {
        std::unique_lock lock(gSnapMutex);
        auto it = gSnap.find(k);
        if (it == gSnap.end()) return false;
        auto p = it->second.pushed.find(key);
        if (p == it->second.pushed.end()) return false;
        it->second.pushed.erase(p);
        if (it->second.pushed.empty() && it->second.builtin.empty()) gSnap.erase(it);
    }
    std::lock_guard lock(gQueueMutex);
    gDirty.push_back(k);
    return true;
}

void reset() {
    std::unique_lock lock(gSnapMutex);
    std::lock_guard  qlock(gQueueMutex);
    gSnap.clear();
    gWanted.clear();
    gDirty.clear();
    gTracked.clear();
    gTrackedSet.clear();
    gPollCursor = 0;
}

} // namespace tessera::game::world
