// Tessera - Java 1.21.4+ item model definitions (assets/<ns>/items/<id>.json), platform independent.
//
// A definition is a tree: "model" leaves, "composite" stacks, and "condition" / "select" /
// "range_dispatch" choices on the stack's state. This part parses the tree, lists every set of
// models it can end in (so their icons can be drawn ahead of time) and picks the set for a stack.
#pragma once

#include "core/item/Cit.h" // Facts

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tessera::itemdef {

struct Node;
using NodePtr = std::shared_ptr<Node const>;

// A model's tint source: layer / tintindex i takes tints[i].
struct Tint {
    std::string type;             // without "minecraft:": constant, dye, grass, potion, ...
    uint32_t    color = 0xFFFFFF; // the constant, or the default when the stack gives none (RGB)
};

// One entry of an enchantments predicate: any of ids at a level in [min, max].
struct EnchantTest {
    std::vector<std::string> ids; // empty: any enchantment
    int                      min = 1, max = 255;
};

struct Node {
    enum class Kind { Model, Composite, Condition, Select, Range, Empty, Unsupported } kind = Kind::Unsupported;
    std::string          model;    // Model: model reference ("item/x", "ns:item/x")
    std::vector<Tint>    tints;    // Model
    std::string              predicate;    // Condition on a component: predicate id without ns
    std::vector<EnchantTest> enchantTests; // ... its enchantment tests (all must hold)
    std::vector<NodePtr> children; // Composite
    // Condition / Select / Range
    std::string                                         property; // without "minecraft:"
    std::string                                         component; // has_component / component: id without ns
    NodePtr                                             onTrue, onFalse, fallback;
    std::vector<std::pair<std::vector<std::string>, NodePtr>> cases; // Select: when (one or a list) -> model
    std::vector<std::pair<float, NodePtr>>               entries;   // Range: threshold -> model, ascending
    float                                               scale     = 1.f;
    bool                                                normalize = true;
};

struct Definition {
    std::string              file;
    std::string              item; // id without namespace (the file name)
    NodePtr                  root;
    std::vector<std::string> warnings;
};

std::optional<Definition> parse(std::string const& path, std::string const& text, std::string& error);

// One way a definition can come out: the models drawn on top of each other (empty: nothing).
// Each entry is a model spec: the model reference, then "~rrggbb,rrggbb" when it is tinted.
using ModelSet = std::vector<std::string>;

struct ModelSpec {
    std::string           model;
    std::vector<uint32_t> tints; // RGB per layer / tintindex
};
ModelSpec splitSpec(std::string const& spec);

// The colour Java gives a potion (its liquid layer), by Java potion id; nullopt when unknown.
std::optional<uint32_t> potionColor(std::string const& javaPotion);

// Every model set the tree can produce (union over choices, product over composites).
// "Unsupported" branches produce no set: the game's own icon stays for them.
std::vector<ModelSet> outcomes(Definition const& def);

struct Context {
    cit::Facts const* facts = nullptr;
    std::string       displayContext = "gui"; // gui, firstperson_righthand, ...
    int               maxStack       = 64;
    bool              anyEnchantment = false;
};

// The model set for a stack, or nullopt when the tree ends in something unsupported.
std::optional<ModelSet> pick(Definition const& def, Context const& ctx);

} // namespace tessera::itemdef
