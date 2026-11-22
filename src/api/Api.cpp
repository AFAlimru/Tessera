// C exports behind include/Tessera/Api.h.
#define TESSERA_BUILDING
#include "Tessera/Api.h"

#include "core/java/JavaPack.h"
#include "game/Items.h"
#include "game/Providers.h"
#include "game/WorldStates.h"

#include <string_view>

#define TESSERA_EXPORT extern "C" __declspec(dllexport)

namespace {
tessera::StateValue makeInt(std::int64_t v) {
    tessera::StateValue s;
    s.kind = tessera::StateValue::Kind::Int;
    s.i    = v;
    return s;
}
} // namespace

TESSERA_EXPORT std::uint32_t tessera_api_version() { return TESSERA_API_VERSION; }

TESSERA_EXPORT bool tessera_set_state_int(int dim, int x, int y, int z, char const* key, std::int64_t value) {
    return key && tessera::game::world::push(dim, x, y, z, key, makeInt(value));
}

TESSERA_EXPORT bool tessera_set_state_bool(int dim, int x, int y, int z, char const* key, bool value) {
    if (!key) return false;
    tessera::StateValue s;
    s.kind = tessera::StateValue::Kind::Bool;
    s.i    = value;
    return tessera::game::world::push(dim, x, y, z, key, std::move(s));
}

TESSERA_EXPORT bool tessera_set_state_string(int dim, int x, int y, int z, char const* key, char const* value) {
    if (!key || !value) return false;
    tessera::StateValue s;
    s.kind = tessera::StateValue::Kind::String;
    s.s    = value;
    return tessera::game::world::push(dim, x, y, z, key, std::move(s));
}

TESSERA_EXPORT bool tessera_clear_state(int dim, int x, int y, int z, char const* key) {
    return key && tessera::game::world::clear(dim, x, y, z, key);
}

TESSERA_EXPORT bool tessera_register_provider(char const* key, tessera_provider_fn fn, void* user) {
    return key && fn && tessera::game::providers::add(key, reinterpret_cast<tessera::game::providers::Fn>(fn), user);
}

TESSERA_EXPORT bool tessera_unregister_provider(char const* key) {
    return key && tessera::game::providers::remove(key);
}

TESSERA_EXPORT int tessera_item_model(void const* stack, tessera_quad* out, int capacity) {
    if (!stack) return 0;
    auto const mesh = tessera::game::items::itemModel(*static_cast<ItemStack const*>(stack));
    if (!mesh) return 0;
    int const n = int(mesh->model.size());
    for (int i = 0; out && i < n && i < capacity; ++i) {
        auto const& q = mesh->model[size_t(i)];
        for (int k = 0; k < 12; ++k) out[i].pos[k] = q.pos[size_t(k)];
        for (int k = 0; k < 3; ++k) out[i].normal[k] = q.normal[size_t(k)];
        out[i].color = q.color;
    }
    return n;
}

TESSERA_EXPORT bool tessera_mark_dirty(int dim, int x, int y, int z) {
    tessera::game::world::requestRebuild(dim, x, y, z);
    return true;
}
