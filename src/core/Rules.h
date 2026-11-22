// Tessera - block-state dependent textures for Minecraft Bedrock.
// Platform-independent rule model: parsing, state matching, name templating.
// Nothing in src/core may include game headers, so it can be reused on Android.
#pragma once
#include <memory>
#include <array>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tessera {

// One block-state value as read from the block's serialization NBT.
struct StateValue {
    enum class Kind : uint8_t { Int, Bool, String };
    Kind        kind{Kind::Int};
    int64_t     i{0};
    std::string s;

    [[nodiscard]] std::string toString() const;
};

using StateMap = std::map<std::string, StateValue, std::less<>>;

// Where a quad sits inside its block cell. Derived from geometry, so it works
// for any shape: redstone dust lying on the floor is Bottom, dust climbing the
// east wall is East.
enum class Side : uint8_t { Bottom, Top, North, South, East, West, Center };
inline constexpr uint8_t sideBit(Side s) { return uint8_t(1u << uint8_t(s)); }
inline constexpr uint8_t kAllSides = 0x7F;
std::string_view         sideName(Side s);

// A texture reference: terrain_texture.json name plus optional array index ("name#1").
struct TextureRef {
    std::string name;
    int         variant{0};
};

enum class TintMode : uint8_t { Keep, None, Color };

struct Condition {
    std::string key;
    struct Alt {
        enum class Kind : uint8_t { Int, Bool, String, Range } kind{Kind::Int};
        int64_t     lo{0}, hi{0};
        std::string s;
    };
    std::vector<Alt> alts; // any-of
    [[nodiscard]] bool matches(StateMap const& states) const;
};

// P5: one quad of a block's own shape, in block space (0..1), with UVs within its texture.
struct GeoQuad {
    std::string           texture;  // terrain atlas name
    std::array<float, 12> pos{};    // four corners, x y z each
    std::array<float, 8>  uv{};     // four corners, u v each (0..1 of the texture)
    int8_t                dir  = -1; // facing: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east
    int8_t                cull = -1; // hidden when the neighbour this way is solid
    bool                  shade = true;
    int8_t                tint  = -1; // tintindex: coloured by the biome / block tint
    int32_t               tintColor = -1; // ... or by this fixed colour (0xRRGGBB), when >= 0
};

struct Rule {
    std::vector<std::string>                          blocks; // full names, "minecraft:" added if missing
    std::vector<Condition>                            when;
    uint8_t                                           sideMask{kAllSides};
    // Java blockstate rotation (quarter turns, x applied before y). Wildcard
    // sources ("*:north") then name faces of the unrotated model, and their
    // textures turn with the block, so one set of textures serves every facing.
    uint8_t                                           rotX{0}, rotY{0};
    std::vector<std::pair<TextureRef, std::string>>   replace; // source -> target template
    TintMode                                          tint{TintMode::Keep};
    uint32_t                                          tintRgba{0xFFFFFFFF}; // packed R | G<<8 | B<<16 | A<<24
    bool usesVirtual{false}; // reads a computed "tessera:" state (in "when" or a template)
    std::shared_ptr<std::vector<GeoQuad> const> geometry; // P5: replaces the block's own shape
    bool                                        cutout{false}; // geometry has see-through pixels
    bool                                        translucent{false}; // ... many half see-through (keep a blended layer)
    // Applies only to the block drawn as an item (in the hand, in a slot), not in the world.
    bool                                        item{false};
    // One of several alternatives chosen by block position (Java's weighted random
    // variants): applies where seed % pickTotal lies in [pickLo, pickHi). Total 0: always.
    uint16_t                                    pickLo{0}, pickHi{0}, pickTotal{0};
    // Item form: Java's display.gui (rotation xyz, translation xyz in pixels, scale xyz).
    std::optional<std::array<float, 9>>         gui;
    [[nodiscard]] bool matchesBlock(std::string_view name) const;
};

struct RuleSet {
    std::vector<Rule>        rules;
    std::vector<std::string> warnings;
    // Rule indices per block name, in file order (packs can carry thousands of rules).
    std::unordered_map<std::string, std::vector<uint32_t>> byBlock;
    // Blocks some rule gives an item-form shape (see Rule::item).
    std::unordered_set<std::string> itemBlocks;
    [[nodiscard]] std::span<uint32_t const> rulesFor(std::string_view name) const;

    // Parse a state_textures.json document. Never throws; problems go to warnings.
    static RuleSet parse(std::string_view json);
    [[nodiscard]] bool hasBlock(std::string_view name) const;
    [[nodiscard]] bool blockUsesVirtual(std::string_view name) const;
    // Whether any rule for this block reads `key` (in "when" or a "{key}" template).
    [[nodiscard]] bool blockUsesKey(std::string_view name, std::string_view key) const;
};

// Computed states are not part of the block's own states; they come from the
// world around the block or its block entity ("tessera:extended", "tessera:note"), or
// from other mods through the API ("mymod:foo"). Any namespaced key other than
// "minecraft:" (which Bedrock uses for some real states) is one.
inline constexpr std::string_view kVirtualPrefix = "tessera:";
inline bool isVirtualKey(std::string_view key) {
    return key.find(':') != std::string_view::npos && !key.starts_with("minecraft:");
}

TextureRef parseTextureRef(std::string_view s);

// Substitute {state} placeholders. Unknown placeholders are left as-is and reported.
std::string expandTemplate(std::string_view tpl, StateMap const& states, bool* ok = nullptr);

} // namespace tessera
