// Tessera - custom item textures in the OptiFine CIT format (platform independent).
//
// Pack authors write optifine/cit/**.properties as for Java (OptiFine, CIT Resewn).
// This part parses them and matches an item's facts; the game layer reads the facts
// from the item stack and swaps the icon.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tessera::cit {

struct Range {
    long lo = 0, hi = 0;
    bool percent = false; // damage given as a share of the maximum
};

// A name test: exact text, pattern (* and ?), or regex; optionally case-insensitive.
struct NameMatch {
    enum class Kind : uint8_t { Exact, Pattern, Regex } kind = Kind::Exact;
    bool        ignoreCase = false;
    std::string text;
    bool        matches(std::string const& name) const;
};

struct Rule {
    std::string              file;
    std::vector<std::string> items;  // Java ids, no namespace
    std::string              texture; // pack path of a PNG, or empty
    std::string              model;   // item model reference ("item/x"), or empty
    std::vector<Range>       damage, stackSize, enchantmentLevels;
    std::vector<std::string> enchantmentIds; // Java ids, no namespace
    std::optional<NameMatch> name;
    std::vector<std::string> potions; // Java potion ids, no namespace
    int                      weight = 0;
    std::vector<std::string> warnings;
};

std::optional<Rule> parse(std::string const& path, std::string const& text, std::string& error);

// What the game layer knows about a stack. Lookups that cost something are callbacks.
struct Facts {
    std::string                                   id; // Java-style id, no namespace
    long                                          damage = 0, maxDamage = 0, count = 1;
    std::optional<std::string>                    name;
    std::function<int(std::string const&)>        enchantLevel; // Java enchantment id -> level (0 = none)
    std::function<std::optional<std::string>()>   potion;       // Java potion id
    std::optional<std::string>                    stewEffect;   // suspicious stew: Java effect id
};

bool matches(Rule const& rule, Facts const& facts);

} // namespace tessera::cit
