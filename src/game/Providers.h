// Tessera - states computed by other mods (public API: tessera_register_provider).
#pragma once

#include "core/Rules.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::game::providers {

using Fn = bool (*)(int dim, int x, int y, int z, char const* block, void* user, void* out);

bool add(std::string_view key, Fn fn, void* user);
bool remove(std::string_view key);

// Keys with a provider (changes bump the rule generation so caches refresh).
std::vector<std::string> keys();

// Chunk-build threads: the provider's value for this block, if it gave one.
std::optional<StateValue> call(std::string const& key, int dim, int x, int y, int z, std::string const& block);

} // namespace tessera::game::providers
