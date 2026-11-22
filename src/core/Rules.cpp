#include "core/Rules.h"

#include <nlohmann/json.hpp>

#include <charconv>

namespace tessera {

using nlohmann::json;

std::string StateValue::toString() const {
    switch (kind) {
    case Kind::Int: return std::to_string(i);
    case Kind::Bool: return i ? "true" : "false";
    default: return s;
    }
}

std::string_view sideName(Side s) {
    switch (s) {
    case Side::Bottom: return "bottom";
    case Side::Top: return "top";
    case Side::North: return "north";
    case Side::South: return "south";
    case Side::East: return "east";
    case Side::West: return "west";
    default: return "center";
    }
}

static std::string withNamespace(std::string name) {
    if (name.find(':') == std::string::npos) name = "minecraft:" + name;
    return name;
}

static bool parseInt(std::string_view s, int64_t& out) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

TextureRef parseTextureRef(std::string_view s) {
    TextureRef ref;
    auto       hash = s.rfind('#');
    int64_t    v    = 0;
    if (hash != std::string_view::npos && parseInt(s.substr(hash + 1), v)) {
        ref.name    = std::string(s.substr(0, hash));
        ref.variant = int(v);
    } else {
        ref.name = std::string(s);
    }
    return ref;
}

bool Condition::matches(StateMap const& states) const {
    auto it = states.find(key);
    if (it == states.end()) return false;
    auto const& v = it->second;
    for (auto const& a : alts) {
        switch (a.kind) {
        case Alt::Kind::Int:
        case Alt::Kind::Bool:
            if (v.kind != StateValue::Kind::String && v.i == a.lo) return true;
            break;
        case Alt::Kind::Range:
            if (v.kind != StateValue::Kind::String && v.i >= a.lo && v.i <= a.hi) return true;
            break;
        case Alt::Kind::String:
            if (v.toString() == a.s) return true;
            break;
        }
    }
    return false;
}

bool Rule::matchesBlock(std::string_view name) const {
    for (auto const& b : blocks)
        if (b == name) return true;
    return false;
}

std::span<uint32_t const> RuleSet::rulesFor(std::string_view name) const {
    auto it = byBlock.find(std::string(name));
    return it == byBlock.end() ? std::span<uint32_t const>{} : std::span<uint32_t const>{it->second};
}

bool RuleSet::hasBlock(std::string_view name) const { return !rulesFor(name).empty(); }

bool RuleSet::blockUsesVirtual(std::string_view name) const {
    for (auto i : rulesFor(name))
        if (rules[i].usesVirtual) return true;
    return false;
}

bool RuleSet::blockUsesKey(std::string_view name, std::string_view key) const {
    std::string const placeholder = "{" + std::string(key) + "}";
    for (auto i : rulesFor(name)) {
        auto const& r = rules[i];
        for (auto const& c : r.when)
            if (c.key == key) return true;
        for (auto const& [src, dst] : r.replace)
            if (dst.find(placeholder) != std::string::npos) return true;
    }
    return false;
}

static bool parseAlt(json const& j, Condition::Alt& out) {
    using K = Condition::Alt::Kind;
    if (j.is_boolean()) {
        out.kind = K::Bool;
        out.lo   = j.get<bool>() ? 1 : 0;
        return true;
    }
    if (j.is_number_integer()) {
        out.kind = K::Int;
        out.lo   = j.get<int64_t>();
        return true;
    }
    if (j.is_string()) {
        auto s   = j.get<std::string>();
        auto dot = s.find("..");
        int64_t lo, hi;
        if (dot != std::string::npos && parseInt(std::string_view(s).substr(0, dot), lo)
            && parseInt(std::string_view(s).substr(dot + 2), hi)) {
            out.kind = K::Range;
            out.lo   = lo;
            out.hi   = hi;
        } else {
            out.kind = K::String;
            out.s    = std::move(s);
        }
        return true;
    }
    return false;
}

static uint8_t parseSides(json const& j, std::vector<std::string>& warnings) {
    uint8_t mask = 0;
    auto    add  = [&](std::string const& s) {
        if (s == "bottom" || s == "down") mask |= sideBit(Side::Bottom);
        else if (s == "top" || s == "up") mask |= sideBit(Side::Top);
        else if (s == "north") mask |= sideBit(Side::North);
        else if (s == "south") mask |= sideBit(Side::South);
        else if (s == "east") mask |= sideBit(Side::East);
        else if (s == "west") mask |= sideBit(Side::West);
        else if (s == "side" || s == "sides" || s == "wall")
            mask |= sideBit(Side::North) | sideBit(Side::South) | sideBit(Side::East) | sideBit(Side::West);
        else if (s == "center") mask |= sideBit(Side::Center);
        else if (s == "any") mask |= kAllSides;
        else warnings.push_back("unknown side '" + s + "'");
    };
    if (j.is_string()) add(j.get<std::string>());
    else if (j.is_array())
        for (auto const& e : j)
            if (e.is_string()) add(e.get<std::string>());
    return mask ? mask : kAllSides;
}

static bool parseHexColor(std::string_view s, uint32_t& rgba) {
    if (s.size() != 7 || s[0] != '#') return false;
    uint32_t v = 0;
    auto     r = std::from_chars(s.data() + 1, s.data() + 7, v, 16);
    if (r.ec != std::errc{}) return false;
    uint32_t R = (v >> 16) & 0xFF, G = (v >> 8) & 0xFF, B = v & 0xFF;
    rgba       = R | (G << 8) | (B << 16) | (0xFFu << 24);
    return true;
}

RuleSet RuleSet::parse(std::string_view text) {
    RuleSet set;
    json    doc = json::parse(text, nullptr, false, true); // allow comments
    if (doc.is_discarded() || !doc.is_object()) {
        set.warnings.push_back("state_textures.json is not a valid JSON object");
        return set;
    }
    auto rules = doc.find("rules");
    if (rules == doc.end() || !rules->is_array()) {
        set.warnings.push_back("missing \"rules\" array");
        return set;
    }
    size_t index = 0;
    for (auto const& jr : *rules) {
        auto where = "rule #" + std::to_string(index++);
        if (!jr.is_object()) {
            set.warnings.push_back(where + ": not an object");
            continue;
        }
        Rule r;
        if (auto b = jr.find("block"); b != jr.end()) {
            if (b->is_string()) r.blocks.push_back(withNamespace(b->get<std::string>()));
            else if (b->is_array())
                for (auto const& e : *b)
                    if (e.is_string()) r.blocks.push_back(withNamespace(e.get<std::string>()));
        }
        if (r.blocks.empty()) {
            set.warnings.push_back(where + ": missing \"block\"");
            continue;
        }
        if (auto w = jr.find("when"); w != jr.end() && w->is_object()) {
            for (auto const& [key, val] : w->items()) {
                Condition c;
                c.key = key;
                Condition::Alt alt;
                if (val.is_array()) {
                    for (auto const& e : val)
                        if (parseAlt(e, alt)) c.alts.push_back(alt);
                } else if (parseAlt(val, alt)) {
                    c.alts.push_back(alt);
                }
                if (c.alts.empty()) set.warnings.push_back(where + ": bad value for state '" + key + "'");
                else r.when.push_back(std::move(c));
            }
        }
        if (auto o = jr.find("on"); o != jr.end()) r.sideMask = parseSides(*o, set.warnings);
        if (auto rot = jr.find("rotation"); rot != jr.end() && rot->is_object()) {
            auto quarter = [&](char const* axis) -> uint8_t {
                auto a = rot->find(axis);
                if (a == rot->end() || !a->is_number_integer()) return 0;
                int deg = a->get<int>();
                if (deg % 90 != 0) set.warnings.push_back(where + ": rotation must be a multiple of 90");
                return uint8_t(((deg / 90) % 4 + 4) % 4);
            };
            r.rotX = quarter("x");
            r.rotY = quarter("y");
        }
        if (auto rep = jr.find("replace"); rep != jr.end() && rep->is_object()) {
            for (auto const& [src, dst] : rep->items())
                if (dst.is_string()) r.replace.emplace_back(parseTextureRef(src), dst.get<std::string>());
        }
        if (auto g = jr.find("geometry"); g != jr.end() && g->is_array()) {
            auto quads = std::make_shared<std::vector<GeoQuad>>();
            for (auto const& jq : *g) {
                if (!jq.is_object()) continue;
                GeoQuad q;
                q.texture = jq.value("t", std::string{});
                auto p = jq.find("p");
                auto u = jq.find("uv");
                if (q.texture.empty() || p == jq.end() || u == jq.end() || !p->is_array() || !u->is_array() || p->size() != 12
                    || u->size() != 8)
                    continue;
                for (size_t i = 0; i < 12; ++i) q.pos[i] = (*p)[i].get<float>();
                for (size_t i = 0; i < 8; ++i) q.uv[i] = (*u)[i].get<float>();
                q.dir   = int8_t(jq.value("d", -1));
                q.cull  = int8_t(jq.value("c", -1));
                q.shade = jq.value("s", true);
                q.tint  = int8_t(jq.value("ti", -1));
                q.tintColor = jq.value("tc", -1);
                quads->push_back(std::move(q));
            }
            if (!quads->empty()) r.geometry = std::move(quads);
            r.cutout = jr.value("cutout", false);
            r.translucent = jr.value("translucent", false);
            r.item   = jr.value("item", false);
            auto display = [&](char const* key) -> std::optional<std::array<float, 9>> {
                auto g = jr.find(key);
                if (g == jr.end() || !g->is_array() || g->size() != 9) return std::nullopt;
                std::array<float, 9> t{};
                for (size_t i = 0; i < 9; ++i) t[i] = (*g)[i].get<float>();
                return t;
            };
            r.gui = display("gui");
        }
        if (auto pk = jr.find("pick"); pk != jr.end() && pk->is_array() && pk->size() == 3) {
            r.pickLo    = uint16_t((*pk)[0].get<int>());
            r.pickHi    = uint16_t((*pk)[1].get<int>());
            r.pickTotal = uint16_t((*pk)[2].get<int>());
        }
        if (r.replace.empty() && !r.geometry) {
            set.warnings.push_back(where + ": empty \"replace\"");
            continue;
        }
        if (auto t = jr.find("tint"); t != jr.end() && t->is_string()) {
            auto s = t->get<std::string>();
            if (s == "none") r.tint = TintMode::None;
            else if (s == "keep") r.tint = TintMode::Keep;
            else if (parseHexColor(s, r.tintRgba)) r.tint = TintMode::Color;
            else set.warnings.push_back(where + ": bad tint '" + s + "'");
        }
        for (auto const& c : r.when)
            if (isVirtualKey(c.key)) r.usesVirtual = true;
        for (auto const& [src, dst] : r.replace) // a "{ns:key}" placeholder
            for (size_t open = dst.find('{'); open != std::string::npos; open = dst.find('{', open + 1)) {
                auto close = dst.find('}', open);
                if (close != std::string::npos && isVirtualKey(std::string_view(dst).substr(open + 1, close - open - 1)))
                    r.usesVirtual = true;
            }
        set.rules.push_back(std::move(r));
    }
    for (uint32_t i = 0; i < set.rules.size(); ++i)
        for (auto const& b : set.rules[i].blocks) {
            set.byBlock[b].push_back(i);
            if (set.rules[i].item) set.itemBlocks.insert(b);
        }
    return set;
}

std::string expandTemplate(std::string_view tpl, StateMap const& states, bool* ok) {
    std::string out;
    bool        good = true;
    size_t      i    = 0;
    while (i < tpl.size()) {
        auto open = tpl.find('{', i);
        if (open == std::string_view::npos) {
            out.append(tpl.substr(i));
            break;
        }
        auto close = tpl.find('}', open);
        if (close == std::string_view::npos) {
            out.append(tpl.substr(i));
            break;
        }
        out.append(tpl.substr(i, open - i));
        auto key = tpl.substr(open + 1, close - open - 1);
        if (auto it = states.find(key); it != states.end()) out += it->second.toString();
        else {
            good = false;
            out.append(tpl.substr(open, close - open + 1));
        }
        i = close + 1;
    }
    if (ok) *ok = good;
    return out;
}

} // namespace tessera
