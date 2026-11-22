#define TESSERA_BUILDING
#include "Tessera/Api.h"

#include "game/Engine.h"
#include "game/Providers.h"

#include <map>
#include <shared_mutex>

namespace tessera::game::providers {
namespace {

struct Entry {
    Fn    fn;
    void* user;
};
std::shared_mutex            gMutex;
std::map<std::string, Entry> gProviders;

} // namespace

bool add(std::string_view key, Fn fn, void* user) {
    if (!fn || key.find(':') == std::string_view::npos || key.starts_with("tessera:") || key.starts_with("minecraft:"))
        return false;
    {
        std::unique_lock lock(gMutex);
        gProviders.insert_or_assign(std::string(key), Entry{fn, user});
    }
    invalidateCaches();
    return true;
}

bool remove(std::string_view key) {
    {
        std::unique_lock lock(gMutex);
        if (!gProviders.erase(std::string(key))) return false;
    }
    invalidateCaches();
    return true;
}

std::vector<std::string> keys() {
    std::shared_lock         lock(gMutex);
    std::vector<std::string> out;
    for (auto const& [k, _] : gProviders) out.push_back(k);
    return out;
}

std::optional<StateValue> call(std::string const& key, int dim, int x, int y, int z, std::string const& block) {
    Entry e{};
    {
        std::shared_lock lock(gMutex);
        auto             it = gProviders.find(key);
        if (it == gProviders.end()) return std::nullopt;
        e = it->second;
    }
    tessera_value v{};
    bool          ok = false;
    try {
        ok = e.fn(dim, x, y, z, block.c_str(), e.user, &v);
    } catch (...) {
        return std::nullopt;
    }
    if (!ok) return std::nullopt;
    StateValue s;
    switch (v.kind) {
    case TESSERA_VALUE_INT: s.kind = StateValue::Kind::Int, s.i = v.i; break;
    case TESSERA_VALUE_BOOL: s.kind = StateValue::Kind::Bool, s.i = v.i != 0; break;
    case TESSERA_VALUE_STRING:
        s.kind = StateValue::Kind::String;
        s.s.assign(v.s, strnlen(v.s, sizeof(v.s)));
        break;
    default: return std::nullopt;
    }
    return s;
}

} // namespace tessera::game::providers
