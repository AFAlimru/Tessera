// C++ form of tools/convert_java_pack.py; see JavaPack.h. Image operations follow
// Pillow's (QUAD/AFFINE transforms with nearest sampling, alpha_composite) so the
// output matches the Python prototype.
#include "core/java/JavaPack.h"
#include "core/item/ItemDef.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace tessera::java {
namespace {

using Json = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string stripNs(std::string_view name) {
    auto const colon = name.find(':');
    return std::string(colon == std::string_view::npos ? name : name.substr(colon + 1));
}

std::optional<Json> parseJson(std::optional<std::string> const& bytes) {
    if (!bytes) return std::nullopt;
    std::string_view text{*bytes};
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    auto j = Json::parse(text, nullptr, false, true);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

// Python's round(): halves go to the even neighbour (the default rounding mode).
long pyRound(double v) { return long(std::nearbyint(v)); }

// The text a "when" value is compared as ("powered": false -> "false").
std::string whenText(Json const& v) {
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    return v.dump();
}

std::vector<std::string> splitBar(std::string const& s) {
    std::vector<std::string> out;
    size_t                   start = 0;
    while (true) {
        auto const bar = s.find('|', start);
        out.push_back(s.substr(start, bar - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return out;
}

using StateMap = std::map<std::string, std::string>;

StateMap parseProps(std::string const& text) {
    StateMap out;
    size_t   start = 0;
    while (start < text.size()) {
        auto const comma = text.find(',', start);
        auto const kv    = text.substr(start, comma - start);
        if (auto const eq = kv.find('='); eq != std::string::npos) out[kv.substr(0, eq)] = kv.substr(eq + 1);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// "true"/"false" -> bool, integers -> int, anything else stays text.
Json typed(std::string const& v) {
    if (v == "true") return true;
    if (v == "false") return false;
    bool digits = !v.empty();
    for (size_t i = 0; i < v.size(); ++i)
        if (!(std::isdigit(uint8_t(v[i])) || (i == 0 && v[i] == '-' && v.size() > 1))) digits = false;
    if (digits) return std::stoll(v);
    return v;
}

// ---------------------------------------------------------------------------
// Pillow-equivalent image operations
// ---------------------------------------------------------------------------
Image blank(int w, int h) {
    Image i;
    i.width  = w;
    i.height = h;
    i.pixels.assign(size_t(w) * size_t(h), 0u);
    return i;
}

int coord(double v) { return v < 0.0 ? -1 : int(v); }

uint32_t sample(Image const& src, double x, double y) {
    int const ix = coord(x), iy = coord(y);
    if (ix < 0 || ix >= src.width || iy < 0 || iy >= src.height) return 0;
    return src.at(ix, iy);
}

// Image.transform((w, h), QUAD, (nw, sw, se, ne), NEAREST)
Image quadTransform(Image const& src, int w, int h, std::array<double, 8> const& q) {
    double const x0 = q[0], y0 = q[1];
    double const As = 1.0 / w, At = 1.0 / h;
    double const a[8] = {
        x0, (q[6] - x0) * As, (q[2] - x0) * At, (q[4] - q[2] - q[6] + x0) * As * At,
        y0, (q[7] - y0) * As, (q[3] - y0) * At, (q[5] - q[3] - q[7] + y0) * As * At,
    };
    Image out = blank(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double const xi = x + 0.5, yi = y + 0.5;
            out.pixels[size_t(y) * size_t(w) + size_t(x)] =
                sample(src, a[0] + a[1] * xi + a[2] * yi + a[3] * xi * yi, a[4] + a[5] * xi + a[6] * yi + a[7] * xi * yi);
        }
    return out;
}

// Image.transform((size, size), AFFINE, (a, b, c, d, e, f), NEAREST)
Image affineTransform(Image const& src, int size, std::array<double, 6> const& a) {
    Image out = blank(size, size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            double const xi = x + 0.5, yi = y + 0.5;
            out.pixels[size_t(y) * size_t(size) + size_t(x)] =
                sample(src, a[0] * xi + a[1] * yi + a[2], a[3] * xi + a[4] * yi + a[5]);
        }
    return out;
}

// Image.alpha_composite (Pillow's integer arithmetic).
uint32_t over(uint32_t dst, uint32_t src) {
    uint32_t const sa = src >> 24;
    if (sa == 0) return dst;
    constexpr uint32_t P    = 7;
    auto               div  = [](uint32_t v) { return ((v >> 8) + v) >> 8; };
    uint32_t const     da   = dst >> 24;
    uint32_t const     blnd = da * (255 - sa);
    uint32_t const     oa   = sa * 255 + blnd;
    uint32_t const     c1   = sa * 255 * 255 * (1u << P) / oa;
    uint32_t const     c2   = 255 * (1u << P) - c1;
    uint32_t           out  = div(oa + 0x80) << 24;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t const s = (src >> shift) & 0xFF, d = (dst >> shift) & 0xFF;
        uint32_t const v = div(s * c1 + d * c2 + (0x80u << P)) >> P;
        out |= (v & 0xFF) << shift;
    }
    return out;
}

void composite(Image& canvas, Image const& part, int dx, int dy, int sx0, int sy0, int sx1, int sy1) {
    for (int y = sy0; y < sy1; ++y)
        for (int x = sx0; x < sx1; ++x) {
            auto& d = canvas.pixels[size_t(dy + y - sy0) * size_t(canvas.width) + size_t(dx + x - sx0)];
            d       = over(d, part.at(x, y));
        }
}

void tint(Image& img, std::array<int, 3> const& rgb) {
    for (auto& p : img.pixels) {
        if (!(p >> 24)) continue;
        uint32_t const r = (p & 0xFF) * uint32_t(rgb[0]) / 255, g = ((p >> 8) & 0xFF) * uint32_t(rgb[1]) / 255,
                       b = ((p >> 16) & 0xFF) * uint32_t(rgb[2]) / 255;
        p                = (p & 0xFF000000u) | r | (g << 8) | (b << 16);
    }
}

// labPBR (Java shader packs) to Bedrock texture-set maps. Pixels are RGBA, R in the low byte.
// Normal: labPBR keeps the normal's x, y in R, G (z is implied); Bedrock wants all three,
// with the green channel the DirectX way round (y down).
} // namespace
uint32_t labToBedrockNormal(uint32_t n) {
    double const x = ((n & 0xFF) / 255.0) * 2.0 - 1.0, y = (((n >> 8) & 0xFF) / 255.0) * 2.0 - 1.0;
    double const z = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
    auto ch = [](double v) { return uint32_t(std::clamp((v * 0.5 + 0.5) * 255.0 + 0.5, 0.0, 255.0)); };
    return ch(x) | (ch(-y) << 8) | (ch(z) << 16) | 0xFF000000u;
}
// Specular: labPBR R perceptual smoothness, G reflectance (230+ = metal), B porosity (0..64) or
// subsurface (65..255), A emission (255 = none). Bedrock MERS: metalness, emissive, roughness,
// subsurface.
uint32_t labToBedrockMers(uint32_t s) {
    uint32_t const sm = s & 0xFF, f0 = (s >> 8) & 0xFF, sss = (s >> 16) & 0xFF, em = s >> 24;
    uint32_t const metal = f0 >= 230 ? 255 : 0;
    uint32_t const emissive = em == 255 ? 0 : em * 255 / 254;
    uint32_t const rough = 255 - sm;
    uint32_t const sub = sss >= 65 ? (sss - 65) * 255 / 190 : 0;
    return metal | (emissive << 8) | (rough << 16) | (sub << 24);
}

namespace {
uint64_t hashImage(Image const& img) {
    uint64_t h    = 1469598103934665603ull;
    auto     feed = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    feed(uint32_t(img.width));
    feed(uint32_t(img.height));
    for (auto p : img.pixels) feed(p);
    return h;
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------
struct P3 {
    double x = 0, y = 0, z = 0;
    double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};

struct Face {
    std::string texture;
    std::string cullface; // "" or a direction
    bool        hasUv = false;
    double      uv[4] = {0, 0, 0, 0};
    int         rotation  = 0;
    int         tintindex = -1;
};

struct Turn {
    double angle   = 0;
    char   axis    = 'y';
    bool   rescale = false;
    P3     origin{8, 8, 8};
};

struct Element {
    P3                                        from, to;
    bool                                      shade = true;
    std::optional<Turn>                       turn;
    std::vector<std::pair<std::string, Face>> faces; // in file order
};

struct TextureRef {
    std::string value;
    bool        plain = true; // a string in the file (not a {"sprite": ...} object)
};

struct Model {
    std::map<std::string, TextureRef>               textures;
    std::shared_ptr<std::vector<Element> const>     elements; // null: none
    // display.gui: rotation, translation, scale (Java's inventory view); and gui_light.
    std::optional<std::array<double, 9>>            gui;
    std::string                                     guiLight; // "front", "side" or unset
    // Every display context (rotation, translation, scale), inherited key by key.
    std::map<std::string, std::array<double, 9>>    display;
};

void readDisplay(Model& m, Json const& j) {
    if (auto l = j.find("gui_light"); l != j.end() && l->is_string()) m.guiLight = l->get<std::string>();
    auto d = j.find("display");
    if (d == j.end() || !d->is_object()) return;
    for (auto const& [ctx, g] : d->items()) {
        if (!g.is_object()) continue;
        std::array<double, 9> t{0, 0, 0, 0, 0, 0, 1, 1, 1};
        auto read = [&](char const* key, size_t at) {
            auto v = g.find(key);
            if (v == g.end() || !v->is_array() || v->size() < 3) return;
            for (size_t i = 0; i < 3; ++i)
                if ((*v)[i].is_number()) t[at + i] = (*v)[i].get<double>();
        };
        read("rotation", 0), read("translation", 3), read("scale", 6);
        m.display[ctx] = t;
        if (ctx == "gui") m.gui = t;
    }
}

std::string spriteOf(Json const& v) {
    if (v.is_object()) return v.value("sprite", std::string{});
    if (v.is_string()) return v.get<std::string>();
    return {};
}

P3 vec(Json const& v) {
    if (!v.is_array() || v.size() < 3) return {};
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

std::vector<Element> parseElements(Json const& list) {
    std::vector<Element> out;
    for (auto const& e : list) {
        Element el;
        el.from = vec(e.value("from", Json::array()));
        el.to   = vec(e.value("to", Json::array()));
        el.shade = e.value("shade", true);
        if (auto r = e.find("rotation"); r != e.end() && r->is_object()) {
            Turn t;
            t.angle = r->value("angle", 0.0);
            auto ax = r->value("axis", std::string("y"));
            t.axis  = ax.empty() ? 'y' : ax[0];
            t.rescale = r->value("rescale", false);
            if (auto o = r->find("origin"); o != r->end()) t.origin = vec(*o);
            el.turn = t;
        }
        if (auto f = e.find("faces"); f != e.end() && f->is_object())
            for (auto const& [dir, fc] : f->items()) {
                Face face;
                face.texture = spriteOf(fc.value("texture", Json("")));
                if (auto uv = fc.find("uv"); uv != fc.end() && uv->is_array() && uv->size() >= 4) {
                    face.hasUv = true;
                    for (int i = 0; i < 4; ++i) face.uv[i] = (*uv)[size_t(i)].get<double>();
                }
                face.rotation  = fc.value("rotation", 0);
                face.cullface  = fc.value("cullface", std::string{});
                face.tintindex = fc.value("tintindex", -1);
                el.faces.emplace_back(dir, face);
            }
        out.push_back(std::move(el));
    }
    return out;
}

struct MissingModel {
    std::string name;
};

// ---------------------------------------------------------------------------
// Geometry (see tools/convert_xekr.py for the derivation)
// ---------------------------------------------------------------------------
constexpr char const* kFaces[6] = {"down", "up", "north", "south", "west", "east"};

std::optional<std::array<int, 3>> dirOf(std::string const& face) {
    if (face == "down") return std::array{0, -1, 0};
    if (face == "up") return std::array{0, 1, 0};
    if (face == "north") return std::array{0, 0, -1};
    if (face == "south") return std::array{0, 0, 1};
    if (face == "west") return std::array{-1, 0, 0};
    if (face == "east") return std::array{1, 0, 0};
    return std::nullopt;
}

std::string faceOfDir(std::array<int, 3> const& d) {
    for (auto const* f : kFaces)
        if (dirOf(f) == d) return f;
    return {};
}

int axisOf(std::string const& face) {
    if (face == "up" || face == "down") return 1;
    if (face == "north" || face == "south") return 2;
    return 0;
}

// (a, b, depth) of a point seen from `face`.
P3 faceCoords(std::string const& face, P3 const& p) {
    if (face == "up") return {p.x, p.z, p.y};
    if (face == "down") return {p.x, 16 - p.z, -p.y};
    if (face == "north") return {16 - p.x, 16 - p.y, -p.z};
    if (face == "south") return {p.x, 16 - p.y, p.z};
    if (face == "west") return {p.z, 16 - p.y, -p.x};
    return {16 - p.z, 16 - p.y, p.x}; // east
}

std::array<P3, 4> planePoints(std::string const& face, P3 const& f, P3 const& t) {
    if (face == "up" || face == "down") {
        double const y = face == "up" ? t.y : f.y;
        return {P3{f.x, y, f.z}, P3{f.x, y, t.z}, P3{t.x, y, f.z}, P3{t.x, y, t.z}};
    }
    if (face == "north" || face == "south") {
        double const z = face == "north" ? f.z : t.z;
        return {P3{f.x, f.y, z}, P3{f.x, t.y, z}, P3{t.x, f.y, z}, P3{t.x, t.y, z}};
    }
    double const x = face == "west" ? f.x : t.x;
    return {P3{x, f.y, f.z}, P3{x, f.y, t.z}, P3{x, t.y, f.z}, P3{x, t.y, t.z}};
}

struct Ordered {
    std::array<P3, 4>     corners;
    std::array<double, 4> rect; // a_lo, b_lo, a_hi, b_hi
};

std::optional<Ordered> orderedCorners(std::string const& face, std::array<P3, 4> const& pts) {
    std::array<std::pair<P3, P3>, 4> ab; // (face coords, point)
    for (size_t i = 0; i < 4; ++i) ab[i] = {faceCoords(face, pts[i]), pts[i]};
    double alo = INFINITY, ahi = -INFINITY, blo = INFINITY, bhi = -INFINITY;
    for (auto const& [c, p] : ab) {
        alo = std::min(alo, c.x), ahi = std::max(ahi, c.x);
        blo = std::min(blo, c.y), bhi = std::max(bhi, c.y);
    }
    if (ahi - alo < 1e-6 || bhi - blo < 1e-6) return std::nullopt;
    auto pick = [&](double a, double b) {
        for (auto const& [c, p] : ab)
            if (std::abs(c.x - a) < 1e-6 && std::abs(c.y - b) < 1e-6) return p;
        return ab[0].second;
    };
    return Ordered{{pick(alo, blo), pick(ahi, blo), pick(ahi, bhi), pick(alo, bhi)}, {alo, blo, ahi, bhi}};
}

// Java builds an element's faces from "from" and "to" as given: when exactly one of a face's
// two in-plane axes runs backwards (from > to), the face winds the other way and shows its
// other side (packs use this for inside-out boxes, e.g. a wax layer seen only past the edges).
bool insideOut(std::string const& face, P3 const& f, P3 const& t) {
    bool const rx = f.x > t.x, ry = f.y > t.y, rz = f.z > t.z;
    if (face == "north" || face == "south") return rx != ry;
    if (face == "east" || face == "west") return rz != ry;
    return rx != rz; // up, down
}

P3 rotPoint(P3 p, int rx, int ry) {
    for (int i = 0; i < ((rx / 90) % 4 + 4) % 4; ++i) p = {p.x, p.z, 16 - p.y};
    for (int i = 0; i < ((ry / 90) % 4 + 4) % 4; ++i) p = {16 - p.z, p.y, p.x};
    return p;
}

std::array<int, 3> rotDir(std::array<int, 3> d, int rx, int ry) {
    for (int i = 0; i < ((rx / 90) % 4 + 4) % 4; ++i) d = {d[0], d[2], -d[1]};
    for (int i = 0; i < ((ry / 90) % 4 + 4) % 4; ++i) d = {-d[2], d[1], d[0]};
    return d;
}

P3 turnPoint(P3 const& p, Turn const& t) {
    double x = p.x - t.origin.x, y = p.y - t.origin.y, z = p.z - t.origin.z;
    double const r = t.angle * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    if (t.axis == 'y') std::tie(x, z) = std::pair{x * c + z * s, -x * s + z * c};
    else if (t.axis == 'x') std::tie(y, z) = std::pair{y * c - z * s, y * s + z * c};
    else std::tie(x, y) = std::pair{x * c - y * s, x * s + y * c};
    return {x + t.origin.x, y + t.origin.y, z + t.origin.z};
}

bool alongAxis(std::string const& fdir, char axis) {
    if (axis == 'x') return fdir == "east" || fdir == "west";
    if (axis == 'y') return fdir == "up" || fdir == "down";
    return fdir == "north" || fdir == "south";
}

// Output pixel -> input pixel, from three point pairs (PIL's AFFINE coefficients).
std::array<double, 6> affineFrom(std::array<std::pair<double, double>, 3> const& dst,
                                 std::array<std::pair<double, double>, 3> const& src) {
    auto const [x0, y0] = dst[0];
    auto const [x1, y1] = dst[1];
    auto const [x2, y2] = dst[2];
    double const          det = x0 * (y1 - y2) - y0 * (x1 - x2) + (x1 * y2 - x2 * y1);
    std::array<double, 6> out{};
    for (int k = 0; k < 2; ++k) {
        double const v0 = k ? src[0].second : src[0].first, v1 = k ? src[1].second : src[1].first,
                     v2 = k ? src[2].second : src[2].first;
        out[size_t(k * 3)]     = (v0 * (y1 - y2) - y0 * (v1 - v2) + (v1 * y2 - v2 * y1)) / det;
        out[size_t(k * 3 + 1)] = (x0 * (v1 - v2) - v0 * (x1 - x2) + (x1 * v2 - x2 * v1)) / det;
        out[size_t(k * 3 + 2)] =
            (x0 * (y1 * v2 - y2 * v1) - y0 * (x1 * v2 - x2 * v1) + v0 * (x1 * y2 - x2 * y1)) / det;
    }
    return out;
}

struct Part {
    std::string model;
    int         rx = 0, ry = 0;
    bool        operator<(Part const& o) const { return std::tie(model, rx, ry) < std::tie(o.model, o.rx, o.ry); }
};

using Tints = std::map<int, std::array<int, 3>>;

struct Layer {
    double                           depth = 0;
    int                              order = 0;
    std::array<P3, 4>                world;
    std::array<std::pair<double, double>, 4> uvs;
    Image const*                     tex = nullptr;
    std::string                      texName; // Java name, no namespace
    int                              tintIndex = -1;
    std::optional<std::array<int, 3>> tint;
    double                           plane = 0;
    std::optional<std::array<P3, 4>> flat; // a turned face drawn straight, before being turned into place
};

std::array<int, 3> dustRgb(int power) {
    double const f = power / 15.0;
    double const r = f * 0.6 + (f > 0 ? 0.4 : 0.3);
    double const g = std::clamp(f * f * 0.7 - 0.5, 0.0, 1.0);
    double const b = std::clamp(f * f * 0.6 - 0.7, 0.0, 1.0);
    return {int(r * 255), int(g * 255), int(b * 255)};
}

// ---------------------------------------------------------------------------
// Baker
// ---------------------------------------------------------------------------
std::set<std::string> const kSkipBlocks = {"fire", "soul_fire", "piston_head", "moving_piston", "water", "lava"};

std::map<std::string, std::map<std::string, std::string>> const kBridge = {
    {"note_block", {{"note", "tessera:note"}, {"instrument", "tessera:instrument"}, {"powered", "tessera:powered"}}},
    {"piston", {{"extended", "tessera:extended"}}},
    {"sticky_piston", {{"extended", "tessera:extended"}}},
    {"jukebox", {{"has_record", "tessera:has_record"}}},
    {"target", {{"power", "tessera:signal"}}},
    {"sculk_sensor", {{"power", "tessera:signal"}}},
    {"calibrated_sculk_sensor", {{"power", "tessera:signal"}}},
    // Connections Bedrock works out while drawing (Tessera does the same, the way Java does).
    {"redstone_wire",
     {{"north", "tessera:redstone_north"}, {"east", "tessera:redstone_east"}, {"south", "tessera:redstone_south"}, {"west", "tessera:redstone_west"}}},
    {"tripwire",
     {{"north", "tessera:tripwire_north"}, {"east", "tessera:tripwire_east"}, {"south", "tessera:tripwire_south"}, {"west", "tessera:tripwire_west"}}},
};

std::map<std::string, std::map<std::string, std::string>> const kAgree = {
    {"lever", {{"powered", "open_bit"}}},
};

class Baker {
public:
    explicit Baker(BakeOptions const& o) : mOpt(o) {}

    BakeResult run();

    static std::optional<std::string> skipReason(std::string const& block) {
        if (kSkipBlocks.contains(block)) return "drawn by the game itself (fire, fluids, moving pistons)";
        if (connectionShaped(block))
            return "shape comes from neighbours Bedrock keeps no state for (fences, walls, bars, chorus plants): "
                   "Bedrock's own mesh is kept, connected textures still apply";
        return std::nullopt;
    }

    bool packHas(std::string const& path) {
        if (!mPackFiles) {
            mPackFiles.emplace();
            for (char const* dir : {"assets/minecraft/models/", "assets/minecraft/textures/"})
                for (auto const& p : mOpt.pack->list(dir)) mPackFiles->insert(p);
        }
        return mPackFiles->contains(path);
    }

    // Is the model the pack's own, or does it draw a texture of the pack's?
    ItemIcons::Info itemInfo(std::string const& ref) {
        ItemIcons::Info info;
        std::shared_ptr<Model> m;
        try {
            m = loadModel(ref);
        } catch (MissingModel const&) {
            return info;
        }
        std::string const name = stripNs(ref);
        info.found    = true;
        info.flat     = !m->elements || m->elements->empty();
        info.ownModel = packHas("assets/minecraft/models/" + name + ".json");
        info.touched  = info.ownModel;
        auto has = [&](std::string const& tex) {
            auto r = resolveRef(*m, tex);
            return r && !r->empty() && packHas("assets/minecraft/textures/" + stripNs(*r) + ".png");
        };
        if (info.flat) {
            for (int i = 0; !info.touched; ++i) {
                auto it = m->textures.find("layer" + std::to_string(i));
                if (it == m->textures.end()) break;
                info.touched = has(it->second.value);
            }
        } else {
            for (auto const& el : *m->elements)
                for (auto const& [dir, fc] : el.faces)
                    if (!info.touched) info.touched = has(fc.texture);
        }
        return info;
    }

    // A model with elements as Java's inventory shows it: turned by its display.gui
    // transform, seen straight on, with back faces culled and a depth buffer.
    std::optional<Image> guiIcon(Model const& m, std::vector<uint32_t> const& tints) {
        constexpr int kOut = 64, kSuper = 2, kW = kOut * kSuper;
        constexpr double kPi = 3.14159265358979323846;
        // Blocks' own view (block/block), when the model chain has none and looks like a block.
        std::array<double, 9> t = m.gui ? *m.gui : std::array<double, 9>{30, 225, 0, 0, 0, 0, 0.625, 0.625, 0.625};
        bool const            side = m.guiLight != "front";
        double const rx = t[0] * kPi / 180, ry = t[1] * kPi / 180, rz = t[2] * kPi / 180;
        using V = std::array<double, 3>;
        auto turn = [&](V v) { // Rx * Ry * Rz
            v = {v[0] * std::cos(rz) - v[1] * std::sin(rz), v[0] * std::sin(rz) + v[1] * std::cos(rz), v[2]};
            v = {v[0] * std::cos(ry) + v[2] * std::sin(ry), v[1], -v[0] * std::sin(ry) + v[2] * std::cos(ry)};
            v = {v[0], v[1] * std::cos(rx) - v[2] * std::sin(rx), v[1] * std::sin(rx) + v[2] * std::cos(rx)};
            return v;
        };
        auto place = [&](P3 const& p) { // model pixels -> view (x right, y up, z to the viewer), slot = [-0.5, 0.5]
            V v{(p.x / 16.0 - 0.5) * t[6], (p.y / 16.0 - 0.5) * t[7], (p.z / 16.0 - 0.5) * t[8]};
            v = turn(v);
            return V{v[0] + t[3] / 16.0, v[1] + t[4] / 16.0, v[2] + t[5] / 16.0};
        };
        std::vector<double>   depth(size_t(kW) * kW, -1e30);
        std::vector<uint32_t> color(size_t(kW) * kW, 0u);
        struct Tri {
            V        a, b, c;                      // screen x, y and depth
            double   ua, va, ub, vb, uc, vc;       // texture pixels
            Image const* tex;
            double   shade;
            uint32_t tint;
        };
        std::vector<Tri> tris;
        for (auto const& el : *m.elements) {
            bool const turned = el.turn && el.turn->angle != 0;
            for (auto const& [fdir, fc] : el.faces) {
                auto const d = dirOf(fdir);
                if (!d) continue;
                auto oc = orderedCorners(fdir, planePoints(fdir, el.from, el.to));
                if (!oc) continue;
                auto ref = resolveRef(m, fc.texture);
                if (!ref || ref->empty()) continue;
                Image const* tex = javaTex(*ref);
                if (!tex) continue;
                double const u0 = fc.hasUv ? fc.uv[0] : oc->rect[0], v0 = fc.hasUv ? fc.uv[1] : oc->rect[1];
                double const u1 = fc.hasUv ? fc.uv[2] : oc->rect[2], v1 = fc.hasUv ? fc.uv[3] : oc->rect[3];
                std::array<std::pair<double, double>, 4> uvs{{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
                int const r = ((fc.rotation / 90) % 4 + 4) % 4;
                if (r) std::rotate(uvs.begin(), uvs.end() - r, uvs.end());
                bool const flipped = insideOut(fdir, el.from, el.to);
                // Which way round the corners go, seen from outside the face.
                P3 const& c0 = oc->corners[0];
                P3 const& c1 = oc->corners[1];
                P3 const& c3 = oc->corners[3];
                V const   e1{c1.x - c0.x, c1.y - c0.y, c1.z - c0.z}, e2{c3.x - c0.x, c3.y - c0.y, c3.z - c0.z};
                V const   raw{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                double const way = (raw[0] * (*d)[0] + raw[1] * (*d)[1] + raw[2] * (*d)[2] >= 0 ? 1.0 : -1.0) * (flipped ? -1.0 : 1.0);
                std::array<V, 4> s;
                for (size_t i = 0; i < 4; ++i) {
                    P3 c = oc->corners[i];
                    if (turned) {
                        c = turnPoint(c, *el.turn);
                        if (el.turn->rescale) {
                            double const k = 1.0 / std::cos(el.turn->angle * kPi / 180.0);
                            auto const&  o = el.turn->origin;
                            if (el.turn->axis != 'x') c.x = o.x + (c.x - o.x) * k;
                            if (el.turn->axis != 'y') c.y = o.y + (c.y - o.y) * k;
                            if (el.turn->axis != 'z') c.z = o.z + (c.z - o.z) * k;
                        }
                    }
                    s[i] = place(c);
                }
                V const a1{s[1][0] - s[0][0], s[1][1] - s[0][1], s[1][2] - s[0][2]}, a2{s[3][0] - s[0][0], s[3][1] - s[0][1], s[3][2] - s[0][2]};
                V       n{a1[1] * a2[2] - a1[2] * a2[1], a1[2] * a2[0] - a1[0] * a2[2], a1[0] * a2[1] - a1[1] * a2[0]};
                double const len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len < 1e-12) continue;
                for (auto& c : n) c *= way / len;
                if (n[2] <= 1e-6) continue; // faces away from the viewer
                double const shade = !el.shade ? 1.0 : side ? std::clamp(0.78 + 0.26 * n[1] - 0.14 * n[0], 0.5, 1.0) : 1.0;
                uint32_t const tint = fc.tintindex >= 0 && size_t(fc.tintindex) < tints.size() ? tints[size_t(fc.tintindex)] : 0xFFFFFFu;
                auto px = [&](size_t i) { return V{(s[i][0] + 0.5) * kW, (0.5 - s[i][1]) * kW, s[i][2]}; };
                double const sw = tex->width / 16.0, sh = tex->height / 16.0;
                for (auto const& idx : {std::array<size_t, 3>{0, 1, 2}, std::array<size_t, 3>{0, 2, 3}})
                    tris.push_back(Tri{px(idx[0]), px(idx[1]), px(idx[2]), uvs[idx[0]].first * sw, uvs[idx[0]].second * sh,
                                       uvs[idx[1]].first * sw, uvs[idx[1]].second * sh, uvs[idx[2]].first * sw,
                                       uvs[idx[2]].second * sh, tex, shade, tint});
            }
        }
        if (tris.empty()) return std::nullopt;
        // Solid pixels first (depth tested and written), then see-through ones over them.
        for (int pass = 0; pass < 2; ++pass)
            for (auto const& tr : tris) {
                double const area = (tr.b[0] - tr.a[0]) * (tr.c[1] - tr.a[1]) - (tr.b[1] - tr.a[1]) * (tr.c[0] - tr.a[0]);
                if (std::abs(area) < 1e-9) continue;
                int const x0 = std::max(0, int(std::floor(std::min({tr.a[0], tr.b[0], tr.c[0]}))));
                int const x1 = std::min(kW - 1, int(std::ceil(std::max({tr.a[0], tr.b[0], tr.c[0]}))));
                int const y0 = std::max(0, int(std::floor(std::min({tr.a[1], tr.b[1], tr.c[1]}))));
                int const y1 = std::min(kW - 1, int(std::ceil(std::max({tr.a[1], tr.b[1], tr.c[1]}))));
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        double const fx = x + 0.5, fy = y + 0.5;
                        double const wa = ((tr.b[0] - fx) * (tr.c[1] - fy) - (tr.b[1] - fy) * (tr.c[0] - fx)) / area;
                        double const wb = ((tr.c[0] - fx) * (tr.a[1] - fy) - (tr.c[1] - fy) * (tr.a[0] - fx)) / area;
                        double const wc = 1.0 - wa - wb;
                        if (wa < -1e-9 || wb < -1e-9 || wc < -1e-9) continue;
                        double const z = wa * tr.a[2] + wb * tr.b[2] + wc * tr.c[2];
                        size_t const at = size_t(y) * kW + size_t(x);
                        if (z < depth[at] - 1e-9) continue;
                        double const u = wa * tr.ua + wb * tr.ub + wc * tr.uc, v = wa * tr.va + wb * tr.vb + wc * tr.vc;
                        int const tx = std::clamp(int(std::floor(u)), 0, tr.tex->width - 1);
                        int const ty = std::clamp(int(std::floor(v)), 0, tr.tex->height - 1);
                        uint32_t const texel = tr.tex->at(tx, ty);
                        uint32_t const alpha = texel >> 24;
                        if ((pass == 0) != (alpha == 255) || alpha == 0) continue;
                        auto ch = [&](int sh, int tsh) {
                            double const c = ((texel >> sh) & 0xFF) * tr.shade * (((tr.tint >> tsh) & 0xFF) / 255.0);
                            return uint32_t(std::clamp(c + 0.5, 0.0, 255.0)) << sh;
                        };
                        uint32_t const lit = ch(0, 16) | ch(8, 8) | ch(16, 0) | (alpha << 24); // image is RGBA (R low), tints are 0xRRGGBB
                        if (pass == 0) depth[at] = z, color[at] = lit;
                        else color[at] = over(color[at], lit);
                    }
            }
        Image out = blank(kOut, kOut);
        bool  any = false;
        for (int y = 0; y < kOut; ++y)
            for (int x = 0; x < kOut; ++x) {
                double r = 0, g = 0, b = 0, a = 0;
                for (int sy = 0; sy < kSuper; ++sy)
                    for (int sx = 0; sx < kSuper; ++sx) {
                        uint32_t const c  = color[size_t(y * kSuper + sy) * kW + size_t(x * kSuper + sx)];
                        double const   ca = (c >> 24) / 255.0;
                        r += (c & 0xFF) * ca, g += ((c >> 8) & 0xFF) * ca, b += ((c >> 16) & 0xFF) * ca, a += ca;
                    }
                if (a <= 0) continue;
                any = true;
                auto q = [&](double v) { return uint32_t(std::clamp(v / a + 0.5, 0.0, 255.0)); };
                uint32_t const alpha = uint32_t(std::clamp(a / (kSuper * kSuper) * 255.0 + 0.5, 0.0, 255.0));
                out.pixels[size_t(y) * kOut + size_t(x)] = q(r) | (q(g) << 8) | (q(b) << 16) | (alpha << 24);
            }
        return any ? std::optional(out) : std::nullopt;
    }

    // A model with elements cut into one quad per texel, as Bedrock builds held items from their
    // icon (HandMesh).
    std::optional<HandMesh> handMesh(std::string const& ref, std::vector<uint32_t> const& tints) {
        auto m = iconModel(ref);
        if (!m || !m->elements || m->elements->empty()) return std::nullopt;
        constexpr double kPi = 3.14159265358979323846;
        using V = std::array<double, 3>;
        HandMesh mesh;
        // Java's displays: v -> R * S * v + t / 16 on block units centred on the model (R = Rx * Ry * Rz,
        // as Java's rotationXYZ).
        using M4 = std::array<double, 16>; // row major
        auto mm = [](M4 const& a, M4 const& b) {
            M4 r{};
            for (size_t i = 0; i < 4; ++i)
                for (size_t k = 0; k < 4; ++k)
                    for (size_t j = 0; j < 4; ++j) r[i * 4 + k] += a[i * 4 + j] * b[j * 4 + k];
            return r;
        };
        auto translate = [](double x, double y, double z) { return M4{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1}; };
        auto rot = [&](double xd, double yd, double zd) {
            double const x = xd * kPi / 180, y = yd * kPi / 180, z = zd * kPi / 180;
            M4 const rx{1, 0, 0, 0, 0, std::cos(x), -std::sin(x), 0, 0, std::sin(x), std::cos(x), 0, 0, 0, 0, 1};
            M4 const ry{std::cos(y), 0, std::sin(y), 0, 0, 1, 0, 0, -std::sin(y), 0, std::cos(y), 0, 0, 0, 0, 1};
            M4 const rz{std::cos(z), -std::sin(z), 0, 0, std::sin(z), std::cos(z), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            return mm(rx, mm(ry, rz));
        };
        auto display = [&](std::array<double, 9> const& d) {
            return mm(translate(d[3] / 16, d[4] / 16, d[5] / 16), mm(rot(d[0], d[1], d[2]), M4{d[6], 0, 0, 0, 0, d[7], 0, 0, 0, 0, d[8], 0, 0, 0, 0, 1}));
        };
        auto inverse = [&](std::array<double, 9> const& d) { // S^-1 * R^T * T(-t)
            M4 r = rot(d[0], d[1], d[2]);
            M4 rt{};
            for (size_t i = 0; i < 4; ++i)
                for (size_t k = 0; k < 4; ++k) rt[i * 4 + k] = r[k * 4 + i];
            auto inv = [](double v) { return std::abs(v) < 1e-6 ? 0.0 : 1.0 / v; };
            return mm(M4{inv(d[6]), 0, 0, 0, 0, inv(d[7]), 0, 0, 0, 0, inv(d[8]), 0, 0, 0, 0, 1}, mm(rt, translate(-d[3] / 16, -d[4] / 16, -d[5] / 16)));
        };
        auto own = [&](char const* view) {
            auto const it = m->display.find(view);
            return it != m->display.end() ? it->second : std::array<double, 9>{0, 0, 0, 0, 0, 0, 1, 1, 1};
        };
        // Centred block units (Java axes) <-> the icon's pixels (x right, y through, z down).
        M4 const toIcon{16, 0, 0, 8, 0, 0, 16, 0.5, 0, -16, 0, 8, 0, 0, 0, 1};
        M4 const fromIcon{1.0 / 16, 0, 0, -0.5, 0, 0, -1.0 / 16, 0.5, 0, 1.0 / 16, 0, -0.5 / 16, 0, 0, 0, 1};
        M4 const centre = translate(-0.5, -0.5, -0.5);
        // item/generated's third and first person views; Bedrock holds a flat item in first person
        // as Java does but upright (without item/generated's 25 degree roll).
        std::array<double, 9> const genThird{0, 0, 0, 0, 3, 1, 0.55, 0.55, 0.55}, genFirst{0, -90, 0, 1.13, 3.2, 1.13, 0.68, 0.68, 0.68};
        // Held, dropped and framed, Java places a model by its display relative to a flat item's:
        // the mesh is the model where third person puts it, seen as the flat icon.
        M4 const place3 = mm(toIcon, mm(inverse(genThird), mm(display(own("thirdperson_righthand")), centre)));
        // First person puts it elsewhere relative to the flat item: from third person's place to
        // first person's, in the icon's pixels.
        M4 const fix1 = mm(toIcon, mm(inverse(genFirst), mm(display(own("firstperson_righthand")),
                                                              mm(inverse(own("thirdperson_righthand")), mm(display(genThird), fromIcon)))));
        for (size_t c = 0; c < 4; ++c)
            for (size_t row = 0; row < 4; ++row) mesh.firstPerson[c * 4 + row] = float(fix1[row * 4 + c]);
        auto place = [&](V const& p) {
            V const q{p[0] / 16, p[1] / 16, p[2] / 16};
            return V{place3[0] * q[0] + place3[1] * q[1] + place3[2] * q[2] + place3[3], place3[4] * q[0] + place3[5] * q[1] + place3[6] * q[2] + place3[7],
                     place3[8] * q[0] + place3[9] * q[1] + place3[10] * q[2] + place3[11]};
        };
        std::vector<HandQuad> out;
        for (auto const& el : *m->elements) {
            bool const turned = el.turn && el.turn->angle != 0;
            auto turnP = [&](P3 c) {
                if (!turned) return c;
                c = turnPoint(c, *el.turn);
                if (el.turn->rescale) {
                    double const k = 1.0 / std::cos(el.turn->angle * kPi / 180.0);
                    auto const&  o = el.turn->origin;
                    if (el.turn->axis != 'x') c.x = o.x + (c.x - o.x) * k;
                    if (el.turn->axis != 'y') c.y = o.y + (c.y - o.y) * k;
                    if (el.turn->axis != 'z') c.z = o.z + (c.z - o.z) * k;
                }
                return c;
            };
            auto placeP = [&](P3 const& p) {
                P3 const t = turnP(p);
                return place(V{t.x, t.y, t.z});
            };
            for (auto const& [fdir, fc] : el.faces) {
                auto const d = dirOf(fdir);
                if (!d) continue;
                auto oc = orderedCorners(fdir, planePoints(fdir, el.from, el.to));
                if (!oc) continue;
                auto ref2 = resolveRef(*m, fc.texture);
                if (!ref2 || ref2->empty()) continue;
                Image const* tex = javaTex(*ref2);
                if (!tex || tex->width <= 0 || tex->height <= 0) continue;
                double const u0 = fc.hasUv ? fc.uv[0] : oc->rect[0], v0 = fc.hasUv ? fc.uv[1] : oc->rect[1];
                double const u1 = fc.hasUv ? fc.uv[2] : oc->rect[2], v1 = fc.hasUv ? fc.uv[3] : oc->rect[3];
                std::array<std::pair<double, double>, 4> uvs{{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
                int const r = ((fc.rotation / 90) % 4 + 4) % 4;
                if (r) std::rotate(uvs.begin(), uvs.end() - r, uvs.end());
                double const sw = tex->width / 16.0, sh = tex->height / 16.0;
                for (auto& uv : uvs) uv.first *= sw, uv.second *= sh; // texture pixels
                std::array<V, 4> c, raw;
                for (size_t i = 0; i < 4; ++i) {
                    c[i]          = placeP(oc->corners[i]);
                    P3 const t    = turnP(oc->corners[i]);
                    raw[i]        = V{t.x / 16, t.y / 16, t.z / 16};
                }
                // The face's outward way, through the same transform.
                P3 const ctr{(el.from.x + el.to.x) / 2, (el.from.y + el.to.y) / 2, (el.from.z + el.to.z) / 2};
                P3 const tip{ctr.x + (*d)[0], ctr.y + (*d)[1], ctr.z + (*d)[2]};
                V const  pc = placeP(ctr), pt = placeP(tip);
                V        want{pt[0] - pc[0], pt[1] - pc[1], pt[2] - pc[2]};
                if (insideOut(fdir, el.from, el.to))
                    for (auto& x : want) x = -x;
                // Breakpoints along an edge where the texture coordinates cross a texel line.
                auto cuts = [&](std::pair<double, double> a, std::pair<double, double> b) {
                    std::vector<double> t{0.0, 1.0};
                    for (int axis = 0; axis < 2; ++axis) {
                        double const s = axis ? a.second : a.first, e = axis ? b.second : b.first;
                        if (std::abs(e - s) < 1e-9) continue;
                        for (double k = std::floor(std::min(s, e)) + 1; k < std::max(s, e) - 1e-9 && t.size() < 258; k += 1)
                            t.push_back((k - s) / (e - s));
                    }
                    std::sort(t.begin(), t.end());
                    t.erase(std::unique(t.begin(), t.end(), [](double x, double y) { return std::abs(x - y) < 1e-9; }), t.end());
                    return t;
                };
                auto const ta = cuts(uvs[0], uvs[1]), tb = cuts(uvs[0], uvs[3]);
                auto at = [&](double a, double b) {
                    V p;
                    for (size_t k = 0; k < 3; ++k) p[k] = c[0][k] + a * (c[1][k] - c[0][k]) + b * (c[3][k] - c[0][k]);
                    return p;
                };
                V const e1{c[1][0] - c[0][0], c[1][1] - c[0][1], c[1][2] - c[0][2]}, e2{c[3][0] - c[0][0], c[3][1] - c[0][1], c[3][2] - c[0][2]};
                V n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                double const len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len < 1e-12) continue;
                bool const reverse = n[0] * want[0] + n[1] * want[1] + n[2] * want[2] < 0;
                for (auto& x : n) x *= (reverse ? -1.0 : 1.0) / len;
                uint32_t const tint = fc.tintindex >= 0 && size_t(fc.tintindex) < tints.size() ? tints[size_t(fc.tintindex)] : 0xFFFFFFu;
                // One colour per cell (0: see-through), then runs of one colour as one quad.
                auto colorAt = [&](size_t i, size_t j) -> uint32_t {
                    double const am = (ta[i] + ta[i + 1]) / 2, bm = (tb[j] + tb[j + 1]) / 2;
                    double const u = uvs[0].first + am * (uvs[1].first - uvs[0].first) + bm * (uvs[3].first - uvs[0].first);
                    double const v = uvs[0].second + am * (uvs[1].second - uvs[0].second) + bm * (uvs[3].second - uvs[0].second);
                    int const tx = ((int(std::floor(u)) % tex->width) + tex->width) % tex->width; // Java wraps
                    int const ty = ((int(std::floor(v)) % tex->height) + tex->height) % tex->height;
                    uint32_t const texel = tex->at(tx, ty);
                    uint32_t const alpha = texel >> 24;
                    if (alpha == 0) return 0;
                    auto ch = [&](int s, int tsh) {
                        double const x = ((texel >> s) & 0xFF) * (((tint >> tsh) & 0xFF) / 255.0);
                        return uint32_t(std::clamp(x + 0.5, 0.0, 255.0)) << s;
                    };
                    return ch(0, 16) | ch(8, 8) | ch(16, 0) | (alpha << 24);
                };
                auto atRaw = [&](double a, double b) {
                    V p;
                    for (size_t k = 0; k < 3; ++k) p[k] = raw[0][k] + a * (raw[1][k] - raw[0][k]) + b * (raw[3][k] - raw[0][k]);
                    return p;
                };
                V nr;
                {
                    V const r1{raw[1][0] - raw[0][0], raw[1][1] - raw[0][1], raw[1][2] - raw[0][2]};
                    V const r2{raw[3][0] - raw[0][0], raw[3][1] - raw[0][1], raw[3][2] - raw[0][2]};
                    nr = V{r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0]};
                    double const l = std::sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
                    for (auto& x : nr) x *= (reverse ? -1.0 : 1.0) / (l > 1e-12 ? l : 1.0);
                }
                auto emit = [&](size_t i0, size_t i1, size_t j, uint32_t color) {
                    HandQuad q, r;
                    q.color = r.color = color;
                    std::array<V, 4> p{at(ta[i0], tb[j]), at(ta[i1], tb[j]), at(ta[i1], tb[j + 1]), at(ta[i0], tb[j + 1])};
                    std::array<V, 4> m{atRaw(ta[i0], tb[j]), atRaw(ta[i1], tb[j]), atRaw(ta[i1], tb[j + 1]), atRaw(ta[i0], tb[j + 1])};
                    if (reverse) std::swap(p[1], p[3]), std::swap(m[1], m[3]);
                    for (size_t k = 0; k < 4; ++k)
                        for (size_t x = 0; x < 3; ++x) q.pos[k * 3 + x] = float(p[k][x]), r.pos[k * 3 + x] = float(m[k][x]);
                    q.normal = {float(n[0]), float(n[1]), float(n[2])};
                    r.normal = {float(nr[0]), float(nr[1]), float(nr[2])};
                    out.push_back(q);
                    mesh.model.push_back(r);
                };
                for (size_t j = 0; j + 1 < tb.size(); ++j) {
                    size_t   start = 0;
                    uint32_t run   = colorAt(0, j);
                    for (size_t i = 1; i + 1 <= ta.size(); ++i) {
                        uint32_t const c = i + 1 < ta.size() ? colorAt(i, j) : ~run; // ~run: end the last run
                        if (c == run) continue;
                        if (run) emit(start, i, j, run);
                        start = i, run = c;
                    }
                }
            }
        }
        if (out.empty()) return std::nullopt;
        mesh.quads = std::move(out);
        return mesh;
    }

    std::optional<Image> itemIcon(std::string const& ref, bool flatOnly = false, std::vector<uint32_t> const& tints = {}) {
        std::shared_ptr<Model> m = iconModel(ref);
        if (!m) return std::nullopt;
        if (m->elements && !m->elements->empty() && flatOnly) return std::nullopt;
        if (m->elements && !m->elements->empty()) return guiIcon(*m, tints);
        return flatIcon(*m, tints);
    }

    // An item model by name, or a model file by path (CIT "./model"); null when missing.
    std::shared_ptr<Model> iconModel(std::string const& ref) {
        std::shared_ptr<Model> m;
        try {
            if (ref.ends_with(".json")) { // a model file given by path (CIT "./model")
                auto j = parseJson(mOpt.pack->read(ref));
                if (!j || !j->is_object()) return nullptr;
                m = std::make_shared<Model>();
                if (auto parent = j->value("parent", std::string{}); !parent.empty() && !stripNs(parent).starts_with("builtin/")) {
                    auto base   = loadModel(parent);
                    m->textures = base->textures;
                    m->elements = base->elements;
                    m->gui      = base->gui;
                    m->guiLight = base->guiLight;
                    m->display  = base->display;
                }
                readDisplay(*m, *j);
                if (auto t = j->find("textures"); t != j->end() && t->is_object())
                    for (auto const& [k, v] : t->items()) m->textures[k] = TextureRef{spriteOf(v), v.is_string()};
                if (auto e = j->find("elements"); e != j->end() && e->is_array())
                    m->elements = std::make_shared<std::vector<Element> const>(parseElements(*e));
                mModels["@icon"] = m;
            } else m = loadModel(ref);
        } catch (MissingModel const&) {
            return nullptr;
        }
        return m;
    }

    std::optional<Image> flatIcon(Model const& model, std::vector<uint32_t> const& tints) {
        Model const* const m = &model;
        // item/generated: layer0, layer1, ... on top of each other, at the largest size.
        std::vector<Image const*> layers;
        for (int i = 0;; ++i) {
            auto it = m->textures.find("layer" + std::to_string(i));
            if (it == m->textures.end()) break;
            auto ref2 = resolveRef(*m, it->second.value);
            if (Image const* t = ref2 && !ref2->empty() ? javaTex(*ref2) : nullptr) layers.push_back(t);
        }
        if (layers.empty()) return std::nullopt;
        int size = 0;
        for (auto const* l : layers) size = std::max(size, l->width);
        Image out = blank(size, size);
        for (size_t li = 0; li < layers.size(); ++li) {
            auto const*    l    = layers[li];
            uint32_t const tint = li < tints.size() ? tints[li] : 0xFFFFFFu;
            int const      s    = std::max(1, size / std::max(1, l->width));
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    int const sx = x / s, sy = y / s;
                    if (sx >= l->width || sy >= l->height) continue;
                    uint32_t px = l->at(sx, sy);
                    if (tint != 0xFFFFFFu) { // image is RGBA (R low), tints are 0xRRGGBB
                        auto ch = [&](int sh, int tsh) { return ((((px >> sh) & 0xFF) * ((tint >> tsh) & 0xFF) + 127) / 255) << sh; };
                        px = ch(0, 16) | ch(8, 8) | ch(16, 0) | (px & 0xFF000000u);
                    }
                    out.pixels[size_t(y) * size_t(size) + size_t(x)] = over(out.pixels[size_t(y) * size_t(size) + size_t(x)], px);
                }
        }
        return out;
    }

    std::optional<std::pair<Image, Image>> flatPbr(std::string const& ref) {
        auto m = iconModel(ref);
        if (!m || (m->elements && !m->elements->empty())) return std::nullopt;
        std::vector<std::pair<Image const*, std::string>> layers;
        for (int i = 0;; ++i) {
            auto it = m->textures.find("layer" + std::to_string(i));
            if (it == m->textures.end()) break;
            auto ref2 = resolveRef(*m, it->second.value);
            if (Image const* t = ref2 && !ref2->empty() ? javaTex(*ref2) : nullptr) layers.emplace_back(t, stripNs(*ref2));
        }
        if (layers.empty()) return std::nullopt;
        int size = 0;
        for (auto const& l : layers) size = std::max(size, l.first->width);
        Image n = blank(size, size), s = blank(size, size);
        for (auto& p : n.pixels) p = 0xFFFF8080u; // flat, no occlusion
        for (auto& p : s.pixels) p = 0xFF000000u; // rough, not metal, not glowing
        bool any = false;
        for (auto const& [l, name] : layers) {
            Maps const* maps = pbrMaps(name, *l);
            if (!maps) continue;
            any = true;
            int const sc = std::max(1, size / std::max(1, l->width));
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    int const sx = x / sc, sy = y / sc;
                    if (sx >= l->width || sy >= l->height || (l->at(sx, sy) >> 24) == 0) continue;
                    size_t const at = size_t(y) * size_t(size) + size_t(x);
                    n.pixels[at] = maps->n.at(sx, sy);
                    s.pixels[at] = maps->s.at(sx, sy);
                }
        }
        if (!any) return std::nullopt;
        return std::pair<Image, Image>{std::move(n), std::move(s)};
    }

    // The labPBR maps of a texture (by Java name), each sized like the texture; nullopt
    // when the pack has neither. Missing one of the two: neutral values.
    struct Maps {
        Image n, s;
    };
    Maps const* pbrMaps(std::string const& texName, Image const& colour) {
        if (auto it = mPbr.find(texName); it != mPbr.end()) return it->second ? &*it->second : nullptr;
        auto load = [&](char const* suffix) -> std::optional<Image> {
            auto data = mOpt.pack->read("assets/minecraft/textures/" + texName + suffix + ".png");
            if (!data) return std::nullopt;
            auto img = decodePng(*data);
            if (!img || img->width <= 0) return std::nullopt;
            if (img->height > img->width && img->height % img->width == 0) { // frames: the first
                img->height = img->width;
                img->pixels.resize(size_t(img->width) * size_t(img->width));
            }
            if (img->width == colour.width && img->height == colour.height) return img;
            Image out = blank(colour.width, colour.height); // nearest, to the colour's size
            for (int y = 0; y < colour.height; ++y)
                for (int x = 0; x < colour.width; ++x)
                    out.pixels[size_t(y) * size_t(colour.width) + size_t(x)] =
                        img->at(x * img->width / colour.width, y * img->height / colour.height);
            return out;
        };
        auto n = load("_n"), s = load("_s");
        auto& slot = mPbr[texName];
        if (!n && !s) return nullptr;
        Maps m;
        m.n = n ? std::move(*n) : blank(colour.width, colour.height);
        m.s = s ? std::move(*s) : blank(colour.width, colour.height);
        if (!n) std::fill(m.n.pixels.begin(), m.n.pixels.end(), 0xFFFF8080u); // flat, no AO
        if (!s) std::fill(m.s.pixels.begin(), m.s.pixels.end(), 0xFF000000u); // rough, no emission
        slot = std::move(m);
        return &*slot;
    }

    // One channel set of the maps laid out like the colour image: each layer's map, masked by
    // the colour's alpha, drawn the way the colour was. part: 0 normal RGB, 1 normal A,
    // 2 specular RGB, 3 specular A (the last two carried in R).
    Image const* maskedMap(Layer const& l, int part) {
        auto const key = std::pair{l.tex, part};
        if (auto it = mMasked.find(key); it != mMasked.end()) return it->second ? &*it->second : nullptr;
        auto&       slot = mMasked[key];
        Maps const* maps = l.tex ? pbrMaps(l.texName, *l.tex) : nullptr;
        if (!maps) return nullptr;
        Image out = *l.tex;
        Image const& src = part < 2 ? maps->n : maps->s;
        for (size_t i = 0; i < out.pixels.size(); ++i) {
            uint32_t const v = src.pixels[i], a = out.pixels[i] & 0xFF000000u;
            out.pixels[i] = (part % 2 == 0 ? (v & 0x00FFFFFFu) : (v >> 24)) | a;
        }
        slot = std::move(out);
        return &*slot;
    }

    // The shader maps for an image drawn from these layers, when any layer has some.
    void emitPbr(std::string const& name, std::vector<Layer const*> const& layers, int scale) {
        if (mResult.pbrTextures.contains(name + "_n")) return;
        bool any = false;
        for (auto const* l : layers) any |= l->tex && pbrMaps(l->texName, *l->tex) != nullptr;
        if (!any) return;
        std::array<Image, 4> parts;
        for (int part = 0; part < 4; ++part) {
            std::vector<Layer>        copies;
            copies.reserve(layers.size());
            for (auto const* l : layers) {
                Layer c = *l;
                c.tint  = std::nullopt;
                Image const* m = maskedMap(*l, part);
                if (!m) { // no maps: neutral values under its colour's alpha
                    auto const key = std::pair{l->tex, part + 8};
                    auto&      slot = mMasked[key];
                    if (!slot && l->tex) {
                        Image flat = *l->tex;
                        uint32_t const v = part == 0 ? 0xFF8080u : part == 1 ? 0xFFu : part == 2 ? 0x000000u : 0xFFu;
                        for (auto& p : flat.pixels) p = v | (p & 0xFF000000u);
                        slot = std::move(flat);
                    }
                    m = slot ? &*slot : nullptr;
                }
                if (!m) continue;
                c.tex = m;
                copies.push_back(std::move(c));
            }
            std::vector<Layer const*> ptrs;
            for (auto const& c : copies) ptrs.push_back(&c);
            parts[size_t(part)] = render(ptrs, scale);
        }
        Image normal = parts[0], spec = parts[2];
        for (size_t i = 0; i < normal.pixels.size(); ++i) {
            bool const seen = (parts[0].pixels[i] >> 24) != 0;
            normal.pixels[i] = seen ? (parts[0].pixels[i] & 0x00FFFFFFu) | ((parts[1].pixels[i] & 0xFF) << 24) : 0xFFFF8080u;
            spec.pixels[i]   = seen ? (parts[2].pixels[i] & 0x00FFFFFFu) | ((parts[3].pixels[i] & 0xFF) << 24) : 0xFF000000u;
        }
        mResult.pbrTextures.emplace(name + "_n", std::move(normal));
        mResult.pbrTextures.emplace(name + "_s", std::move(spec));
    }

    // The shader maps of a single texture used as it is (P5 quads).
    void emitPbr(std::string const& name, std::string const& texName, Image const& colour) {
        if (mResult.pbrTextures.contains(name + "_n")) return;
        Maps const* maps = pbrMaps(texName, colour);
        if (!maps) return;
        mResult.pbrTextures.emplace(name + "_n", maps->n);
        mResult.pbrTextures.emplace(name + "_s", maps->s);
    }

    std::optional<Image> texture(std::string const& pathOrName) {
        if (pathOrName.starts_with("assets/")) {
            auto bytes = readAny(pathOrName);
            if (!bytes) return std::nullopt;
            auto img = decodePng(*bytes);
            if (img && img->height > img->width && img->height % img->width == 0) { // animation strip: first frame
                img->height = img->width;
                img->pixels.resize(size_t(img->width) * size_t(img->width));
            }
            return img;
        }
        Image const* t = javaTex(pathOrName);
        return t ? std::optional(*t) : std::nullopt;
    }

private:
    BakeOptions const&                                       mOpt;
    BakeResult                                               mResult;
    std::unordered_map<std::string, std::optional<Image>>    mTextures;
    std::unordered_map<std::string, std::shared_ptr<Model>>  mModels;
    std::unordered_map<std::string, std::optional<Maps>>     mPbr;    // Java texture -> its labPBR maps
    std::map<std::pair<Image const*, int>, std::optional<Image>> mMasked; // see maskedMap
    std::optional<std::set<std::string>>                     mPackFiles; // itemInfo: the pack's models and textures
    std::set<std::string>                                    mShapedBlocks; // Java blocks baked with their own shape
    std::unordered_map<std::string, std::map<std::string, std::string>> mFaceCache;
    std::unordered_map<uint64_t, std::string>                mEmitted;
    // Transformed (and tinted) face images: the same element face recurs in many states.
    std::unordered_map<std::string, Image>                   mParts;

    std::optional<std::string> readAny(std::string const& path) const {
        if (auto d = mOpt.pack->read(path)) return d;
        if (mOpt.vanilla) return mOpt.vanilla->read(path);
        return std::nullopt;
    }

    std::optional<Json> loadJson(std::string const& rel) const {
        if (auto j = parseJson(mOpt.pack->read(rel))) return j;
        if (mOpt.vanilla) return parseJson(mOpt.vanilla->read(rel));
        return std::nullopt;
    }

    Image const* javaTex(std::string const& ref) {
        std::string const name = stripNs(ref);
        if (auto it = mTextures.find(name); it != mTextures.end()) return it->second ? &*it->second : nullptr;
        std::optional<Image> img;
        std::string const    path = "assets/minecraft/textures/" + name + ".png";
        // The pack's Java texture, else its Bedrock one, else the vanilla Java texture.
        auto data = mOpt.pack->read(path);
        if (!data && mOpt.packBedrockTexture) {
            img = mOpt.packBedrockTexture(name);
            if (img && img->width > 0 && img->height > img->width && img->height % img->width == 0) { // flipbook: first frame
                img->height = img->width;
                img->pixels.resize(size_t(img->width) * size_t(img->width));
            }
        }
        if (!img && !data && mOpt.vanilla) data = mOpt.vanilla->read(path);
        if (!img && data) {
            img = decodePng(*data);
            if (img && img->height > img->width) {
                // Only a texture with an animation .mcmeta is a strip of frames (use the first).
                auto meta = readAny("assets/minecraft/textures/" + name + ".png.mcmeta");
                if (meta && meta->find("\"animation\"") != std::string::npos) {
                    img->height = img->width;
                    img->pixels.resize(size_t(img->width) * size_t(img->width));
                }
            }
        }
        if (!img && mOpt.fallbackTexture) {
            img = mOpt.fallbackTexture(name);
            // A stand-in with frames stacked vertically (a Bedrock flipbook has no
            // .mcmeta to say so): use the first frame.
            if (img && img->width > 0 && img->height > img->width && img->height % img->width == 0) {
                img->height = img->width;
                img->pixels.resize(size_t(img->width) * size_t(img->width));
            }
        }
        auto& slot = mTextures[name] = std::move(img);
        return slot ? &*slot : nullptr;
    }

    std::shared_ptr<Model> loadModel(std::string const& ref) {
        std::string const name = stripNs(ref);
        if (auto it = mModels.find(name); it != mModels.end()) return it->second;
        auto j = loadJson("assets/minecraft/models/" + name + ".json");
        if (!j || !j->is_object()) j = parseJson(builtinModel(name));
        if (!j || !j->is_object()) throw MissingModel{name};
        auto model = std::make_shared<Model>();
        if (auto parent = j->value("parent", std::string{}); !parent.empty() && !stripNs(parent).starts_with("builtin/")) {
            std::shared_ptr<Model> base;
            try {
                base = loadModel(parent);
            } catch (MissingModel const&) {
                // As Java: a parent that is nowhere is left out; a model with its own
                // elements still draws (others are missing too).
                if (!j->contains("elements")) throw;
                mResult.problems.push_back(std::format("{}: parent {} not found (ignored)", name, stripNs(parent)));
            }
            if (base) {
                model->textures = base->textures;
                model->elements = base->elements;
                model->gui      = base->gui;
                model->guiLight = base->guiLight;
                model->display  = base->display;
            }
        }
        readDisplay(*model, *j);
        if (auto t = j->find("textures"); t != j->end() && t->is_object())
            for (auto const& [k, v] : t->items()) model->textures[k] = TextureRef{spriteOf(v), v.is_string()};
        if (auto e = j->find("elements"); e != j->end() && e->is_array())
            model->elements = std::make_shared<std::vector<Element> const>(parseElements(*e));
        mModels[name] = model;
        return model;
    }

    static std::optional<std::string> resolveRef(Model const& m, std::string ref) {
        for (int i = 0; i < 8; ++i) {
            if (!ref.starts_with("#")) return ref;
            auto it = m.textures.find(ref.substr(1));
            ref     = it == m.textures.end() ? std::string{} : it->second.value;
        }
        return std::nullopt;
    }

    int bakeScale(std::vector<Part> const& parts) {
        int need = 1;
        for (auto const& part : parts) {
            auto m = loadModel(part.model);
            if (!m->elements) continue;
            for (auto const& el : *m->elements) {
                for (double v : {el.from.x, el.from.y, el.from.z, el.to.x, el.to.y, el.to.z}) {
                    bool found = false;
                    for (int s : {1, 2, 4})
                        if (std::abs(v * s - double(pyRound(v * s))) < 1e-6) {
                            need  = std::max(need, s);
                            found = true;
                            break;
                        }
                    if (!found) need = 4;
                }
                for (auto const& [dir, fc] : el.faces) {
                    auto ref = resolveRef(*m, fc.texture);
                    if (!ref || ref->empty()) continue;
                    if (auto const* t = javaTex(*ref)) need = std::max(need, std::min(4, std::max(t->width, t->height) / 16));
                }
            }
        }
        return std::min(4, need);
    }

    std::vector<Layer> collectLayers(std::vector<Part> const& parts, std::string const& face, Tints const& tints) {
        std::vector<Layer> layers;
        for (auto const& part : parts) {
            auto m = loadModel(part.model);
            if (!m->elements) continue;
            for (auto const& el : *m->elements) {
                bool const turned = el.turn && el.turn->angle != 0;
                for (auto const& [fdir, fc] : el.faces) {
                    auto const d = dirOf(fdir);
                    if (!d || faceOfDir(rotDir(*d, part.rx, part.ry)) != face) continue;
                    // A turned part's faces along its axis stay flat (a curved rail's ties,
                    // turned about y, seen from above); its other faces are tilted: left out.
                    if (turned && !alongAxis(fdir, el.turn->axis)) continue;
                    auto oc = orderedCorners(fdir, planePoints(fdir, el.from, el.to));
                    if (!oc) continue;
                    double const u0 = fc.hasUv ? fc.uv[0] : oc->rect[0], v0 = fc.hasUv ? fc.uv[1] : oc->rect[1];
                    double const u1 = fc.hasUv ? fc.uv[2] : oc->rect[2], v1 = fc.hasUv ? fc.uv[3] : oc->rect[3];
                    std::array<std::pair<double, double>, 4> uvs{{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
                    int const r = ((fc.rotation / 90) % 4 + 4) % 4;
                    if (r) std::rotate(uvs.begin(), uvs.end() - r, uvs.end());
                    auto ref = resolveRef(*m, fc.texture);
                    if (!ref || ref->empty()) continue;
                    Image const* tex = javaTex(*ref);
                    if (!tex) continue; // missing texture: leave that part to Bedrock
                    Layer layer;
                    std::array<P3, 4> rotated;
                    for (size_t i = 0; i < 4; ++i) {
                        rotated[i]     = rotPoint(turned ? turnPoint(oc->corners[i], *el.turn) : oc->corners[i], part.rx, part.ry);
                        layer.world[i] = faceCoords(face, rotated[i]);
                    }
                    if (turned) {
                        std::array<P3, 4> flat;
                        for (size_t i = 0; i < 4; ++i) flat[i] = faceCoords(face, rotPoint(oc->corners[i], part.rx, part.ry));
                        layer.flat = flat;
                    }
                    layer.depth = layer.world[0].z;
                    layer.order = int(layers.size());
                    layer.uvs   = uvs;
                    layer.tex   = tex;
                    layer.texName = stripNs(*ref);
                    if (auto t = tints.find(fc.tintindex); t != tints.end()) layer.tint = t->second;
                    layer.tintIndex = fc.tintindex;
                    layer.plane = std::clamp(rotated[0][axisOf(face)] / 16.0, 0.0, 1.0);
                    layers.push_back(layer);
                }
            }
        }
        return layers;
    }

    Image const& partImage(Layer const& l, int w, int h, std::array<double, 8> const& quad) {
        std::string key(sizeof(void*) + sizeof(int) * 2 + sizeof(quad) + sizeof(int) * 4, ' ');
        char*       at = key.data();
        auto        put = [&](void const* p, size_t n) {
            std::memcpy(at, p, n);
            at += n;
        };
        put(&l.tex, sizeof(void*));
        put(&w, sizeof(int));
        put(&h, sizeof(int));
        put(quad.data(), sizeof(quad));
        int const t[4] = {l.tint ? 1 : 0, l.tint ? (*l.tint)[0] : 0, l.tint ? (*l.tint)[1] : 0, l.tint ? (*l.tint)[2] : 0};
        put(t, sizeof(t));
        if (auto it = mParts.find(key); it != mParts.end()) return it->second;
        Image part = quadTransform(*l.tex, w, h, quad);
        if (l.tint) tint(part, *l.tint);
        return mParts.emplace(std::move(key), std::move(part)).first->second;
    }

    Image render(std::vector<Layer const*> layers, int scale) {
        int const size   = 16 * scale;
        Image     canvas = blank(size, size);
        std::stable_sort(layers.begin(), layers.end(), [](Layer const* a, Layer const* b) {
            return a->depth != b->depth ? a->depth < b->depth : a->order < b->order;
        });
        for (auto const* l : layers) {
            auto const& world = l->flat ? *l->flat : l->world;
            double      a0 = INFINITY, a1 = -INFINITY, b0 = INFINITY, b1 = -INFINITY;
            for (auto const& w : world) a0 = std::min(a0, w.x), a1 = std::max(a1, w.x), b0 = std::min(b0, w.y), b1 = std::max(b1, w.y);
            int const w = int(pyRound((a1 - a0) * scale)), h = int(pyRound((b1 - b0) * scale));
            if (w <= 0 || h <= 0) continue;
            double const          su = l->tex->width / 16.0, sv = l->tex->height / 16.0;
            std::array<double, 8> quad{};
            size_t                qi = 0;
            for (auto const& [a, b] : {std::pair{a0, b0}, std::pair{a0, b1}, std::pair{a1, b1}, std::pair{a1, b0}}) {
                std::pair<double, double> uv{0, 0};
                for (size_t i = 0; i < 4; ++i)
                    if (std::abs(world[i].x - a) < 1e-6 && std::abs(world[i].y - b) < 1e-6) {
                        uv = l->uvs[i];
                        break;
                    }
                quad[qi++] = uv.first * su;
                quad[qi++] = uv.second * sv;
            }
            Image const& part = partImage(*l, w, h, quad);
            if (l->flat) {
                std::array<std::pair<double, double>, 3> dst, src;
                for (size_t i = 0; i < 3; ++i) {
                    dst[i] = {l->world[i].x * scale, l->world[i].y * scale};
                    src[i] = {((*l->flat)[i].x - a0) * scale, ((*l->flat)[i].y - b0) * scale};
                }
                Image const turned = affineTransform(part, size, affineFrom(dst, src));
                composite(canvas, turned, 0, 0, 0, 0, size, size);
                continue;
            }
            int const x = int(pyRound(a0 * scale)), y = int(pyRound(b0 * scale));
            int const cx0 = std::max(0, -x), cy0 = std::max(0, -y);
            int const cx1 = std::min(w, size - x), cy1 = std::min(h, size - y);
            if (cx1 > cx0 && cy1 > cy0) composite(canvas, part, x + cx0, y + cy0, cx0, cy0, cx1, cy1);
        }
        return canvas;
    }

    // P5: does this model set need its own shape (anything but whole-block cubes)?
    bool needsGeometry(std::vector<Part> const& parts) {
        for (auto const& part : parts) {
            auto m = loadModel(part.model);
            if (!m->elements) continue;
            for (auto const& el : *m->elements) {
                if (el.turn && el.turn->angle != 0) return true;
                if (el.from.x != 0 || el.from.y != 0 || el.from.z != 0 || el.to.x != 16 || el.to.y != 16 || el.to.z != 16)
                    return true;
            }
        }
        return false;
    }

    // P5: the model set as quads in block space (0..1), turned into place, each with its own
    // texture (emitted as a sprite) and UVs within that sprite (0..1).
    // facesFor: grass block sides carry Bedrock's tint mask in alpha (as its own grass_side:
    // grass 255, dirt 0); the atlas tints them per biome through overlay_color variants.
    bool mSideTintMask = false;
    bool mGeometryCutout = false; // set by geometryFor: some texture has see-through pixels
    bool mGeometryTranslucent = false; // ... and a good part of them half see-through (stained glass)

    // tints: fixed colours by tintindex (redstone dust by power), baked into the quads.
    // bakeTints: the tint is drawn into the face's image instead (item shapes: a held block's
    // mesh has no colour of its own to carry it).
    Json geometryFor(std::vector<Part> const& parts, Tints const& tints = {}, bool bakeTints = false) {
        mGeometryCutout = false;
        mGeometryTranslucent = false;
        Json out = Json::array();
        auto dirIndex = [](std::array<int, 3> const& d) {
            for (int i = 0; i < 6; ++i)
                if (dirOf(kFaces[i]) == d) return i;
            return -1;
        };
        for (auto const& part : parts) {
            auto m = loadModel(part.model);
            if (!m->elements) continue;
            for (auto const& el : *m->elements) {
                bool const turned = el.turn && el.turn->angle != 0;
                // A flat lying element (string, dust) has its two faces in one plane. Bedrock
                // draws such shapes two-sided and, in see-through layers, re-sorts them by
                // distance as the camera moves: the pair flickers. Java culls the one facing
                // away; seen from above that is the underside, so it is left out.
                bool const flatLying = el.from.y == el.to.y && !turned
                    && std::any_of(el.faces.begin(), el.faces.end(), [](auto const& f) { return f.first == "up"; });
                for (auto const& [fdir, fc] : el.faces) {
                    if (flatLying && fdir == "down") continue;
                    auto const d = dirOf(fdir);
                    if (!d) continue;
                    auto oc = orderedCorners(fdir, planePoints(fdir, el.from, el.to));
                    if (!oc) continue;
                    auto ref = resolveRef(*m, fc.texture);
                    if (!ref || ref->empty()) continue;
                    Image const* tex = javaTex(*ref);
                    if (!tex) continue;
                    double const u0 = fc.hasUv ? fc.uv[0] : oc->rect[0], v0 = fc.hasUv ? fc.uv[1] : oc->rect[1];
                    double const u1 = fc.hasUv ? fc.uv[2] : oc->rect[2], v1 = fc.hasUv ? fc.uv[3] : oc->rect[3];
                    std::array<std::pair<double, double>, 4> uvs{{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
                    int const r = ((fc.rotation / 90) % 4 + 4) % 4;
                    if (r) std::rotate(uvs.begin(), uvs.end() - r, uvs.end());
                    if (insideOut(fdir, el.from, el.to)) { // wound the other way: its other side shows
                        std::swap(oc->corners[1], oc->corners[3]);
                        std::swap(uvs[1], uvs[3]);
                    }
                    Json p = Json::array(), uv = Json::array();
                    // A flat element's two faces lie in one plane. Java shows the one facing the
                    // viewer (back faces are culled); Bedrock draws such shapes two-sided, so each
                    // is moved a hair (1/32 pixel) along its own way and the depth test decides.
                    int const    axis = (*d)[0] ? 0 : (*d)[1] ? 1 : 2;
                    double const lift = el.from[axis] == el.to[axis] ? 1.0 / 32 : 0.0;
                    for (size_t i = 0; i < 4; ++i) {
                        P3 c = oc->corners[i];
                        c.x += (*d)[0] * lift, c.y += (*d)[1] * lift, c.z += (*d)[2] * lift;
                        if (turned) {
                            c = turnPoint(c, *el.turn);
                            if (el.turn->rescale) {
                                double const k = 1.0 / std::cos(el.turn->angle * 3.14159265358979323846 / 180.0);
                                auto const&  o = el.turn->origin;
                                if (el.turn->axis != 'x') c.x = o.x + (c.x - o.x) * k;
                                if (el.turn->axis != 'y') c.y = o.y + (c.y - o.y) * k;
                                if (el.turn->axis != 'z') c.z = o.z + (c.z - o.z) * k;
                            }
                        }
                        c = rotPoint(c, part.rx, part.ry);
                        p.push_back(c.x / 16.0), p.push_back(c.y / 16.0), p.push_back(c.z / 16.0);
                        uv.push_back(uvs[i].first / 16.0), uv.push_back(uvs[i].second / 16.0);
                    }
                    size_t half = 0;
                    for (auto px : tex->pixels) {
                        uint32_t const a = px >> 24;
                        if (a < 255) mGeometryCutout = true;
                        if (a > 0 && a < 255) ++half;
                    }
                    if (half * 10 > tex->pixels.size()) mGeometryTranslucent = true;
                    Json q    = Json::object();
                    auto const tint = fc.tintindex >= 0 ? tints.find(fc.tintindex) : tints.end();
                    if (bakeTints && tint != tints.end()) {
                        Image tinted = *tex;
                        for (auto& px : tinted.pixels) {
                            auto ch = [&](int sh, int t) { return uint32_t(((px >> sh) & 0xFF) * uint32_t(t) / 255u) << sh; };
                            px = ch(0, tint->second[0]) | ch(8, tint->second[1]) | ch(16, tint->second[2]) | (px & 0xFF000000u);
                        }
                        q["t"] = emit(tinted);
                    } else {
                        q["t"] = emit(*tex);
                        // The texture as it is: connected-texture rules for it (matchTiles)
                        // apply to this face too.
                        auto& src = mResult.textureSources[q["t"].get<std::string>()];
                        if (std::string const t = stripNs(*ref); std::find(src.begin(), src.end(), t) == src.end()) src.push_back(t);
                    }
                    emitPbr(q["t"].get<std::string>(), stripNs(*ref), *tex);
                    q["p"]    = std::move(p);
                    q["uv"]   = std::move(uv);
                    q["d"]    = dirIndex(rotDir(*d, part.rx, part.ry));
                    auto cd   = fc.cullface.empty() ? std::nullopt : dirOf(fc.cullface);
                    q["c"]    = cd ? dirIndex(rotDir(*cd, part.rx, part.ry)) : -1;
                    q["s"]    = el.shade;
                    if (turned) q["tilt"] = true;
                    if (fc.tintindex >= 0) q["ti"] = fc.tintindex;
                    if (!bakeTints && tint != tints.end())
                        q["tc"] = (tint->second[0] << 16) | (tint->second[1] << 8) | tint->second[2];
                    out.push_back(std::move(q));
                }
            }
        }
        clipHidden(out);
        separateCoplanar(out);
        // Shapes in see-through layers are drawn without depth writes: the later of two faces
        // in one place shows. Faces looking down go first, so from above (as such shapes are
        // mostly seen: string, dust) the upper face shows, as Java's culling has it.
        std::stable_partition(out.begin(), out.end(), [](Json const& q) { return q.value("d", -1) == 0; });
        return out;
    }

    // A face fully hidden in places by opaque faces lying (almost) on it - a frame or decal
    // drawn a hair in front of a block's surface (XeKr's glowing outlines) - loses those
    // places: the faces no longer overlap, so nothing is left to flicker between them at a
    // distance, and the decal can stay on the surface instead of being pushed out of it.
    void clipHidden(Json& quads) {
        constexpr double kNear = 0.01; // blocks: "lying on it"
        static constexpr int axisOf[6] = {1, 1, 2, 2, 0, 0};
        static constexpr int signOf[6] = {-1, 1, -1, 1, -1, 1};
        struct Q {
            std::array<std::array<double, 3>, 4> p;
            std::array<double, 8>                uv;
            int                                  d;
            double                               off; // along the outward normal
        };
        auto read = [](Json const& j) -> std::optional<Q> {
            if (j.contains("tilt")) return std::nullopt;
            int const d = j.value("d", -1);
            if (d < 0 || d > 5 || !j.contains("p") || !j.contains("uv")) return std::nullopt;
            Q q;
            q.d = d;
            for (size_t k = 0; k < 4; ++k)
                for (size_t c = 0; c < 3; ++c) q.p[k][c] = j["p"][k * 3 + c].get<double>();
            for (size_t i = 0; i < 8; ++i) q.uv[i] = j["uv"][i].get<double>();
            q.off = q.p[0][size_t(axisOf[d])] * signOf[d];
            // Only flat rectangles (p2 = p1 + p3 - p0).
            for (size_t c = 0; c < 3; ++c)
                if (std::abs(q.p[2][c] - (q.p[1][c] + q.p[3][c] - q.p[0][c])) > 1e-6) return std::nullopt;
            return q;
        };
        // Is every texel the face shows opaque?
        auto opaque = [&](Json const& j, Q const& q) {
            auto it = mResult.textures.find(j.value("t", std::string{}));
            if (it == mResult.textures.end() || it->second.empty()) return false;
            Image const& img = it->second;
            double u0 = 1, u1 = 0, v0 = 1, v1 = 0;
            for (size_t k = 0; k < 4; ++k)
                u0 = std::min(u0, q.uv[k * 2]), u1 = std::max(u1, q.uv[k * 2]), v0 = std::min(v0, q.uv[k * 2 + 1]), v1 = std::max(v1, q.uv[k * 2 + 1]);
            int const x0 = std::clamp(int(std::floor(u0 * img.width + 1e-6)), 0, img.width - 1);
            int const x1 = std::clamp(int(std::ceil(u1 * img.width - 1e-6)), x0 + 1, img.width);
            int const y0 = std::clamp(int(std::floor(v0 * img.height + 1e-6)), 0, img.height - 1);
            int const y1 = std::clamp(int(std::ceil(v1 * img.height - 1e-6)), y0 + 1, img.height);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    if ((img.at(x, y) >> 24) != 255) return false;
            return true;
        };
        std::vector<std::optional<Q>> qs;
        std::vector<bool>             solid;
        for (auto const& j : quads) {
            qs.push_back(read(j));
            solid.push_back(qs.back() && opaque(j, *qs.back()));
        }
        Json result = Json::array();
        bool changed = false;
        for (size_t bi = 0; bi < quads.size(); ++bi) {
            auto const& B = qs[bi];
            if (!B) {
                result.push_back(quads[bi]);
                continue;
            }
            // B's own coordinates: s along p0->p1, t along p0->p3.
            std::array<double, 3> e1, e3;
            for (size_t c = 0; c < 3; ++c) e1[c] = B->p[1][c] - B->p[0][c], e3[c] = B->p[3][c] - B->p[0][c];
            double const l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2], l3 = e3[0] * e3[0] + e3[1] * e3[1] + e3[2] * e3[2];
            if (l1 < 1e-12 || l3 < 1e-12) {
                result.push_back(quads[bi]);
                continue;
            }
            auto st = [&](std::array<double, 3> const& x) {
                double s = 0, t = 0;
                for (size_t c = 0; c < 3; ++c) s += (x[c] - B->p[0][c]) * e1[c], t += (x[c] - B->p[0][c]) * e3[c];
                return std::pair{s / l1, t / l3};
            };
            std::vector<std::array<double, 4>> covers; // s0 s1 t0 t1, within [0,1]
            for (size_t ci = 0; ci < quads.size(); ++ci) {
                if (ci == bi || !solid[ci]) continue;
                auto const& C = qs[ci];
                if (C->d != B->d) continue;
                double const ahead = C->off - B->off;
                if (ahead < 1e-7 || ahead > kNear) continue; // must lie just in front
                double s0 = 1e9, s1 = -1e9, t0 = 1e9, t1 = -1e9;
                for (auto const& pt : C->p) {
                    auto [s, t] = st(pt);
                    s0 = std::min(s0, s), s1 = std::max(s1, s), t0 = std::min(t0, t), t1 = std::max(t1, t);
                }
                s0 = std::max(s0, 0.0), s1 = std::min(s1, 1.0), t0 = std::max(t0, 0.0), t1 = std::min(t1, 1.0);
                if (s1 - s0 > 1e-6 && t1 - t0 > 1e-6) covers.push_back({s0, s1, t0, t1});
            }
            if (covers.empty()) {
                result.push_back(quads[bi]);
                continue;
            }
            // Grid of B cut at every cover edge; keep the cells no cover hides.
            std::vector<double> ss{0, 1}, ts{0, 1};
            for (auto const& c : covers) ss.push_back(c[0]), ss.push_back(c[1]), ts.push_back(c[2]), ts.push_back(c[3]);
            auto uniq = [](std::vector<double>& v) {
                std::sort(v.begin(), v.end());
                v.erase(std::unique(v.begin(), v.end(), [](double a, double b) { return std::abs(a - b) < 1e-7; }), v.end());
            };
            uniq(ss), uniq(ts);
            size_t const nx = ss.size() - 1, ny = ts.size() - 1;
            std::vector<char> keep(nx * ny, 1);
            for (size_t y = 0; y < ny; ++y)
                for (size_t x = 0; x < nx; ++x) {
                    double const cs = (ss[x] + ss[x + 1]) / 2, ct = (ts[y] + ts[y + 1]) / 2;
                    for (auto const& c : covers)
                        if (cs > c[0] && cs < c[1] && ct > c[2] && ct < c[3]) keep[y * nx + x] = 0;
                }
            changed = true;
            auto emitCell = [&](double s0, double s1, double t0, double t1) {
                Json q = quads[bi];
                std::array<std::pair<double, double>, 4> const corner{{{s0, t0}, {s1, t0}, {s1, t1}, {s0, t1}}};
                for (size_t k = 0; k < 4; ++k) {
                    auto [s, t] = corner[k];
                    for (size_t c = 0; c < 3; ++c) q["p"][k * 3 + c] = B->p[0][c] + s * e1[c] + t * e3[c];
                    for (size_t c = 0; c < 2; ++c)
                        q["uv"][k * 2 + c] = B->uv[c] + s * (B->uv[2 + c] - B->uv[c]) + t * (B->uv[6 + c] - B->uv[c]);
                }
                result.push_back(std::move(q));
            };
            // Runs of kept cells per row, then rows with the same run merged downwards.
            std::vector<std::array<size_t, 4>> open; // x0 x1 y0 y1(exclusive)
            for (size_t y = 0; y < ny; ++y) {
                std::vector<std::pair<size_t, size_t>> runs;
                for (size_t x = 0; x < nx;) {
                    if (!keep[y * nx + x]) {
                        ++x;
                        continue;
                    }
                    size_t e = x;
                    while (e < nx && keep[y * nx + e]) ++e;
                    runs.emplace_back(x, e);
                    x = e;
                }
                std::vector<std::array<size_t, 4>> next;
                for (auto const& [a, b] : runs) {
                    auto it = std::find_if(open.begin(), open.end(), [&](auto const& o) { return o[0] == a && o[1] == b; });
                    if (it != open.end()) {
                        auto o = *it;
                        o[3] = y + 1;
                        next.push_back(o);
                        open.erase(it);
                    } else next.push_back({a, b, y, y + 1});
                }
                for (auto const& o : open) emitCell(ss[o[0]], ss[o[1]], ts[o[2]], ts[o[3]]);
                open = std::move(next);
            }
            for (auto const& o : open) emitCell(ss[o[0]], ss[o[1]], ts[o[2]], ts[o[3]]);
        }
        if (changed) quads = std::move(result);
    }

    // Java packs float decals a hair (0.01 pixel) above a surface, often several deep; Java's
    // depth buffer copes, Bedrock's does not (they flicker as the camera moves). Faces looking
    // the same way that overlap are kept in their order but at least kGap apart, starting
    // from the rearmost; so is a face just in front of the block's own boundary (the
    // neighbour's surface). Works along each face's normal, so tilted faces are covered too.
    static void separateCoplanar(Json& quads) {
        constexpr double kGap = 0.004; // blocks
        static constexpr int axisOf[6] = {1, 1, 2, 2, 0, 0};
        static constexpr int signOf[6] = {-1, 1, -1, 1, -1, 1};
        using V = std::array<double, 3>;
        auto dot   = [](V const& a, V const& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        auto cross = [](V const& a, V const& b) { return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
        struct Face {
            size_t           index;
            std::array<V, 4> p;
            V                n;
            double           off, placed;
            bool             axial;
            int              d;
        };
        std::vector<Face> faces;
        for (size_t i = 0; i < quads.size(); ++i) {
            auto const& q = quads[i];
            int const   d = q.value("d", -1);
            if (d < 0 || d > 5) continue;
            Face f;
            f.index = i;
            f.d     = d;
            for (int k = 0; k < 4; ++k)
                for (int c = 0; c < 3; ++c) f.p[size_t(k)][size_t(c)] = q["p"][size_t(k * 3 + c)].get<double>();
            f.axial = !q.contains("tilt");
            if (f.axial) {
                f.n                    = V{0, 0, 0};
                f.n[size_t(axisOf[d])] = signOf[d];
            } else {
                V const e1{f.p[1][0] - f.p[0][0], f.p[1][1] - f.p[0][1], f.p[1][2] - f.p[0][2]};
                V const e2{f.p[3][0] - f.p[0][0], f.p[3][1] - f.p[0][1], f.p[3][2] - f.p[0][2]};
                V       n   = cross(e1, e2);
                double  len = std::sqrt(dot(n, n));
                if (len < 1e-12) continue;
                for (auto& c : n) c /= len;
                if (n[size_t(axisOf[d])] * signOf[d] < 0) // outward: toward the direction it is shaded as
                    for (auto& c : n) c = -c;
                f.n = n;
            }
            f.off = f.placed = dot(f.n, f.p[0]);
            faces.push_back(f);
        }
        std::vector<size_t> order(faces.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return faces[a].off < faces[b].off; });
        // Do two faces with the same normal overlap, seen along it?
        auto overlap = [&](Face const& a, Face const& b) {
            V const ref = std::abs(a.n[1]) < 0.9 ? V{0, 1, 0} : V{1, 0, 0};
            V       u   = cross(a.n, ref);
            double  len = std::sqrt(dot(u, u));
            for (auto& c : u) c /= len;
            V const v = cross(a.n, u);
            double  lo[2][2] = {{1e9, 1e9}, {1e9, 1e9}}, hi[2][2] = {{-1e9, -1e9}, {-1e9, -1e9}};
            Face const* fs[2] = {&a, &b};
            for (int i = 0; i < 2; ++i)
                for (auto const& pt : fs[i]->p) {
                    double const x = dot(pt, u), y = dot(pt, v);
                    lo[i][0] = std::min(lo[i][0], x), hi[i][0] = std::max(hi[i][0], x);
                    lo[i][1] = std::min(lo[i][1], y), hi[i][1] = std::max(hi[i][1], y);
                }
            return std::min(hi[0][0], hi[1][0]) - std::max(lo[0][0], lo[1][0]) > 1e-6
                && std::min(hi[0][1], hi[1][1]) - std::max(lo[0][1], lo[1][1]) > 1e-6;
        };
        for (size_t oi = 0; oi < order.size(); ++oi) {
            Face& f = faces[order[oi]];
            if (f.axial) { // just in front of the block's boundary behind it
                double const boundary = signOf[f.d] > 0 ? 0.0 : -1.0; // as an offset along the normal
                if (f.off - boundary > 1e-9 && f.off - boundary < kGap) f.placed = boundary + kGap;
            }
            for (size_t oj = 0; oj < oi; ++oj) {
                Face const& o = faces[order[oj]];
                if (f.off - o.off <= 1e-9 || dot(f.n, o.n) < 0.9999) continue;
                if (o.placed + kGap <= f.placed || !overlap(f, o)) continue;
                f.placed = o.placed + kGap;
            }
            double const move = f.placed - f.off;
            if (move <= 1e-12) continue;
            auto& q = quads[f.index];
            for (int k = 0; k < 4; ++k)
                for (int c = 0; c < 3; ++c) q["p"][size_t(k * 3 + c)] = f.p[size_t(k)][size_t(c)] + f.n[size_t(c)] * move;
        }
    }

    // (Glass panes are baked: Tessera works out their connections, tessera:pane_<side>.)
    static bool connectionShaped(std::string const& block) {
        for (char const* end : {"_fence", "_wall", "_bars"})
            if (block.ends_with(end)) return true;
        return block == "iron_bars" || block == "chorus_plant";
    }

    // Blocks Java's BlockColors tints: only their faces with a tintindex take a colour.
    static bool javaTinted(std::string const& block) {
        if (block.find("grass") != std::string::npos || block.find("fern") != std::string::npos || block.ends_with("_leaves")
            || block.ends_with("_stem") || block.starts_with("attached_"))
            return true;
        static std::set<std::string> const names = {"vine", "sugar_cane", "lily_pad", "redstone_wire", "water", "bubble_column",
                                                    "water_cauldron", "leaf_litter", "wildflowers", "bush"};
        return names.contains(block);
    }

    static bool geometryExcluded(std::string const& block) {
        // Shapes made from neighbour connections Bedrock does not keep as states.
        // (Glass panes have theirs worked out by Tessera: tessera:pane_<side>.)
        for (char const* end : {"_stairs", "_fence", "_wall", "_bars"})
            if (block.ends_with(end)) return true;
        static std::set<std::string> const names = {"iron_bars", "chorus_plant", "vine", "glow_lichen", "sculk_vein", "scaffolding", "resin_clump"};
        return names.contains(block);
    }

    std::string emit(Image const& img, std::vector<Layer const*> const& from, std::string_view suffix = {}) {
        std::string const name = emit(img, suffix);
        auto&             src  = mResult.textureSources[name];
        for (auto const* l : from)
            if (std::find(src.begin(), src.end(), l->texName) == src.end()) src.push_back(l->texName);
        return name;
    }

    std::string emit(Image const& img, std::string_view suffix = {}) {
        uint64_t const h = hashImage(img) ^ (suffix.empty() ? 0 : 0x9E3779B97F4A7C15ull);
        if (auto it = mEmitted.find(h); it != mEmitted.end()) return it->second;
        std::string name = std::format("{}{:016x}{}", mOpt.texturePrefix, h, suffix);
        mResult.textures.emplace(name, img);
        mEmitted.emplace(h, name);
        return name;
    }

    std::map<std::string, std::string> const& facesFor(std::vector<Part> const& parts, Tints const& tints) {
        std::string key;
        for (auto const& p : parts) key += std::format("{}|{}|{};", p.model, p.rx, p.ry);
        for (auto const& [i, c] : tints) key += std::format("t{}={},{},{};", i, c[0], c[1], c[2]);
        if (mSideTintMask) key += "mask;";
        if (auto it = mFaceCache.find(key); it != mFaceCache.end()) return it->second;
        int const                          scale = bakeScale(parts);
        std::map<std::string, std::string> rep;
        for (auto const* face : kFaces) {
            auto const layers = collectLayers(parts, face, tints);
            if (layers.empty()) continue;
            std::vector<Layer const*> all;
            for (auto const& l : layers) all.push_back(&l);
            Image img = render(all, scale);
            if (!img.anyOpaque()) continue; // nothing drawn on this face: leave Bedrock's own
            // Grass block sides: Bedrock tints them through the texture's alpha (its own
            // grass_side marks the grass part); the parts the model tints become that mask.
            bool masked = false;
            if (mSideTintMask && std::string_view(face) != "up" && std::string_view(face) != "down") {
                std::vector<Layer const*> tinted;
                for (auto const* l : all)
                    if (l->tintIndex >= 0) tinted.push_back(l);
                if (!tinted.empty()) {
                    Image const mask = render(tinted, scale);
                    for (size_t i = 0; i < img.pixels.size() && i < mask.pixels.size(); ++i)
                        if (img.pixels[i] >> 24)
                            img.pixels[i] = (img.pixels[i] & 0x00FFFFFFu) | ((mask.pixels[i] >> 24) >= 128 ? 0xFF000000u : 0u);
                    masked = true;
                }
            }
            rep[std::string("*:") + face] = emit(img, all, masked ? "_grass" : "");
            emitPbr(rep[std::string("*:") + face], all, scale);
            // Group layers whose planes are within 0.03 of each other (overlays hug their face).
            std::vector<Layer const*> byPlane = all;
            std::stable_sort(byPlane.begin(), byPlane.end(), [](Layer const* a, Layer const* b) { return a->plane < b->plane; });
            std::vector<std::pair<double, std::vector<Layer const*>>> groups;
            for (auto const* l : byPlane) {
                if (!groups.empty() && l->plane - groups.back().first <= 0.03) groups.back().second.push_back(l);
                else groups.push_back({l->plane, {l}});
            }
            if (groups.size() > 1) // hollow block: inner faces get their own image
                for (auto& [start, ls] : groups) {
                    double lo = INFINITY, hi = -INFINITY;
                    for (auto const* l : ls) lo = std::min(lo, l->plane), hi = std::max(hi, l->plane);
                    Image const sub = render(ls, scale);
                    if (sub.anyOpaque()) {
                        auto const& name = rep[std::format("*:{}@{:.4f}", face, (lo + hi) / 2)] = emit(sub, ls);
                        emitPbr(name, ls, scale);
                    }
                }
        }
        return mFaceCache[key] = std::move(rep);
    }

    // The alternatives a state can come out as: Java picks one of a variant's list by block
    // position, weighted. Multipart files give one (their first "apply" stands for all).
    struct Choice {
        std::vector<Part> parts;
        int               weight = 1;
    };
    static std::vector<Choice> blockstateChoices(std::vector<Json> const& defs, StateMap const& state) {
        for (auto const& bs : defs) {
            auto variants = bs.find("variants");
            if (variants == bs.end() || !variants->is_object()) {
                auto parts = blockstateParts(bs, state);
                if (!parts.empty()) return {Choice{std::move(parts), 1}};
                if (bs.contains("multipart")) return {};
                continue;
            }
            for (auto const& [key, v] : variants->items()) {
                bool ok = true;
                for (auto const& [k, val] : parseProps(key))
                    if (auto it = state.find(k); it == state.end() || it->second != val) ok = false;
                if (!ok) continue;
                std::vector<Choice> out;
                auto add = [&](Json const& e) {
                    if (!e.is_object()) return;
                    Part const p{e.value("model", std::string{}), e.value("x", 0), e.value("y", 0)};
                    int const  w = std::max(1, e.value("weight", 1));
                    for (auto& c : out)
                        if (!(c.parts[0] < p) && !(p < c.parts[0])) { // the same again: one alternative, heavier
                            c.weight += w;
                            return;
                        }
                    if (out.size() < 8) out.push_back(Choice{{p}, w});
                };
                if (v.is_array()) {
                    for (auto const& e : v) add(e);
                } else add(v);
                return out;
            }
        }
        return {};
    }

    // Java merges a block's blockstate files across packs state by state: the highest pack
    // whose file covers the state decides (a multipart file covers every state).
    static std::vector<Part> blockstateParts(std::vector<Json> const& defs, StateMap const& state) {
        for (auto const& bs : defs) {
            auto parts = blockstateParts(bs, state);
            if (!parts.empty() || bs.contains("multipart")) return parts;
        }
        return {};
    }

    std::vector<Json> loadBlockstates(std::string const& block) const {
        std::string const path = "assets/minecraft/blockstates/" + block + ".json";
        std::vector<Json> out;
        for (auto const& text : mOpt.pack->readAll(path))
            if (auto j = parseJson(text); j && j->is_object()) out.push_back(std::move(*j));
        if (out.empty() && mOpt.vanilla)
            if (auto j = parseJson(mOpt.vanilla->read(path)); j && j->is_object()) out.push_back(std::move(*j));
        return out;
    }

    static std::vector<Part> blockstateParts(Json const& bs, StateMap const& state) {
        auto pick = [](Json const& v) {
            Json const& e = v.is_array() ? (v.empty() ? Json::object() : v[0]) : v;
            return Part{e.value("model", std::string{}), e.value("x", 0), e.value("y", 0)};
        };
        if (auto variants = bs.find("variants"); variants != bs.end() && variants->is_object()) {
            for (auto const& [key, v] : variants->items()) {
                bool ok = true;
                for (auto const& [k, val] : parseProps(key))
                    if (auto it = state.find(k); it == state.end() || it->second != val) ok = false;
                if (ok) return {pick(v)};
            }
            return {};
        }
        std::function<bool(Json const&)> matches = [&](Json const& when) -> bool {
            if (auto o = when.find("OR"); o != when.end()) {
                for (auto const& w : *o)
                    if (matches(w)) return true;
                return false;
            }
            if (auto a = when.find("AND"); a != when.end()) {
                for (auto const& w : *a)
                    if (!matches(w)) return false;
                return true;
            }
            for (auto const& [k, v] : when.items()) {
                auto        it  = state.find(k);
                auto const  opt = splitBar(whenText(v));
                if (it == state.end() || std::find(opt.begin(), opt.end(), it->second) == opt.end()) return false;
            }
            return true;
        };
        std::vector<Part> out;
        if (auto mp = bs.find("multipart"); mp != bs.end() && mp->is_array())
            for (auto const& p : *mp) {
                auto w = p.find("when");
                if (w == p.end() || matches(*w)) {
                    // "apply" may list several models (random choice in Java): the first stands for all.
                    if (auto a = p.find("apply"); a != p.end()) out.push_back(pick(*a));
                }
            }
        return out;
    }

    static std::set<std::string> blockstateProps(Json const& bs) {
        std::set<std::string> props;
        if (auto v = bs.find("variants"); v != bs.end() && v->is_object())
            for (auto const& [key, _] : v->items())
                for (auto const& [k, _2] : parseProps(key)) props.insert(k);
        std::function<void(Json const&)> walk = [&](Json const& when) {
            for (auto const& [k, v] : when.items()) {
                if (k == "OR" || k == "AND") {
                    for (auto const& w : v) walk(w);
                } else props.insert(k);
            }
        };
        if (auto mp = bs.find("multipart"); mp != bs.end() && mp->is_array())
            for (auto const& p : *mp)
                if (auto w = p.find("when"); w != p.end()) walk(*w);
        return props;
    }

    static std::vector<std::string> blockstateModels(Json const& bs) {
        std::vector<std::string> out;
        auto add = [&](Json const& v) {
            if (v.is_array()) {
                for (auto const& e : v) out.push_back(e.value("model", std::string{}));
            } else out.push_back(v.value("model", std::string{}));
        };
        if (auto v = bs.find("variants"); v != bs.end() && v->is_object())
            for (auto const& [_, x] : v->items()) add(x);
        if (auto mp = bs.find("multipart"); mp != bs.end() && mp->is_array())
            for (auto const& p : *mp)
                if (auto a = p.find("apply"); a != p.end()) add(*a);
        return out;
    }

    std::set<std::string> touchedBlocks() {
        auto names = [](Assets const& src, std::string const& dir, std::string const& ext) {
            std::set<std::string> out;
            for (auto const& path : src.list(dir))
                if (path.size() > dir.size() + ext.size() && path.ends_with(ext))
                    out.insert(path.substr(dir.size(), path.size() - dir.size() - ext.size()));
            return out;
        };
        std::set<std::string> touched;
        for (auto const& n : names(*mOpt.pack, "assets/minecraft/blockstates/", ".json"))
            if (n.find('/') == std::string::npos) touched.insert(n);
        if (!mOpt.vanilla) return touched;
        auto const packModels   = names(*mOpt.pack, "assets/minecraft/models/", ".json");
        auto const packTextures = names(*mOpt.pack, "assets/minecraft/textures/", ".png");
        auto       modelTouched = [&](std::string const& ref) {
            std::string const name = stripNs(ref);
            if (packModels.contains(name)) return true;
            std::shared_ptr<Model> m;
            try {
                m = loadModel(name);
            } catch (MissingModel const&) {
                return false;
            }
            for (auto const& [k, t] : m->textures)
                if (t.plain && packTextures.contains(stripNs(t.value))) return true;
            return false;
        };
        for (auto const& n : names(*mOpt.vanilla, "assets/minecraft/blockstates/", ".json")) {
            if (n.find('/') != std::string::npos || touched.contains(n)) continue;
            auto bs = parseJson(mOpt.vanilla->read("assets/minecraft/blockstates/" + n + ".json"));
            if (!bs || !bs->is_object()) continue;
            for (auto const& m : blockstateModels(*bs))
                if (modelTouched(m)) {
                    touched.insert(n);
                    break;
                }
        }
        return touched;
    }
};

BakeResult Baker::run() {
    auto const& table = mappings();
    Json        rules = Json::array();
    for (auto const& block : touchedBlocks()) {
        if (kSkipBlocks.contains(block)) continue;
        // Bars, fences, walls: their shape comes from neighbours Bedrock keeps no state
        // for, so a baked face can only guess it; and connected-texture packs draw them by their
        // own texture. They keep Bedrock's mesh and texture.
        if (connectionShaped(block)) continue;
        auto rowsIt = table.find(block);
        auto const defs = loadBlockstates(block);
        if (rowsIt == table.end() || defs.empty()) {
            mResult.problems.push_back(std::format("{}: no {}", block, rowsIt == table.end() ? "mapping" : "blockstate"));
            continue;
        }
        auto rows = rowsIt->second;
        if (rows.size() > 1) rows.erase(""); // the property-less row is only a fallback
        // Dust and string: the table has no rows for their connections (Bedrock keeps none);
        // every combination gets one, told apart by the connections Tessera works out.
        // Glass panes too (each side joined or not), for packs that give them models of
        // their own per connection (Fogg05's seamless glass).
        static std::map<std::string, std::vector<std::string>> const kLinks = {
            {"redstone_wire", {"none", "side", "up"}}, {"tripwire", {"false", "true"}}, {"_pane", {"false", "true"}}};
        auto lk = kLinks.find(block.ends_with("_pane") ? std::string("_pane") : block);
        if (lk != kLinks.end() && !rows.empty() && !parseProps(rows.begin()->first).contains("north")) {
            std::map<std::string, BedrockState> all;
            for (auto const& [jprops, row] : rows)
                for (auto const& n : lk->second)
                    for (auto const& e : lk->second)
                        for (auto const& s : lk->second)
                            for (auto const& w : lk->second)
                                all.emplace(std::format("{}{}east={},north={},south={},west={}", jprops, jprops.empty() ? "" : ",", e, n, s, w), row);
            rows = std::move(all);
        }
        std::set<std::string> used;
        for (auto const& d : defs) used.merge(blockstateProps(d));
        bool const leaves = block.ends_with("_leaves");
        std::map<std::string, std::string> bridge;
        if (auto b = kBridge.find(block); b != kBridge.end())
            for (auto const& [k, v] : b->second)
                if (used.contains(k)) bridge[k] = v;
        if (block.ends_with("_pane"))
            for (char const* side : {"north", "east", "south", "west"})
                if (used.contains(side)) bridge[side] = std::string("tessera:pane_") + side;
        if (block.ends_with("_leaves") && used.contains("distance")) bridge["distance"] = "tessera:distance";
        // Doors: Bedrock keeps "open" on the lower half and the hinge on the upper half only;
        // Tessera reads both halves and gives each half the pair (tessera:door_open/_hinge).
        bool const door = block.ends_with("_door") && !block.ends_with("trapdoor");
        if (door) {
            if (used.contains("open")) bridge["open"] = "tessera:door_open";
            if (used.contains("hinge")) bridge["hinge"] = "tessera:door_hinge";
        }
        auto const* agree = kAgree.contains(block) ? &kAgree.at(block) : nullptr;

        std::set<std::string> seen;
        int                   count = 0;
        for (auto const& [jprops, row] : rows) {
            StateMap state = parseProps(jprops);
            // Panes, bars, fences and walls: Bedrock keeps no connection states, so the table's
            // rows leave them out. Their faces are baked as if joined on every side (the full
            // pane / fence texture); Bedrock's own mesh then shows the right parts of it.
            if (geometryExcluded(block))
                for (char const* side : {"north", "east", "south", "west"})
                    if (used.contains(side) && !state.contains(side)) state[side] = block.ends_with("_wall") ? "low" : "true";
            std::map<std::string, Json> when;
            for (auto const& [k, v] : parseProps(row.states))
                // Leaves: update_bit only says a decay check is pending (it is set on the very
                // leaves that are about to decay), so it never picks a model.
                // String: suspended_bit has no Java counterpart (and is set on string lying on the ground).
                if (k != "waterlogged" && !(door && (k == "open_bit" || k == "door_hinge_bit")) && !(leaves && k == "update_bit")
                    && !(block == "tripwire" && k == "suspended_bit")
                    // Suspicious sand / gravel: Bedrock's "hanging" (no block below) has no Java
                    // counterpart and is true when placed by command.
                    && !(block.starts_with("suspicious_") && k == "hanging"))
                    when[k] = typed(v);
            for (auto const& [jk, bk] : bridge)
                if (auto it = state.find(jk); it != state.end()) when[bk] = typed(it->second);
            // The table keeps a lever's handle pointing the same way by swapping on/off for
            // two of the four facings; a pack shows power, so take the row where they agree.
            bool skip = false;
            if (agree)
                for (auto const& [jk, bk] : *agree) {
                    auto s = state.find(jk);
                    auto w = when.find(bk);
                    if (s != state.end() && w != when.end() && typed(s->second) != w->second) skip = true;
                }
            if (skip) continue;
            Json whenJson = Json::object();
            for (auto const& [k, v] : when) whenJson[k] = v;
            std::string const wkey = row.name + "|" + whenJson.dump();
            if (seen.contains(wkey)) continue; // several Java states share this Bedrock state: the first stands for all

            auto const choices = blockstateChoices(defs, state);
            if (choices.empty()) continue;
            int pickTotal = 0, pickAt = 0;
            for (auto const& c : choices) pickTotal += c.weight;
            bool failed = false;
            for (auto const& choice : choices) {
            int const pickFrom = pickAt;
            pickAt += choice.weight;
            std::map<std::string, std::string> rep;
            int                                rx = 0, ry = 0;
            Json                               geometry;
            bool                               cutout = false, translucent = false;
            try {
                auto const& parts = choice.parts;
                Tints       tints;
                if (block == "redstone_wire") {
                    auto p   = state.find("power");
                    tints[0] = dustRgb(p == state.end() ? 0 : std::stoi(p->second));
                }
                if (!geometryExcluded(block) && needsGeometry(parts)) {
                    geometry = geometryFor(parts, tints);
                    cutout   = mGeometryCutout;
                    translucent = mGeometryTranslucent;
                    if (!javaTinted(block))
                        for (auto& q : geometry) q.erase("ti");
                }
                mSideTintMask = block == "grass_block";
                if (geometry.is_array() && !geometry.empty()) {
                    // P5: drawn as its own shape; no face images needed
                } else if (parts.size() == 1) { // one model: bake it unrotated, let Tessera turn it
                    rx  = parts[0].rx;
                    ry  = parts[0].ry;
                    rep = facesFor({Part{parts[0].model, 0, 0}}, tints);
                } else {
                    rep = facesFor(parts, tints);
                }
            } catch (MissingModel const& e) {
                mResult.problems.push_back(std::format("{} [{}]: missing model {}", block, jprops, e.name));
                failed = true;
                break;
            }
            bool const hasGeometry = geometry.is_array() && !geometry.empty();
            if (hasGeometry) mShapedBlocks.insert(block);
            if (rep.empty() && !hasGeometry) continue;
            Json rule = Json::object();
            rule["block"] = row.name;
            if (!when.empty()) rule["when"] = whenJson;
            if (rx || ry) rule["rotation"] = Json{{"x", rx}, {"y", ry}};
            Json replace = Json::object();
            for (auto const& [k, v] : rep) replace[k] = v;
            rule["replace"] = replace;
            if (hasGeometry) {
                rule["geometry"] = std::move(geometry);
                if (cutout) rule["cutout"] = true;
                if (translucent) rule["translucent"] = true;
            }
            if (block == "redstone_wire") rule["tint"] = "none";
            if (choices.size() > 1) rule["pick"] = Json::array({pickFrom, pickAt, pickTotal});
            rules.push_back(std::move(rule));
            ++count;
            }
            if (failed) continue;
            seen.insert(wkey);
        }
        mResult.rulesPerBlock[block] = count;
    }
    // Block items whose Java item model has its own shape (a waxed block's outline, a 3D
    // item model): that shape is what the game draws when the block is shown as an item.
    {
        constexpr std::string_view dir = "assets/minecraft/items/";
        std::set<std::string>      own;
        auto add = [&](std::string const& path, std::optional<std::string> const& text, bool vanilla) {
            if (!text) return;
            std::string error;
            auto        def = itemdef::parse(path, *text, error);
            if (!def) return;
            auto rowsIt = table.find(def->item);
            if (rowsIt == table.end()) return; // not a block
            cit::Facts facts;
            facts.id = def->item;
            itemdef::Context ctx;
            ctx.facts = &facts;
            auto set  = itemdef::pick(*def, ctx);
            if (!set || set->size() != 1) return;
            std::string const model = stripNs(itemdef::splitSpec((*set)[0]).model);
            // Java's own definitions: when the pack changes how the block looks as an item (its
            // item model, the textures that model draws) or the block has its own shape in the
            // world (else the world shape of Bedrock's default state would stand in: a rod
            // pointing down, a button lying on the floor).
            if (vanilla && !packHas("assets/minecraft/models/" + model + ".json") && !itemInfo(model).touched
                && !mShapedBlocks.contains(def->item))
                return;
            Json geometry;
            Json gui;
            try {
                auto m = loadModel(model);
                if (!m->elements || m->elements->empty()) return;
                // A plain full cube (stained glass, a re-declared cube_all) needs no shape of its
                // own: the game draws it as the block, with the block's textures and its see-through
                // look in slots and hands.
                if (!needsGeometry({Part{model, 0, 0}})) return;
                // The definition's own tint colours (leaves, grass...), drawn into the faces they tint.
                Tints tintMap;
                auto const defTints = itemdef::splitSpec((*set)[0]).tints;
                for (size_t i = 0; i < defTints.size(); ++i)
                    tintMap[int(i)] = {int((defTints[i] >> 16) & 0xFF), int((defTints[i] >> 8) & 0xFF), int(defTints[i] & 0xFF)};
                geometry = geometryFor({Part{model, 0, 0}}, tintMap, true);
                if (m->gui) gui = Json(std::vector<double>(m->gui->begin(), m->gui->end()));

            } catch (MissingModel const&) {
                return;
            }
            if (!geometry.is_array() || geometry.empty()) return;
            for (auto& q : geometry) q.erase("ti"); // tints are in the images
            std::set<std::string> names;
            for (auto const& [jprops, row] : rowsIt->second) names.insert(row.name);
            for (auto const& name : names) {
                Json rule        = Json::object();
                rule["block"]    = name;
                rule["item"]     = true;
                rule["replace"]  = Json::object();
                rule["geometry"] = geometry;
                if (!gui.is_null()) rule["gui"] = gui;
                if (mGeometryCutout) rule["cutout"] = true;
                if (mGeometryTranslucent) rule["translucent"] = true;
                rules.push_back(std::move(rule));
                ++mResult.itemShapes;
            }
        };
        for (auto const& path : mOpt.pack->list(std::string(dir))) {
            if (!path.ends_with(".json") || path.find('/', dir.size()) != std::string::npos) continue;
            own.insert(path);
            add(path, mOpt.pack->read(path), false);
        }
        if (mOpt.vanilla)
            for (auto const& path : mOpt.vanilla->list(std::string(dir)))
                if (path.ends_with(".json") && path.find('/', dir.size()) == std::string::npos && !own.contains(path))
                    add(path, mOpt.vanilla->read(path), true);
    }
    mResult.ruleCount = rules.size();
    mResult.rulesJson = Json{{"format_version", 1}, {"rules", rules}}.dump();
    return std::move(mResult);
}

} // namespace

std::string bakerVersion() { return std::string("java-bake ") + __DATE__ + " " + __TIME__; }

struct ItemIcons::Impl {
    explicit Impl(BakeOptions const& options) : baker(options) {}
    Baker baker;
};
ItemIcons::ItemIcons(BakeOptions const& options) : mImpl(options.pack ? std::make_unique<Impl>(options) : nullptr) {}
ItemIcons::~ItemIcons() = default;
ItemIcons::Info ItemIcons::info(std::string const& model) { return mImpl ? mImpl->baker.itemInfo(model) : Info{}; }
std::optional<Image> ItemIcons::icon(std::string const& model, std::vector<uint32_t> const& tints, bool flatOnly) {
    return mImpl ? mImpl->baker.itemIcon(model, flatOnly, tints) : std::nullopt;
}

std::optional<std::pair<Image, Image>> ItemIcons::flatPbr(std::string const& model) {
    return mImpl ? mImpl->baker.flatPbr(model) : std::nullopt;
}

std::optional<HandMesh> ItemIcons::handMesh(std::string const& model, std::vector<uint32_t> const& tints) {
    return mImpl ? mImpl->baker.handMesh(model, tints) : std::nullopt;
}

std::optional<Image> renderItemIcon(BakeOptions const& options, std::string const& model, bool flatOnly) {
    if (!options.pack) return std::nullopt;
    Baker baker(options);
    return baker.itemIcon(model, flatOnly);
}

std::optional<Image> loadTexture(BakeOptions const& options, std::string const& pathOrName) {
    if (!options.pack) return std::nullopt;
    Baker baker(options);
    return baker.texture(pathOrName);
}

std::optional<std::string> blockSkipReason(std::string const& javaBlock) { return Baker::skipReason(javaBlock); }

BakeResult bake(BakeOptions const& options) {
    if (!options.pack) throw std::invalid_argument("bake: no pack");
    Baker baker(options);
    return baker.run();
}

} // namespace tessera::java
