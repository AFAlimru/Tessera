#include "core/ctm/Ctm.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <string_view>

namespace tessera::ctm {
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

std::vector<std::string> split(std::string const& s, char sep) {
    std::vector<std::string> out;
    size_t                   start = 0;
    while (true) {
        auto const at = s.find(sep, start);
        out.push_back(s.substr(start, at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return out;
}

std::optional<int> toInt(std::string const& s) {
    int  v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

std::string stripMinecraft(std::string s) {
    if (s.starts_with("minecraft:")) s = s.substr(10);
    return s;
}

std::string dirOf(std::string const& path) {
    auto const slash = path.rfind('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
}

std::string fileOf(std::string const& path) {
    auto const slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A tile reference to a pack path.
std::string tilePath(std::string name, std::string const& dir) {
    if (!name.ends_with(".png")) name += ".png";
    if (auto colon = name.find(':'); colon != std::string::npos && name.find('/') > colon) // namespace:path
        return "assets/" + name.substr(0, colon) + "/" + name.substr(colon + 1);
    if (name.starts_with("~/")) return "assets/minecraft/optifine/" + name.substr(2);
    if (name.starts_with("assets/")) return name;
    if (name.find('/') != std::string::npos) return "assets/minecraft/" + name;
    return dir + name;
}

std::optional<BlockMatch> parseBlock(std::string const& token) {
    auto parts = split(token, ':');
    if (parts.empty() || parts[0].empty()) return std::nullopt;
    size_t i = 0;
    BlockMatch m;
    if (parts.size() >= 2 && parts[0].find('=') == std::string::npos && parts[1].find('=') == std::string::npos) {
        if (parts[0] != "minecraft") return std::nullopt; // other namespaces: no Bedrock block to map to
        i = 1;
    }
    m.name = parts[i++];
    if (toInt(m.name)) return std::nullopt; // numeric ids (pre-1.13)
    for (; i < parts.size(); ++i) {
        auto const eq = parts[i].find('=');
        if (eq == std::string::npos) continue;
        m.props.emplace_back(parts[i].substr(0, eq), split(parts[i].substr(eq + 1), ','));
    }
    return m;
}

std::string tileName(std::string s) {
    s = stripMinecraft(std::move(s));
    if (s.starts_with("block/")) s = s.substr(6);
    else if (s.starts_with("blocks/")) s = s.substr(7);
    if (s.ends_with(".png")) s.resize(s.size() - 4);
    return s;
}

std::vector<std::pair<int, int>> parseHeights(std::string const& value, std::vector<std::string>& warnings) {
    std::vector<std::pair<int, int>> out;
    for (auto w : words(value)) {
        // "a-b", "(-64)-(-1)", "a"
        std::string clean;
        std::vector<std::string> nums;
        std::string              cur;
        bool                     inParen = false;
        for (char c : w) {
            if (c == '(') inParen = true;
            else if (c == ')') inParen = false;
            else if (c == '-' && !inParen && !cur.empty()) nums.push_back(cur), cur.clear();
            else cur += c;
        }
        nums.push_back(cur);
        auto a = toInt(nums[0]);
        auto b = nums.size() > 1 ? toInt(nums[1]) : a;
        if (!a || !b) {
            warnings.push_back("bad height range: " + w);
            continue;
        }
        out.emplace_back(std::min(*a, *b), std::max(*a, *b));
    }
    return out;
}

size_t requiredTiles(Rule const& r) {
    switch (r.method) {
    case Method::Ctm:
    case Method::OverlayCtm: return 47;
    case Method::Compact: return 5;
    case Method::Horizontal:
    case Method::Vertical:
    case Method::HorizontalVertical:
    case Method::VerticalHorizontal: return 4;
    case Method::Top:
    case Method::Fixed:
    case Method::OverlayFixed:
    case Method::Random:
    case Method::OverlayRandom: return 1;
    case Method::Repeat:
    case Method::OverlayRepeat: return size_t(std::max(1, r.width * r.height));
    case Method::Overlay: return 17;
    }
    return 1;
}

uint32_t mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352d;
    h ^= h >> 15;
    h *= 0x846ca68b;
    h ^= h >> 16;
    return h;
}

} // namespace

std::optional<Method> methodFromName(std::string const& name) {
    static std::map<std::string, Method> const methods = {
        {"ctm", Method::Ctm},
        {"glass", Method::Ctm},
        {"ctm_compact", Method::Compact},
        {"horizontal", Method::Horizontal},
        {"bookshelf", Method::Horizontal},
        {"vertical", Method::Vertical},
        {"horizontal+vertical", Method::HorizontalVertical},
        {"h+v", Method::HorizontalVertical},
        {"vertical+horizontal", Method::VerticalHorizontal},
        {"v+h", Method::VerticalHorizontal},
        {"top", Method::Top},
        {"sandstone", Method::Top},
        {"random", Method::Random},
        {"repeat", Method::Repeat},
        {"pattern", Method::Repeat},
        {"fixed", Method::Fixed},
        {"static", Method::Fixed},
        {"overlay", Method::Overlay},
        {"overlay_ctm", Method::OverlayCtm},
        {"overlay_random", Method::OverlayRandom},
        {"overlay_repeat", Method::OverlayRepeat},
        {"overlay_fixed", Method::OverlayFixed},
    };
    auto it = methods.find(name);
    return it == methods.end() ? std::nullopt : std::optional(it->second);
}

char const* methodName(Method m) {
    switch (m) {
    case Method::Ctm: return "ctm";
    case Method::Compact: return "ctm_compact";
    case Method::Horizontal: return "horizontal";
    case Method::Vertical: return "vertical";
    case Method::HorizontalVertical: return "horizontal+vertical";
    case Method::VerticalHorizontal: return "vertical+horizontal";
    case Method::Top: return "top";
    case Method::Random: return "random";
    case Method::Repeat: return "repeat";
    case Method::Fixed: return "fixed";
    case Method::Overlay: return "overlay";
    case Method::OverlayCtm: return "overlay_ctm";
    case Method::OverlayRandom: return "overlay_random";
    case Method::OverlayRepeat: return "overlay_repeat";
    case Method::OverlayFixed: return "overlay_fixed";
    }
    return "?";
}

std::optional<Rule> parse(std::string const& path, std::string const& text, std::string& error) {
    // Java .properties: key=value lines, # and ! comments, \ continues a line.
    std::map<std::string, std::string> kv;
    std::string                        pending;
    size_t                             start = 0;
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
        kv[trim(std::string_view(line).substr(0, eq))] = trim(std::string_view(line).substr(eq + 1));
    }

    Rule r;
    r.file                 = path;
    std::string const dir  = dirOf(path);
    std::string       base = fileOf(path);
    if (base.ends_with(".properties")) base.resize(base.size() - 11);

    auto get = [&](char const* key) -> std::string const* {
        auto it = kv.find(key);
        return it == kv.end() ? nullptr : &it->second;
    };

    auto const* m = get("method");
    if (!m) {
        error = kv.empty() ? "empty (all lines commented out?)" : "no method";
        return std::nullopt;
    }
    auto method = methodFromName(*m);
    if (!method) {
        error = "unknown method " + *m;
        return std::nullopt;
    }
    r.method = *method;

    if (auto const* v = get("width")) r.width = toInt(*v).value_or(0);
    if (auto const* v = get("height")) r.height = toInt(*v).value_or(0);

    if (auto const* v = get("tiles")) {
        for (auto const& w : words(*v)) {
            if (w == "<skip>") r.tiles.push_back({Tile::Kind::Skip, {}});
            else if (w == "<default>") r.tiles.push_back({Tile::Kind::Default, {}});
            else if (auto dash = w.find('-'); dash != std::string::npos && toInt(w.substr(0, dash)) && toInt(w.substr(dash + 1))) {
                int const a = *toInt(w.substr(0, dash)), b = *toInt(w.substr(dash + 1));
                for (int i = a; i <= b; ++i) r.tiles.push_back({Tile::Kind::Image, tilePath(std::to_string(i), dir)});
            } else r.tiles.push_back({Tile::Kind::Image, tilePath(w, dir)});
        }
    }
    if (r.tiles.empty()) {
        error = "no tiles";
        return std::nullopt;
    }
    if (r.tiles.size() < requiredTiles(r))
        r.warnings.push_back(std::format("{} needs {} tiles, has {}", methodName(r.method), requiredTiles(r), r.tiles.size()));

    if (auto const* v = get("matchBlocks"))
        for (auto const& w : words(*v))
            if (auto b = parseBlock(w)) r.matchBlocks.push_back(std::move(*b));
    if (auto const* v = get("matchTiles"))
        for (auto const& w : words(*v)) r.matchTiles.push_back(tileName(w));
    if (!get("matchBlocks") && !get("matchTiles")) {
        if (base.starts_with("block_")) {
            if (auto b = parseBlock(base.substr(6))) r.matchBlocks.push_back(std::move(*b));
        } else r.matchTiles.push_back(tileName(base));
    }
    if (r.matchBlocks.empty() && r.matchTiles.empty()) {
        error = "matches nothing";
        return std::nullopt;
    }

    r.connect = r.matchTiles.empty() ? Connect::Block : Connect::Tile;
    if (auto const* v = get("connect")) {
        if (*v == "block") r.connect = Connect::Block;
        else if (*v == "tile") r.connect = Connect::Tile;
        else if (*v == "state") r.connect = Connect::State;
    }
    if (auto const* v = get("faces")) {
        r.faces = 0;
        for (auto const& w : words(*v)) {
            if (w == "bottom") r.faces |= dirBit(Dir::Down);
            else if (w == "top") r.faces |= dirBit(Dir::Up);
            else if (w == "north") r.faces |= dirBit(Dir::North);
            else if (w == "south") r.faces |= dirBit(Dir::South);
            else if (w == "west") r.faces |= dirBit(Dir::West);
            else if (w == "east") r.faces |= dirBit(Dir::East);
            else if (w == "sides")
                r.faces |= dirBit(Dir::North) | dirBit(Dir::South) | dirBit(Dir::West) | dirBit(Dir::East);
            else if (w == "all") r.faces |= kAllDirs;
        }
        if (!r.faces) r.faces = kAllDirs;
    }
    if (auto const* v = get("weight")) r.weight = toInt(*v).value_or(0);
    if (auto const* v = get("innerSeams")) r.innerSeams = *v == "true";
    if (auto const* v = get("weights"))
        for (auto const& w : words(*v)) r.weights.push_back(std::max(0, toInt(w).value_or(1)));
    if (auto const* v = get("symmetry")) r.symmetry = *v == "all" ? Symmetry::All : *v == "opposite" ? Symmetry::Opposite : Symmetry::None;
    if (auto const* v = get("randomLoops")) r.randomLoops = std::clamp(toInt(*v).value_or(0), 0, 9);
    if (auto const* v = get("linked")) r.linked = *v == "true";
    if (auto const* v = get("connectBlocks"))
        for (auto const& w : words(*v))
            if (auto b = parseBlock(w)) r.connectBlocks.push_back(std::move(*b));
    if (auto const* v = get("connectTiles"))
        for (auto const& w : words(*v)) r.connectTiles.push_back(tileName(w));
    if (auto const* v = get("tintIndex")) r.tintIndex = toInt(*v).value_or(-1);
    if (auto const* v = get("tintBlock")) r.tintBlock = stripMinecraft(*v);
    if (auto const* v = get("heights")) r.heights = parseHeights(*v, r.warnings);
    else {
        auto const* lo = get("minHeight");
        auto const* hi = get("maxHeight");
        if (lo || hi) r.heights.emplace_back(lo ? toInt(*lo).value_or(-2048) : -2048, hi ? toInt(*hi).value_or(2048) : 2048);
    }
    if (auto const* v = get("biomes")) r.biomes = words(*v);
    for (auto const& [key, value] : kv)
        if (key.starts_with("ctm.")) {
            auto a = toInt(key.substr(4));
            auto b = toInt(value);
            if (a && b) r.compactMap.emplace_back(*a, *b);
        }
    return r;
}

std::array<int, 4> compactQuarters(uint8_t n) {
    // For each quarter: the horizontal neighbour beside it, the vertical one above or
    // below it, and the diagonal between them.
    auto quarter = [&](uint8_t side, uint8_t vertical, uint8_t diagonal) {
        bool const h = n & side, v = n & vertical;
        if (h && v) return (n & diagonal) ? 1 : 4;
        if (v) return 2; // connected up/down only: border at the side
        if (h) return 3; // connected sideways only: border at top/bottom
        return 0;
    };
    return {quarter(Left, Up, UpLeft), quarter(Right, Up, UpRight), quarter(Left, Down, DownLeft),
            quarter(Right, Down, DownRight)};
}

std::vector<int> overlayTiles(uint8_t n) {
    bool const       l = n & Left, r = n & Right, u = n & Up, d = n & Down;
    std::vector<int> out;
    int const        edges = int(l) + int(r) + int(u) + int(d);
    if (edges == 4) out.push_back(8);
    else if (edges == 3) out.push_back(!d ? 13 : !u ? 5 : !r ? 6 : 12);
    else if (edges == 2) {
        if (u && l) out.push_back(11);
        else if (u && r) out.push_back(10);
        else if (d && l) out.push_back(4);
        else if (d && r) out.push_back(3);
        else if (u && d) out.push_back(15), out.push_back(1);
        else out.push_back(9), out.push_back(7);
    } else if (edges == 1) out.push_back(u ? 15 : d ? 1 : l ? 9 : 7);
    if (!u && !l && (n & UpLeft)) out.push_back(16);
    if (!u && !r && (n & UpRight)) out.push_back(14);
    if (!d && !l && (n & DownLeft)) out.push_back(2);
    if (!d && !r && (n & DownRight)) out.push_back(0);
    return out;
}

int randomTile(Rule const& rule, int x, int y, int z, Dir face) {
    int f = int(face);
    if (rule.symmetry == Symmetry::Opposite) f /= 2;
    else if (rule.symmetry == Symmetry::All) f = 0;
    uint32_t h = mix(uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u ^ uint32_t(f) * 2654435761u);
    for (int i = 0; i < rule.randomLoops; ++i) h = mix(h);
    int const count = int(rule.tiles.size());
    if (count <= 1) return 0;
    if (rule.weights.size() == rule.tiles.size()) {
        long total = 0;
        for (int w : rule.weights) total += w;
        if (total > 0) {
            long pick = long(h % uint32_t(total));
            for (int i = 0; i < count; ++i) {
                pick -= rule.weights[size_t(i)];
                if (pick < 0) return i;
            }
        }
    }
    return int(h % uint32_t(count));
}

int repeatTile(Rule const& rule, int cu, int cv) {
    int const w = std::max(1, rule.width), h = std::max(1, rule.height);
    int const x = ((cu % w) + w) % w, y = ((cv % h) + h) % h;
    return y * w + x;
}

} // namespace tessera::ctm
