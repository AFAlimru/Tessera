#include "core/item/ItemDef.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>

namespace tessera::itemdef {
namespace {

using json = nlohmann::json;

std::string stripNs(std::string s) {
    if (s.starts_with("minecraft:")) s = s.substr(10);
    return s;
}

// Java effect colours of the potions (water and the effect-less ones share water's).
struct PotionColor {
    char const* id;
    uint32_t    rgb;
};
constexpr uint32_t    kWaterColor    = 0x385DC6;
constexpr PotionColor kPotionColors[] = {
    {"water", kWaterColor},       {"mundane", kWaterColor},       {"thick", kWaterColor},        {"awkward", kWaterColor},
    {"night_vision", 0xC2FF66},   {"invisibility", 0xF6F6F6},     {"leaping", 0xFDFF84},         {"fire_resistance", 0xFF9900},
    {"swiftness", 0x33EBFF},      {"slowness", 0x8BAFE0},         {"water_breathing", 0x98DAC0}, {"healing", 0xF82423},
    {"harming", 0xA9656A},        {"poison", 0x87A363},           {"regeneration", 0xCD5CAB},    {"strength", 0xFFC700},
    {"weakness", 0x484D48},       {"wither", 0x736156},           {"turtle_master", 0x8D82E6},   {"slow_falling", 0xF3CFB9},
    {"luck", 0x59C106},           {"wind_charged", 0xBDC9FF},     {"weaving", 0x78695A},         {"oozing", 0x99FFA3},
    {"infested", 0x8C9B8C},
};

uint32_t parseColor(json const& v, uint32_t fallback) {
    if (v.is_number_integer()) return uint32_t(v.get<int64_t>()) & 0xFFFFFFu;
    if (v.is_array() && v.size() >= 3) {
        auto ch = [&](size_t i) { return uint32_t(std::clamp(v[i].get<double>(), 0.0, 1.0) * 255.0 + 0.5); };
        return (ch(0) << 16) | (ch(1) << 8) | ch(2);
    }
    return fallback;
}

// A select case's "when" for a component, as comparable words.
void whenWords(json const& v, std::vector<std::string>& out) {
    if (v.is_string()) out.push_back(stripNs(v.get<std::string>()));
    else if (v.is_array()) {
        for (auto const& e : v) whenWords(e, out);
    } else if (v.is_object() && v.contains("potion") && v["potion"].is_string())
        out.push_back("potion:" + stripNs(v["potion"].get<std::string>()));
    else if (v.is_object() && v.contains("id") && v["id"].is_string()) out.push_back("stew:" + stripNs(v["id"].get<std::string>()));
    else out.push_back(v.dump());
}

NodePtr parseNode(json const& j, std::vector<std::string>& warnings, int depth = 0) {
    auto n = std::make_shared<Node>();
    if (depth > 1024 || !j.is_object()) return n;
    std::string const type = stripNs(j.value("type", std::string{}));
    auto child = [&](char const* key) -> NodePtr {
        auto it = j.find(key);
        return it == j.end() ? nullptr : parseNode(*it, warnings, depth + 1);
    };
    if (type == "model") {
        n->kind  = Node::Kind::Model;
        n->model = j.value("model", std::string{});
        if (auto it = j.find("tints"); it != j.end() && it->is_array())
            for (auto const& t : *it) {
                Tint tint;
                if (t.is_object()) {
                    tint.type = stripNs(t.value("type", std::string{}));
                    if (tint.type == "grass") tint.color = 0x7CBD6B;          // Java's inventory grass
                    else if (tint.type == "potion") tint.color = kWaterColor; // Java: no contents
                    if (auto v = t.find("value"); v != t.end()) tint.color = parseColor(*v, tint.color);
                    if (auto v = t.find("default"); v != t.end()) tint.color = parseColor(*v, tint.color);
                }
                n->tints.push_back(std::move(tint));
            }
    } else if (type == "composite") {
        n->kind = Node::Kind::Composite;
        if (auto it = j.find("models"); it != j.end() && it->is_array())
            for (auto const& m : *it) n->children.push_back(parseNode(m, warnings, depth + 1));
    } else if (type == "condition") {
        n->kind      = Node::Kind::Condition;
        n->property  = stripNs(j.value("property", std::string{}));
        n->component = stripNs(j.value("component", std::string{}));
        n->onTrue    = child("on_true");
        n->onFalse   = child("on_false");
        n->predicate = stripNs(j.value("predicate", std::string{}));
        if (auto v = j.find("value"); v != j.end() && v->is_array() && (n->predicate == "enchantments" || n->predicate == "stored_enchantments"))
            for (auto const& e : *v) {
                EnchantTest test;
                if (!e.is_object()) continue;
                if (auto ids = e.find("enchantments"); ids != e.end()) {
                    if (ids->is_string()) test.ids.push_back(stripNs(ids->get<std::string>()));
                    else if (ids->is_array())
                        for (auto const& id : *ids)
                            if (id.is_string()) test.ids.push_back(stripNs(id.get<std::string>()));
                }
                if (auto lv = e.find("levels"); lv != e.end()) {
                    if (lv->is_number()) test.min = test.max = lv->get<int>();
                    else if (lv->is_object()) test.min = lv->value("min", 1), test.max = lv->value("max", 255);
                }
                n->enchantTests.push_back(std::move(test));
            }
    } else if (type == "select") {
        n->kind      = Node::Kind::Select;
        n->property  = stripNs(j.value("property", std::string{}));
        n->component = stripNs(j.value("component", std::string{}));
        n->fallback  = child("fallback");
        if (auto it = j.find("cases"); it != j.end() && it->is_array())
            for (auto const& c : *it) {
                std::vector<std::string> when;
                if (auto w = c.find("when"); w != c.end()) whenWords(*w, when);
                n->cases.push_back({std::move(when), c.contains("model") ? parseNode(c["model"], warnings, depth + 1) : nullptr});
            }
    } else if (type == "range_dispatch") {
        n->kind      = Node::Kind::Range;
        n->property  = stripNs(j.value("property", std::string{}));
        n->scale     = j.value("scale", 1.f);
        n->normalize = j.value("normalize", true);
        n->fallback  = child("fallback");
        if (auto it = j.find("entries"); it != j.end() && it->is_array())
            for (auto const& e : *it)
                if (e.contains("model")) n->entries.push_back({e.value("threshold", 0.f), parseNode(e["model"], warnings, depth + 1)});
        std::stable_sort(n->entries.begin(), n->entries.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    } else if (type == "empty") {
        n->kind = Node::Kind::Empty;
    } else {
        n->kind = Node::Kind::Unsupported; // special (chests, heads, shields...), bundle/selected_item
        warnings.push_back("model type \"" + type + "\" is not supported (the game's icon stays)");
    }
    return n;
}

bool conditionKnown(Node const& n) {
    auto const& p = n.property;
    if (p == "damaged" || p == "broken") return true;
    if (p == "has_component")
        return n.component == "custom_name" || n.component == "enchantments" || n.component == "stored_enchantments" || n.component == "damage";
    return p == "component" && !n.enchantTests.empty();
}

bool selectKnown(Node const& n) {
    auto const& p = n.property;
    if (p == "display_context" || p == "main_hand") return true;
    return p == "component" && (n.component == "custom_name" || n.component == "potion_contents" || n.component == "suspicious_stew_effects");
}

// The model with its tint colours; potion tints take `potion` when given.
std::string specOf(Node const& n, uint32_t const* potion) {
    std::string spec = n.model;
    for (size_t i = 0; i < n.tints.size(); ++i) {
        uint32_t const c = n.tints[i].type == "potion" && potion ? *potion : n.tints[i].color;
        spec += std::format("{}{:06x}", i ? "," : "~", c & 0xFFFFFFu);
    }
    return spec;
}

void collect(NodePtr const& n, std::set<ModelSet>& out) {
    if (!n) return;
    switch (n->kind) {
    case Node::Kind::Model: {
        out.insert({specOf(*n, nullptr)}); // potion tints: the default colour
        bool potion = false;
        for (auto const& t : n->tints) potion |= t.type == "potion";
        if (potion)
            for (auto const& p : kPotionColors) out.insert({specOf(*n, &p.rgb)});
        break;
    }
    case Node::Kind::Empty: out.insert({}); break;
    case Node::Kind::Composite: {
        std::set<ModelSet> acc{{}};
        for (auto const& c : n->children) {
            std::set<ModelSet> part;
            collect(c, part);
            if (part.empty()) return; // an unsupported child: the composite cannot be drawn
            std::set<ModelSet> next;
            for (auto const& a : acc)
                for (auto const& b : part) {
                    ModelSet m = a;
                    m.insert(m.end(), b.begin(), b.end());
                    next.insert(std::move(m));
                }
            acc.swap(next);
            if (acc.size() > 64) return; // keep it small
        }
        out.insert(acc.begin(), acc.end());
        break;
    }
    // Choices Tessera cannot evaluate always come out one way (see condition / selectValue /
    // rangeValue): only that branch is listed, so no icons are drawn that are never shown.
    case Node::Kind::Condition:
        if (conditionKnown(*n)) collect(n->onTrue, out);
        collect(n->onFalse, out);
        break;
    case Node::Kind::Select:
        if (selectKnown(*n))
            for (auto const& [w, m] : n->cases) collect(m, out);
        collect(n->fallback, out);
        break;
    case Node::Kind::Range:
        if (n->property == "count" || n->property == "damage") {
            for (auto const& [t, m] : n->entries) collect(m, out);
            collect(n->fallback, out);
        } else {
            NodePtr best = nullptr;
            for (auto const& [t, m] : n->entries)
                if (t <= 0.f) best = m;
            collect(best ? best : n->fallback, out);
        }
        break;
    case Node::Kind::Unsupported: break;
    }
}

bool condition(Node const& n, Context const& ctx) {
    auto const& f = *ctx.facts;
    auto const& p = n.property;
    if (p == "damaged") return f.maxDamage > 0 && f.damage > 0;
    if (p == "broken") return f.maxDamage > 0 && f.damage >= f.maxDamage - 1;
    if (p == "has_component") {
        auto const& c = n.component;
        if (c == "custom_name") return f.name.has_value();
        if (c == "enchantments" || c == "stored_enchantments") return ctx.anyEnchantment;
        if (c == "damage") return f.damage > 0;
        return false;
    }
    if (p == "component" && !n.enchantTests.empty()) {
        for (auto const& test : n.enchantTests) {
            bool ok = false;
            if (test.ids.empty()) ok = ctx.anyEnchantment;
            else if (f.enchantLevel)
                for (auto const& id : test.ids) {
                    int const level = f.enchantLevel(id);
                    if (level >= std::max(1, test.min) && level <= test.max) ok = true;
                }
            if (!ok) return false;
        }
        return true;
    }
    return false; // using_item, selected, carried, keybind_down, fishing_rod/cast, custom_model_data, ...
}

std::optional<std::string> selectValue(Node const& n, Context const& ctx) {
    auto const& p = n.property;
    if (p == "display_context") return ctx.displayContext;
    if (p == "main_hand") return std::string("right");
    if (p == "component" && n.component == "custom_name" && ctx.facts->name) return *ctx.facts->name;
    if (p == "component" && n.component == "potion_contents" && ctx.facts->potion)
        if (auto id = ctx.facts->potion()) return "potion:" + *id;
    if (p == "component" && n.component == "suspicious_stew_effects" && ctx.facts->stewEffect) return "stew:" + *ctx.facts->stewEffect;
    return std::nullopt; // trim_material, charge_type, local_time, context_dimension, block_state, ...
}

float rangeValue(Node const& n, Context const& ctx) {
    auto const& f = *ctx.facts;
    auto const& p = n.property;
    if (p == "count") return n.normalize ? float(f.count) / float(std::max(1, ctx.maxStack)) : float(f.count);
    if (p == "damage") {
        if (f.maxDamage <= 0) return 0.f;
        return n.normalize ? float(f.damage) / float(f.maxDamage) : float(f.damage);
    }
    return 0.f; // custom_model_data, compass, time, cooldown, use_duration, crossbow/pull, ...
}

std::optional<ModelSet> eval(NodePtr const& n, Context const& ctx, int depth = 0) {
    if (!n || depth > 1024) return std::nullopt;
    switch (n->kind) {
    case Node::Kind::Model: {
        std::optional<uint32_t> potion;
        for (auto const& t : n->tints)
            if (t.type == "potion" && ctx.facts->potion)
                if (auto id = ctx.facts->potion()) potion = potionColor(*id);
        return ModelSet{specOf(*n, potion ? &*potion : nullptr)};
    }
    case Node::Kind::Empty: return ModelSet{};
    case Node::Kind::Composite: {
        ModelSet out;
        for (auto const& c : n->children) {
            auto part = eval(c, ctx, depth + 1);
            if (!part) return std::nullopt;
            out.insert(out.end(), part->begin(), part->end());
        }
        return out;
    }
    case Node::Kind::Condition: return eval(condition(*n, ctx) ? n->onTrue : n->onFalse, ctx, depth + 1);
    case Node::Kind::Select: {
        if (auto v = selectValue(*n, ctx))
            for (auto const& [when, m] : n->cases)
                if (std::find(when.begin(), when.end(), *v) != when.end()) return eval(m, ctx, depth + 1);
        return eval(n->fallback, ctx, depth + 1);
    }
    case Node::Kind::Range: {
        float const v    = rangeValue(*n, ctx) * n->scale;
        NodePtr     best = nullptr;
        for (auto const& [t, m] : n->entries)
            if (t <= v) best = m;
        return eval(best ? best : n->fallback, ctx, depth + 1);
    }
    case Node::Kind::Unsupported: return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

ModelSpec splitSpec(std::string const& spec) {
    ModelSpec out;
    auto const at = spec.find('~');
    out.model     = spec.substr(0, at);
    if (at == std::string::npos) return out;
    for (size_t i = at + 1; i < spec.size();) {
        size_t const end = std::min(spec.find(',', i), spec.size());
        out.tints.push_back(uint32_t(std::strtoul(spec.substr(i, end - i).c_str(), nullptr, 16)));
        i = end + 1;
    }
    return out;
}

std::optional<uint32_t> potionColor(std::string const& javaPotion) {
    std::string id = stripNs(javaPotion);
    for (char const* prefix : {"long_", "strong_"})
        if (id.starts_with(prefix)) id = id.substr(std::strlen(prefix));
    for (auto const& p : kPotionColors)
        if (id == p.id) return p.rgb;
    return std::nullopt;
}

std::optional<Definition> parse(std::string const& path, std::string const& text, std::string& error) {
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        error = "not valid JSON";
        return std::nullopt;
    }
    auto it = j.find("model");
    if (it == j.end()) {
        error = "no \"model\"";
        return std::nullopt;
    }
    Definition d;
    d.file = path;
    std::string name = path;
    if (auto slash = name.find_last_of('/'); slash != std::string::npos) name = name.substr(slash + 1);
    if (name.ends_with(".json")) name.resize(name.size() - 5);
    d.item = name;
    d.root = parseNode(*it, d.warnings);
    return d;
}

std::vector<ModelSet> outcomes(Definition const& def) {
    std::set<ModelSet> out;
    collect(def.root, out);
    return {out.begin(), out.end()};
}

std::optional<ModelSet> pick(Definition const& def, Context const& ctx) {
    if (!ctx.facts) return std::nullopt;
    return eval(def.root, ctx);
}

} // namespace tessera::itemdef
