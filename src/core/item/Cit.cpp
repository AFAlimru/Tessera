#include "core/item/Cit.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <regex>
#include <string_view>

namespace tessera::cit {
namespace {

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(uint8_t(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(uint8_t(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::vector<std::string> words(std::string const& s) {
    std::vector<std::string> out;
    std::string              cur;
    for (char c : s) {
        if (std::isspace(uint8_t(c))) {
            if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string stripNs(std::string s) {
    if (auto colon = s.find(':'); colon != std::string::npos) s = s.substr(colon + 1);
    return s;
}

std::optional<long> toLong(std::string_view s) {
    long v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

// "5", "1-10", "10-", "-3", "50%-100%"
std::vector<Range> ranges(std::string const& value, std::vector<std::string>& warnings) {
    std::vector<Range> out;
    for (auto w : words(value)) {
        Range r;
        if (w.find('%') != std::string::npos) {
            r.percent = true;
            w.erase(std::remove(w.begin(), w.end(), '%'), w.end());
        }
        auto const dash = w.find('-', 1);
        if (dash == std::string::npos) {
            auto v = toLong(w);
            if (!v) {
                warnings.push_back("bad range " + w);
                continue;
            }
            r.lo = r.hi = *v;
        } else {
            auto a = w.substr(0, dash), b = w.substr(dash + 1);
            r.lo   = a.empty() ? LONG_MIN : toLong(a).value_or(LONG_MIN);
            r.hi   = b.empty() ? LONG_MAX : toLong(b).value_or(LONG_MAX);
        }
        out.push_back(r);
    }
    return out;
}

bool inRanges(std::vector<Range> const& rs, long v, long max) {
    if (rs.empty()) return true;
    for (auto const& r : rs) {
        long value = v;
        if (r.percent) value = max > 0 ? (v * 100 + max / 2) / max : 0;
        if (value >= r.lo && value <= r.hi) return true;
    }
    return false;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

std::string texturePath(std::string name, std::string const& dir) {
    if (!name.ends_with(".png")) name += ".png";
    if (name.starts_with("./")) return dir + name.substr(2);
    if (auto colon = name.find(':'); colon != std::string::npos && name.find('/') > colon)
        return "assets/" + name.substr(0, colon) + "/textures/" + name.substr(colon + 1);
    if (name.starts_with("assets/")) return name;
    if (name.find('/') != std::string::npos) {
        if (name.starts_with("optifine/") || name.starts_with("textures/")) return "assets/minecraft/" + name;
        return "assets/minecraft/textures/" + name; // "item/x" -> the item texture
    }
    return dir + name;
}

} // namespace

bool NameMatch::matches(std::string const& name) const {
    std::string const subject = ignoreCase ? lower(name) : name;
    std::string const want    = ignoreCase ? lower(text) : text;
    switch (kind) {
    case Kind::Exact: return subject == want;
    case Kind::Pattern: {
        std::string re;
        for (char c : want) {
            if (c == '*') re += ".*";
            else if (c == '?') re += '.';
            else if (std::string_view("\\^$.|+()[]{}").find(c) != std::string_view::npos) re += '\\', re += c;
            else re += c;
        }
        try {
            return std::regex_match(subject, std::regex(re));
        } catch (...) {
            return false;
        }
    }
    case Kind::Regex:
        try {
            return std::regex_match(subject, std::regex(want));
        } catch (...) {
            return false;
        }
    }
    return false;
}

std::optional<Rule> parse(std::string const& path, std::string const& text, std::string& error) {
    std::map<std::string, std::string> kv;
    size_t                             start = 0;
    std::string                        pending;
    while (start <= text.size()) {
        auto const  end  = text.find('\n', start);
        std::string line = trim(std::string_view(text).substr(start, end == std::string::npos ? std::string::npos : end - start));
        start            = end == std::string::npos ? text.size() + 1 : end + 1;
        if (!pending.empty()) line = pending + line, pending.clear();
        if (line.ends_with('\\')) {
            pending = line.substr(0, line.size() - 1) + " ";
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == '!') continue;
        auto const eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(std::string_view(line).substr(0, eq));
        // "components.minecraft\:custom_name" -> "components.custom_name"
        std::string clean;
        for (size_t i = 0; i < key.size(); ++i)
            if (key[i] != '\\') clean += key[i];
        if (auto at = clean.find("minecraft:"); at != std::string::npos) clean.erase(at, 10);
        kv[clean] = trim(std::string_view(line).substr(eq + 1));
    }
    auto get = [&](char const* key) -> std::string const* {
        auto it = kv.find(key);
        return it == kv.end() ? nullptr : &it->second;
    };

    Rule r;
    r.file                = path;
    auto const        cut = path.rfind('/');
    std::string const dir = cut == std::string::npos ? std::string{} : path.substr(0, cut + 1);
    std::string       base = cut == std::string::npos ? path : path.substr(cut + 1);
    if (base.ends_with(".properties")) base.resize(base.size() - 11);

    if (auto const* t = get("type"); t && *t != "item") {
        error = "type " + *t + " is not supported yet";
        return std::nullopt;
    }
    auto const* items = get("items");
    if (!items) items = get("matchItems");
    if (items)
        for (auto const& w : words(*items)) r.items.push_back(stripNs(w));
    else r.items.push_back(base);

    if (auto const* m = get("model")) {
        std::string model = *m;
        if (model.starts_with("./")) model = dir + model.substr(2) + (model.ends_with(".json") ? "" : ".json");
        else model = stripNs(model);
        r.model = model;
    }
    if (auto const* t = get("texture")) r.texture = texturePath(*t, dir);
    if (r.model.empty() && r.texture.empty()) r.texture = dir + base + ".png";

    if (auto const* v = get("damage")) r.damage = ranges(*v, r.warnings);
    if (auto const* v = get("stackSize")) r.stackSize = ranges(*v, r.warnings);
    if (auto const* v = get("enchantmentLevels")) r.enchantmentLevels = ranges(*v, r.warnings);
    auto const* ids = get("enchantmentIDs");
    if (!ids) ids = get("enchantments");
    if (ids)
        for (auto const& w : words(*ids)) r.enchantmentIds.push_back(stripNs(w));
    if (auto const* v = get("weight")) r.weight = int(toLong(*v).value_or(0));

    std::string const* name = get("nbt.display.Name");
    if (!name) name = get("components.custom_name");
    if (name) {
        NameMatch nm;
        std::string v = *name;
        auto prefix   = [&](std::string_view p) {
            if (!v.starts_with(p)) return false;
            v = v.substr(p.size());
            return true;
        };
        if (prefix("pattern:")) nm.kind = NameMatch::Kind::Pattern;
        else if (prefix("ipattern:")) nm.kind = NameMatch::Kind::Pattern, nm.ignoreCase = true;
        else if (prefix("regex:")) nm.kind = NameMatch::Kind::Regex;
        else if (prefix("iregex:")) nm.kind = NameMatch::Kind::Regex, nm.ignoreCase = true;
        nm.text = v;
        r.name  = nm;
    }
    std::string const* potion = get("nbt.Potion");
    if (!potion) potion = get("components.potion_contents.potion");
    if (!potion) potion = get("components.potion_contents");
    if (potion)
        for (auto const& w : words(*potion)) r.potions.push_back(stripNs(w));

    for (auto const& [key, value] : kv)
        if ((key.starts_with("nbt.") || key.starts_with("components.")) && key != "nbt.display.Name" && key != "components.custom_name"
            && key != "nbt.Potion" && !key.starts_with("components.potion_contents"))
            r.warnings.push_back("condition " + key + " is not supported yet (ignored)");
    return r;
}

bool matches(Rule const& rule, Facts const& f) {
    if (std::find(rule.items.begin(), rule.items.end(), f.id) == rule.items.end()) return false;
    if (!inRanges(rule.damage, f.damage, f.maxDamage)) return false;
    if (!inRanges(rule.stackSize, f.count, 64)) return false;
    if (rule.name) {
        if (!f.name || !rule.name->matches(*f.name)) return false;
    }
    if (!rule.enchantmentIds.empty() || !rule.enchantmentLevels.empty()) {
        if (!f.enchantLevel) return false;
        bool any = false;
        if (rule.enchantmentIds.empty()) return false; // levels without ids: not supported
        for (auto const& id : rule.enchantmentIds) {
            int const level = f.enchantLevel(id);
            if (level > 0 && inRanges(rule.enchantmentLevels, level, 0)) any = true;
        }
        if (!any) return false;
    }
    if (!rule.potions.empty()) {
        auto p = f.potion ? f.potion() : std::nullopt;
        if (!p || std::find(rule.potions.begin(), rule.potions.end(), *p) == rule.potions.end()) return false;
    }
    return true;
}

} // namespace tessera::cit
