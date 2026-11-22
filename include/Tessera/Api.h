// Tessera public API — let other mods feed states to resource packs.
//
// A mod sets a namespaced value at a block position ("mymod:charge" = 3); any
// resource pack rule can then use it like a block state:
//
//   { "block": "mymod:battery", "when": { "mymod:charge": "1..4" },
//     "replace": { "battery_side": "battery_side_{mymod:charge}" } }
//
// Calls are thread-safe and cheap: values go into Tessera's snapshot and the
// affected chunk is rebuilt on the next client tick. Keys must be namespaced;
// "tessera:" and "minecraft:" are reserved. Dimension ids: 0 overworld, 1 nether, 2 end.
//
// Two ways to give a value:
//  - push: set/clear a value at a position (any thread); the chunk is rebuilt.
//  - provide (API 2): register a function Tessera calls while drawing a block
//    whose rules read your key. It runs on chunk-build threads: it must be
//    thread-safe, fast, and must not touch live game objects. Call markDirty
//    when the answer for a position changes.
//
// Item models (API 3): the 3D model a resource pack gives an item (Java item model with
// elements), as Tessera draws it when held and dropped — for mods that draw items their own
// way (dropped-item physics, displays). Quads cut per texel like Bedrock's own held items,
// in the model's space: block units (0..1 = one block) on Java's axes (x east, y up,
// z south); corners go round so cross(p1 - p0, p3 - p0) faces out; color is RGBA with R
// in the low byte. No model (flat items, the game's own look): 0.
//
// No link dependency: the wrapper finds Tessera.dll at runtime and every call
// returns false when Tessera is not loaded.
#pragma once

#include <cstdint>

#define TESSERA_API_VERSION 3u

extern "C" {
using tessera_api_version_fn  = std::uint32_t (*)();
using tessera_set_int_fn      = bool (*)(int dim, int x, int y, int z, char const* key, std::int64_t value);
using tessera_set_bool_fn     = bool (*)(int dim, int x, int y, int z, char const* key, bool value);
using tessera_set_string_fn   = bool (*)(int dim, int x, int y, int z, char const* key, char const* value);
using tessera_clear_state_fn  = bool (*)(int dim, int x, int y, int z, char const* key);

// API 2: providers.
enum { TESSERA_VALUE_NONE = 0, TESSERA_VALUE_INT = 1, TESSERA_VALUE_BOOL = 2, TESSERA_VALUE_STRING = 3 };
struct tessera_value {
    int          kind;  // TESSERA_VALUE_*
    std::int64_t i;     // int, or bool (0/1)
    char         s[64]; // string, zero-terminated
};
// Return true and fill *out to give a value; false = no value here.
using tessera_provider_fn = bool (*)(int dim, int x, int y, int z, char const* block, void* user, tessera_value* out);
using tessera_register_provider_fn   = bool (*)(char const* key, tessera_provider_fn fn, void* user);
using tessera_unregister_provider_fn = bool (*)(char const* key);
using tessera_mark_dirty_fn          = bool (*)(int dim, int x, int y, int z);

// API 3: item models.
struct tessera_quad {
    float         pos[12]; // 4 corners, xyz each
    float         normal[3];
    std::uint32_t color;
};
// stack: the game's ItemStack (const ItemStack*). Writes up to capacity quads into out and
// returns how many the model has (call again with a bigger buffer when that is more); 0 = none.
using tessera_item_model_fn = int (*)(void const* stack, tessera_quad* out, int capacity);
}

#ifndef TESSERA_BUILDING
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace tessera::api {

class Client {
public:
    // True when Tessera is loaded and speaks a compatible API version.
    bool available() { return resolve(); }

    bool setInt(int dim, int x, int y, int z, char const* key, std::int64_t value) {
        return resolve() && mSetInt(dim, x, y, z, key, value);
    }
    bool setBool(int dim, int x, int y, int z, char const* key, bool value) {
        return resolve() && mSetBool(dim, x, y, z, key, value);
    }
    bool setString(int dim, int x, int y, int z, char const* key, char const* value) {
        return resolve() && mSetString(dim, x, y, z, key, value);
    }
    bool clear(int dim, int x, int y, int z, char const* key) { return resolve() && mClear(dim, x, y, z, key); }

    // API 2 (false when the loaded Tessera is older).
    bool registerProvider(char const* key, tessera_provider_fn fn, void* user = nullptr) {
        return resolve() && mRegister && mRegister(key, fn, user);
    }
    bool unregisterProvider(char const* key) { return resolve() && mUnregister && mUnregister(key); }
    bool markDirty(int dim, int x, int y, int z) { return resolve() && mMarkDirty && mMarkDirty(dim, x, y, z); }

    // API 3: the pack's 3D model for an ItemStack (see above); 0 when none or Tessera is older.
    int itemModel(void const* stack, tessera_quad* out, int capacity) {
        return resolve() && mItemModel ? mItemModel(stack, out, capacity) : 0;
    }

private:
    bool resolve() {
        if (mSetInt) return true;
#ifdef _WIN32
        HMODULE dll = GetModuleHandleW(L"Tessera.dll");
        if (!dll) return false;
        auto version = reinterpret_cast<tessera_api_version_fn>(GetProcAddress(dll, "tessera_api_version"));
        if (!version || version() < 1u) return false;
        mRegister   = reinterpret_cast<tessera_register_provider_fn>(GetProcAddress(dll, "tessera_register_provider"));
        mUnregister = reinterpret_cast<tessera_unregister_provider_fn>(GetProcAddress(dll, "tessera_unregister_provider"));
        mMarkDirty  = reinterpret_cast<tessera_mark_dirty_fn>(GetProcAddress(dll, "tessera_mark_dirty"));
        mItemModel  = reinterpret_cast<tessera_item_model_fn>(GetProcAddress(dll, "tessera_item_model"));
        mSetBool   = reinterpret_cast<tessera_set_bool_fn>(GetProcAddress(dll, "tessera_set_state_bool"));
        mSetString = reinterpret_cast<tessera_set_string_fn>(GetProcAddress(dll, "tessera_set_state_string"));
        mClear     = reinterpret_cast<tessera_clear_state_fn>(GetProcAddress(dll, "tessera_clear_state"));
        auto setInt = reinterpret_cast<tessera_set_int_fn>(GetProcAddress(dll, "tessera_set_state_int"));
        if (!mSetBool || !mSetString || !mClear || !setInt) return false;
        mSetInt = setInt;
        return true;
#else
        return false;
#endif
    }

    tessera_set_int_fn     mSetInt{};
    tessera_set_bool_fn    mSetBool{};
    tessera_set_string_fn  mSetString{};
    tessera_clear_state_fn mClear{};
    tessera_register_provider_fn   mRegister{};
    tessera_unregister_provider_fn mUnregister{};
    tessera_mark_dirty_fn          mMarkDirty{};
    tessera_item_model_fn          mItemModel{};
};

} // namespace tessera::api
#endif
