// tessera-tool: for resource pack authors (no game needed).
//
//   tessera-tool init    <Bedrock pack dir>
//       Adds the Java-format folders Tessera reads (assets/minecraft/...) to a Bedrock pack.
//   tessera-tool check   <pack> [-o <dir>] [--vanilla <client.jar>|off]
//       What Tessera will make of the pack: rules per block, what could not be baked and why,
//       missing models / textures, connected-texture and CIT files it cannot use, item
//       definitions, the pack's Bedrock side, and what Tessera does not read. <dir>/report.html.
//   tessera-tool preview <pack> [-o <dir>] [--vanilla ...] [--block <name>] [--max <n>]
//       Every block state the pack changes as Tessera shows it, sample walls for its connected
//       textures, and the item icons: <dir>/preview.html (+ img/).
//   tessera-tool pack    <pack dir> [-o <file.mcpack>] [--name <name>]
//       Zips the pack as an .mcpack, adding manifest.json (new UUIDs) and pack_icon.png when missing.
//
// All commands take --lang en | zh-Hans | zh-Hant (default: the system's language).
// <pack> is a folder or a .zip / .mcpack; its files may sit inside one top folder.
// The vanilla Java assets come from the player's own client jar (newest under
// %APPDATA%\.minecraft\versions unless --vanilla says otherwise); nothing of it is copied.
#include "core/ctm/Ctm.h"
#include "core/item/Cit.h"
#include "core/item/ItemDef.h"
#include "core/java/JavaPack.h"
#include "core/java/ZipAssets.h"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace tessera;
using Json = nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------

enum class Lang { En, Hans, Hant };
Lang gLang = Lang::En;

// The text in the chosen language (Traditional Chinese, Simplified Chinese, English).
char const* T(char const* hant, char const* hans, char const* en) { return gLang == Lang::Hant ? hant : gLang == Lang::Hans ? hans : en; }

template <class... A>
std::string F(std::string_view fmt, A const&... args) {
    return std::vformat(fmt, std::make_format_args(args...));
}

Lang systemLang() {
#ifdef _WIN32
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) {
        std::wstring const n = name;
        if (n.starts_with(L"zh-TW") || n.starts_with(L"zh-HK") || n.starts_with(L"zh-MO") || n.starts_with(L"zh-Hant")) return Lang::Hant;
        if (n.starts_with(L"zh")) return Lang::Hans;
    }
#endif
    return Lang::En;
}

char const* htmlLang() { return T("zh-Hant", "zh-Hans", "en"); }

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

std::string u8(fs::path const& p) {
    auto s = p.generic_u8string();
    return {s.begin(), s.end()};
}
fs::path pathOf(std::string const& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::optional<std::string> readFile(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool writeFile(fs::path const& p, std::string const& data) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out << data;
    return bool(out);
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string html(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    return out;
}

uint64_t fnv(std::string const& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

// ---------------------------------------------------------------------------
// Pack sources
// ---------------------------------------------------------------------------

class DirAssets : public java::Assets {
public:
    explicit DirAssets(fs::path root) : mRoot(std::move(root)) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(mRoot, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (!ec && it->is_regular_file()) mFiles.push_back(u8(fs::relative(it->path(), mRoot)));
        std::sort(mFiles.begin(), mFiles.end());
    }
    std::optional<std::string> read(std::string const& path) const override { return readFile(mRoot / pathOf(path)); }
    std::vector<std::string>   list(std::string const& prefix) const override {
        std::vector<std::string> out;
        for (auto it = std::lower_bound(mFiles.begin(), mFiles.end(), prefix); it != mFiles.end() && it->starts_with(prefix); ++it)
            out.push_back(*it);
        return out;
    }

private:
    fs::path                 mRoot;
    std::vector<std::string> mFiles;
};

// A zip whose pack files sit under a folder inside it.
class RootedAssets : public java::Assets {
public:
    RootedAssets(std::unique_ptr<java::Assets> inner, std::string root) : mInner(std::move(inner)), mRoot(std::move(root)) {}
    std::optional<std::string> read(std::string const& path) const override { return mInner->read(mRoot + path); }
    std::vector<std::string>   list(std::string const& prefix) const override {
        auto out = mInner->list(mRoot + prefix);
        for (auto& p : out) p = p.substr(mRoot.size());
        return out;
    }

private:
    std::unique_ptr<java::Assets> mInner;
    std::string                   mRoot;
};

// Remembers the texture files asked for and not found (a texture missing everywhere is
// asked of the pack, then of the vanilla jar).
class Recorder : public java::Assets {
public:
    explicit Recorder(java::Assets const& inner) : mInner(inner) {}
    std::optional<std::string> read(std::string const& path) const override {
        auto r = mInner.read(path);
        if (!r && path.starts_with("assets/minecraft/textures/") && path.ends_with(".png")) {
            std::lock_guard lock(mMutex);
            mMissed.insert(path);
        }
        return r;
    }
    std::vector<std::string> readAll(std::string const& path) const override { return mInner.readAll(path); }
    std::vector<std::string> list(std::string const& prefix) const override { return mInner.list(prefix); }
    std::set<std::string>    missed() const {
        std::lock_guard lock(mMutex);
        return mMissed;
    }

private:
    java::Assets const&           mInner;
    mutable std::mutex            mMutex;
    mutable std::set<std::string> mMissed;
};

struct Pack {
    std::unique_ptr<java::Assets> assets;
    std::string                   label; // file or folder name
};

bool isPackRoot(fs::path const& d) {
    std::error_code ec;
    return fs::exists(d / "assets", ec) || fs::exists(d / "manifest.json", ec);
}

std::optional<Pack> openPack(fs::path const& path, std::string& error) {
    Pack pack;
    pack.label = u8(path.filename());
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        fs::path root = path;
        if (!isPackRoot(root))
            for (auto const& e : fs::directory_iterator(path, ec))
                if (e.is_directory() && isPackRoot(e.path())) {
                    root = e.path();
                    break;
                }
        if (!isPackRoot(root)) {
            error = F(T("{} 裡沒有 assets/ 或 manifest.json", "{} 里没有 assets/ 或 manifest.json", "no assets/ or manifest.json in {}"), u8(path));
            return std::nullopt;
        }
        pack.assets = std::make_unique<DirAssets>(root);
        return pack;
    }
    if (!fs::is_regular_file(path, ec)) {
        error = F(T("找不到：{}", "找不到：{}", "not found: {}"), u8(path));
        return std::nullopt;
    }
    auto zip = std::make_unique<java::ZipAssets>(path, "");
    if (!zip->ok()) {
        error = F(T("不是 zip 檔：{}", "不是 zip 文件：{}", "not a zip file: {}"), u8(path));
        return std::nullopt;
    }
    std::string root;
    bool        found = false;
    for (auto const& name : zip->list("")) {
        size_t at = name.find("assets/minecraft/");
        if (at == std::string::npos && name.ends_with("manifest.json")) at = name.size() - 13;
        if (at != std::string::npos && (at == 0 || name[at - 1] == '/') && (!found || at < root.size())) {
            root  = name.substr(0, at);
            found = true;
        }
    }
    if (!found) {
        error = F(T("{} 裡沒有 assets/ 或 manifest.json", "{} 里没有 assets/ 或 manifest.json", "no assets/ or manifest.json in {}"), u8(path));
        return std::nullopt;
    }
    pack.assets = root.empty() ? std::unique_ptr<java::Assets>(std::move(zip)) : std::make_unique<RootedAssets>(std::move(zip), root);
    return pack;
}

fs::path appData() {
#ifdef _WIN32
    if (char const* a = std::getenv("APPDATA")) return fs::path(a);
#endif
    if (char const* h = std::getenv("HOME")) return fs::path(h);
    return {};
}

std::unique_ptr<java::ZipAssets> openVanilla(std::string const& choice, std::string& note) {
    if (choice == "off") {
        note = T("沒有使用原版 Java 資源（--vanilla off）", "没有使用原版 Java 资源（--vanilla off）", "vanilla Java assets not used (--vanilla off)");
        return nullptr;
    }
    fs::path jar = choice.empty() ? java::findJavaClientJar(appData() / ".minecraft") : pathOf(choice);
    if (jar.empty()) {
        note = T("找不到 Java 版客戶端 jar（%APPDATA%\\.minecraft\\versions）：只檢查材質包自己的檔案，缺的原版模型與貼圖會被當成缺少",
                 "找不到 Java 版客户端 jar（%APPDATA%\\.minecraft\\versions）：只检查资源包自己的文件，缺的原版模型与贴图会被当成缺少",
                 "no Java client jar found (%APPDATA%\\.minecraft\\versions): only the pack's own files are checked, vanilla models "
                 "and textures it relies on show up as missing");
        return nullptr;
    }
    auto z = std::make_unique<java::ZipAssets>(jar);
    if (!z->ok()) {
        note = F(T("無法讀取 {}", "无法读取 {}", "cannot read {}"), u8(jar));
        return nullptr;
    }
    note = F(T("原版 Java 資源：{}（只讀取，不會複製）", "原版 Java 资源：{}（只读取，不会复制）", "vanilla Java assets: {} (read only, nothing copied)"),
             u8(jar.filename()));
    return z;
}

// The pack's own Bedrock texture for a Java name (as the game does: a Bedrock pack's pictures
// win over the vanilla Java ones when the pack has no Java texture of that name).
std::optional<java::Image> packBedrockTexture(java::Assets const& pack, std::string const& javaName) {
    std::string const dir = javaName.starts_with("item/") ? "textures/items/" : "textures/blocks/";
    for (auto const& name : java::bedrockTextureNames(javaName))
        for (char const* ext : {".png", ".tga"}) {
            if (std::string_view(ext) == ".tga") continue; // only PNG is decoded here
            if (auto bytes = pack.read(dir + name + ext))
                if (auto img = java::decodePng(*bytes)) return img;
        }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

struct Args {
    std::string                        command;
    std::vector<std::string>           positional;
    std::map<std::string, std::string> options;
    std::string get(std::string const& k, std::string const& def = {}) const {
        auto it = options.find(k);
        return it == options.end() ? def : it->second;
    }
};

Args parseArgs(std::vector<std::string> const& argv) {
    Args a;
    for (size_t i = 0; i < argv.size(); ++i) {
        std::string const& s = argv[i];
        if (s.starts_with("--") || (s.size() == 2 && s[0] == '-')) {
            std::string key = s.starts_with("--") ? s.substr(2) : s.substr(1);
            if (key == "o") key = "out";
            a.options[key] = i + 1 < argv.size() ? argv[++i] : "";
        } else if (a.command.empty()) a.command = s;
        else a.positional.push_back(s);
    }
    return a;
}

// ---------------------------------------------------------------------------
// Baking (shared by check and preview)
// ---------------------------------------------------------------------------

struct Baked {
    java::BakeResult      result;
    std::set<std::string> missingTextures; // Java names ("block/foo")
    std::set<std::string> fromBedrock;     // Java names drawn from the pack's Bedrock textures
    long long             ms = 0;
};

Baked bakePack(java::Assets const& pack, java::Assets const* vanilla) {
    Recorder                packRec(pack);
    std::optional<Recorder> vanillaRec;
    if (vanilla) vanillaRec.emplace(*vanilla);
    Baked             b;
    std::mutex        m;
    java::BakeOptions options;
    options.pack               = &packRec;
    options.vanilla            = vanillaRec ? &*vanillaRec : nullptr;
    options.packBedrockTexture = [&](std::string const& name) {
        auto img = packBedrockTexture(pack, name);
        if (img) {
            std::lock_guard lock(m);
            b.fromBedrock.insert(name);
        }
        return img;
    };
    auto const start = std::chrono::steady_clock::now();
    b.result         = java::bake(options);
    b.ms             = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    for (auto const& p : (vanillaRec ? vanillaRec->missed() : packRec.missed())) {
        std::string name = p.substr(std::string("assets/minecraft/textures/").size());
        name.resize(name.size() - 4);
        if (!b.fromBedrock.contains(name)) b.missingTextures.insert(name);
    }
    return b;
}

// ---------------------------------------------------------------------------
// Software preview renderer (isometric, painter's algorithm)
// ---------------------------------------------------------------------------

struct Quad {
    java::Image const*    tex = nullptr;
    std::array<float, 12> p{};  // corners (block space 0..1)
    std::array<float, 8>  uv{}; // corners (0..1 of the texture)
    int                   dir  = -1;
    uint32_t              tint = 0xFFFFFF;
};

constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

// Java's view of a block: from the south-east, above. Visible: up, south, east.
void project(float x, float y, float z, float size, float& sx, float& sy) {
    float const cx = x - 0.5f, cy = y - 0.5f, cz = z - 0.5f;
    sx = size * 0.5f + (cx - cz) * 0.866f * size * 0.40f;
    sy = size * 0.52f + ((cx + cz) * 0.5f - cy) * size * 0.40f;
}

java::Image render(std::vector<Quad> quads, int size) {
    java::Image img;
    img.width = img.height = size;
    img.pixels.assign(size_t(size) * size_t(size), 0);
    // Faces turned away from the viewer are not seen.
    std::erase_if(quads, [](Quad const& q) {
        if (q.dir < 0 || q.dir > 5) return false;
        return kDir[q.dir][0] + kDir[q.dir][1] + kDir[q.dir][2] < 0;
    });
    auto centre = [](Quad const& q) {
        return (q.p[0] + q.p[3] + q.p[6] + q.p[9]) + (q.p[1] + q.p[4] + q.p[7] + q.p[10]) + (q.p[2] + q.p[5] + q.p[8] + q.p[11]);
    };
    std::stable_sort(quads.begin(), quads.end(), [&](Quad const& a, Quad const& b) { return centre(a) < centre(b); });
    static constexpr float kShade[6] = {0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f};
    for (auto const& q : quads) {
        if (!q.tex || q.tex->empty()) continue;
        float sx[4], sy[4];
        for (int k = 0; k < 4; ++k) project(q.p[size_t(k * 3)], q.p[size_t(k * 3 + 1)], q.p[size_t(k * 3 + 2)], float(size), sx[k], sy[k]);
        float const shade = q.dir >= 0 && q.dir < 6 ? kShade[q.dir] : 0.8f;
        for (auto const& tri : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
            float const x0 = sx[tri[0]], y0 = sy[tri[0]], x1 = sx[tri[1]], y1 = sy[tri[1]], x2 = sx[tri[2]], y2 = sy[tri[2]];
            float const area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::abs(area) < 1e-6f) continue;
            int const minX = std::max(0, int(std::floor(std::min({x0, x1, x2}))));
            int const maxX = std::min(size - 1, int(std::ceil(std::max({x0, x1, x2}))));
            int const minY = std::max(0, int(std::floor(std::min({y0, y1, y2}))));
            int const maxY = std::min(size - 1, int(std::ceil(std::max({y0, y1, y2}))));
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x) {
                    float const px = float(x) + 0.5f, py = float(y) + 0.5f;
                    float const w1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / area;
                    float const w2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / area;
                    float const w0 = 1 - w1 - w2;
                    if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                    float const u  = w0 * q.uv[size_t(tri[0] * 2)] + w1 * q.uv[size_t(tri[1] * 2)] + w2 * q.uv[size_t(tri[2] * 2)];
                    float const v  = w0 * q.uv[size_t(tri[0] * 2 + 1)] + w1 * q.uv[size_t(tri[1] * 2 + 1)] + w2 * q.uv[size_t(tri[2] * 2 + 1)];
                    int const   tx = std::clamp(int(u * float(q.tex->width)), 0, q.tex->width - 1);
                    int const   ty = std::clamp(int(v * float(q.tex->height)), 0, q.tex->height - 1);
                    uint32_t const t = q.tex->at(tx, ty);
                    uint32_t const a = t >> 24;
                    if (a < 8) continue;
                    auto ch = [&](int sh, int tsh) { return float((t >> sh) & 0xFF) * shade * float((q.tint >> tsh) & 0xFF) / 255.f; };
                    float const r = ch(0, 16), g = ch(8, 8), bl = ch(16, 0);
                    uint32_t&   d  = img.pixels[size_t(y) * size_t(size) + size_t(x)];
                    float const fa = float(a) / 255.f, da = float(d >> 24) / 255.f;
                    float const oa = fa + da * (1 - fa);
                    auto mix = [&](float c, int sh) {
                        float const dc = float((d >> sh) & 0xFF);
                        return uint32_t(std::clamp((c * fa + dc * da * (1 - fa)) / std::max(oa, 1e-6f), 0.f, 255.f));
                    };
                    d = mix(r, 0) | (mix(g, 8) << 8) | (mix(bl, 16) << 16) | (uint32_t(oa * 255.f + 0.5f) << 24);
                }
        }
    }
    return img;
}

// A full block face (0..5) as a quad, with the baked face picture drawn the way Java maps it.
Quad cubeFace(int dir, java::Image const* tex) {
    Quad q;
    q.tex = tex;
    q.dir = dir;
    // Corners from the picture's top-left, clockwise.
    std::array<std::array<float, 3>, 4> c;
    switch (dir) {
    case 1: c = {{{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}}; break;  // up: top = north
    case 0: c = {{{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}}}; break;  // down: top = south
    case 2: c = {{{1, 1, 0}, {0, 1, 0}, {0, 0, 0}, {1, 0, 0}}}; break;  // north
    case 3: c = {{{0, 1, 1}, {1, 1, 1}, {1, 0, 1}, {0, 0, 1}}}; break;  // south
    case 4: c = {{{0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {0, 0, 0}}}; break;  // west
    default: c = {{{1, 1, 1}, {1, 1, 0}, {1, 0, 0}, {1, 0, 1}}}; break; // east
    }
    static constexpr float uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (size_t k = 0; k < 4; ++k) {
        for (size_t a = 0; a < 3; ++a) q.p[k * 3 + a] = c[k][a];
        q.uv[k * 2]     = uv[k * 2];
        q.uv[k * 2 + 1] = uv[k * 2 + 1];
    }
    return q;
}

// World direction -> the model direction a rotated rule's "*:<face>" names (Java x then y).
int unrotate(int dir, int rx, int ry) {
    int v[3] = {kDir[dir][0], kDir[dir][1], kDir[dir][2]};
    for (int i = 0; i < ry; ++i) {
        int const x = v[0];
        v[0] = v[2], v[2] = -x;
    }
    for (int i = 0; i < rx; ++i) {
        int const y = v[1];
        v[1] = -v[2], v[2] = y;
    }
    for (int d = 0; d < 6; ++d)
        if (kDir[d][0] == v[0] && kDir[d][1] == v[1] && kDir[d][2] == v[2]) return d;
    return dir;
}

char const* kFaceName[6] = {"down", "up", "north", "south", "west", "east"};

std::string baseName(std::string s) {
    if (auto h = s.find('#'); h != std::string::npos) s.resize(h);
    return s;
}

// The quads a rule draws (nullopt: nothing shown here).
std::optional<std::vector<Quad>> ruleQuads(Json const& rule, std::map<std::string, java::Image> const& textures) {
    auto texOf = [&](std::string const& name) -> java::Image const* {
        auto it = textures.find(baseName(name));
        return it == textures.end() ? nullptr : &it->second;
    };
    std::vector<Quad> quads;
    if (auto g = rule.find("geometry"); g != rule.end() && g->is_array()) {
        for (auto const& jq : *g) {
            Quad q;
            q.tex         = texOf(jq.value("t", std::string{}));
            auto const& p = jq["p"];
            auto const& u = jq["uv"];
            if (!p.is_array() || p.size() != 12 || !u.is_array() || u.size() != 8) continue;
            for (size_t i = 0; i < 12; ++i) q.p[i] = p[i].get<float>();
            for (size_t i = 0; i < 8; ++i) q.uv[i] = u[i].get<float>();
            q.dir = jq.value("d", -1);
            if (int const tc = jq.value("tc", -1); tc >= 0) q.tint = uint32_t(tc);
            quads.push_back(q);
        }
        return quads;
    }
    auto rep = rule.find("replace");
    if (rep == rule.end() || !rep->is_object() || rep->empty()) return std::nullopt;
    int rx = 0, ry = 0;
    if (auto r = rule.find("rotation"); r != rule.end() && r->is_object()) {
        rx = ((r->value("x", 0) / 90) % 4 + 4) % 4;
        ry = ((r->value("y", 0) / 90) % 4 + 4) % 4;
    }
    for (int d = 0; d < 6; ++d) {
        std::string const  want = std::string("*:") + kFaceName[unrotate(d, rx, ry)];
        java::Image const* tex  = nullptr;
        if (auto it = rep->find(want); it != rep->end() && it->is_string()) tex = texOf(it->get<std::string>());
        else // only per-plane pictures: the outermost one stands for the face
            for (auto const& [k, v] : rep->items())
                if (k.starts_with(want + "@") && v.is_string()) tex = texOf(v.get<std::string>());
        if (tex) quads.push_back(cubeFace(d, tex));
    }
    if (quads.empty()) return std::nullopt;
    return quads;
}

std::string whenText(Json const& rule) {
    std::string s;
    if (auto w = rule.find("when"); w != rule.end() && w->is_object())
        for (auto const& [k, v] : w->items()) {
            if (!s.empty()) s += ", ";
            std::string key = k;
            if (key.starts_with("minecraft:")) key = key.substr(10);
            s += key + "=" + (v.is_string() ? v.get<std::string>() : v.dump());
        }
    if (rule.value("item", false)) s = s.empty() ? T("物品形態", "物品形态", "as an item") : T("物品形態, ", "物品形态, ", "as an item, ") + s;
    if (auto p = rule.find("pick"); p != rule.end() && p->is_array() && p->size() == 3)
        s += F(T("{}隨機外觀 {}/{}", "{}随机外观 {}/{}", "{}random look {}/{}"), s.empty() ? "" : ", ", (*p)[1].get<int>() + 1, (*p)[2].get<int>());
    return s.empty() ? T("所有狀態", "所有状态", "every state") : s;
}

// ---------------------------------------------------------------------------
// Connected-texture sample walls
// ---------------------------------------------------------------------------

// A wall of blocks with a gap and ragged edges, so every kind of connection shows.
constexpr char const* kWall[4] = {"####.", "#.##.", "#####", "..###"};
constexpr int         kWallW = 5, kWallH = 4;

std::optional<java::Image> ctmWall(ctm::Rule const& rule, java::Assets const& pack) {
    std::vector<std::optional<java::Image>> tiles;
    for (auto const& t : rule.tiles) {
        std::optional<java::Image> img;
        if (t.kind == ctm::Tile::Kind::Image)
            if (auto bytes = pack.read(t.path)) img = java::decodePng(*bytes);
        tiles.push_back(std::move(img));
    }
    auto tileAt = [&](int i) -> java::Image const* { return i >= 0 && size_t(i) < tiles.size() && tiles[size_t(i)] ? &*tiles[size_t(i)] : nullptr; };
    constexpr int cell = 32;
    java::Image   out;
    out.width  = kWallW * cell;
    out.height = kWallH * cell;
    out.pixels.assign(size_t(out.width) * size_t(out.height), 0);
    auto solid = [](int x, int y) { return x >= 0 && y >= 0 && x < kWallW && y < kWallH && kWall[y][x] == '#'; };
    auto blit  = [&](java::Image const& t, int cx, int cy, ctm::Range r) {
        int const x0 = int(r.u0 * cell), x1 = int(r.u1 * cell), y0 = int(r.v0 * cell), y1 = int(r.v1 * cell);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                int const      tx = std::clamp(x * t.width / cell, 0, t.width - 1), ty = std::clamp(y * t.height / cell, 0, t.height - 1);
                uint32_t const p  = t.at(tx, ty);
                if ((p >> 24) == 0) continue;
                out.pixels[size_t(cy * cell + y) * size_t(out.width) + size_t(cx * cell + x)] = p;
            }
    };
    bool drew = false;
    for (int y = 0; y < kWallH; ++y)
        for (int x = 0; x < kWallW; ++x) {
            if (!solid(x, y)) continue;
            switch (rule.method) {
            case ctm::Method::Compact: {
                uint8_t nb = 0;
                if (solid(x - 1, y)) nb |= ctm::Left;
                if (solid(x + 1, y)) nb |= ctm::Right;
                if (solid(x, y - 1)) nb |= ctm::Up;
                if (solid(x, y + 1)) nb |= ctm::Down;
                if (solid(x - 1, y - 1)) nb |= ctm::UpLeft;
                if (solid(x + 1, y - 1)) nb |= ctm::UpRight;
                if (solid(x - 1, y + 1)) nb |= ctm::DownLeft;
                if (solid(x + 1, y + 1)) nb |= ctm::DownRight;
                auto const q = ctm::compactQuarters(nb);
                for (size_t i = 0; i < 4; ++i)
                    if (auto const* t = tileAt(q[i])) blit(*t, x, y, ctm::kQuarters[i]), drew = true;
                break;
            }
            case ctm::Method::Fixed:
                if (auto const* t = tileAt(0)) blit(*t, x, y, {0, 0, 1, 1}), drew = true;
                break;
            case ctm::Method::Random:
                if (auto const* t = tileAt(ctm::randomTile(rule, x, -y, 0, ctm::Dir::South))) blit(*t, x, y, {0, 0, 1, 1}), drew = true;
                break;
            case ctm::Method::Repeat:
                if (auto const* t = tileAt(ctm::repeatTile(rule, x, y))) blit(*t, x, y, {0, 0, 1, 1}), drew = true;
                break;
            case ctm::Method::Top:
                if (solid(x, y - 1))
                    if (auto const* t = tileAt(0)) blit(*t, x, y, {0, 0, 1, 1}), drew = true;
                break;
            default: break;
            }
        }
    if (!drew) return std::nullopt;
    return out;
}

// ---------------------------------------------------------------------------
// HTML page
// ---------------------------------------------------------------------------

std::string pageHead(std::string const& title) {
    return std::format(R"(<!doctype html><html lang="{}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>{}</title><style>
:root{{--bg:#f6f5f2;--fg:#1f2328;--muted:#656d76;--card:#fff;--line:#d8dee4;--warn:#9a6700;--bad:#cf222e;--ok:#1a7f37;--chip:#eef1f4}}
@media (prefers-color-scheme:dark){{:root{{--bg:#14171b;--fg:#e6edf3;--muted:#9198a1;--card:#1c2128;--line:#30363d;--warn:#d29922;--bad:#f85149;--ok:#3fb950;--chip:#262c34}}}}
body{{margin:0;background:var(--bg);color:var(--fg);font:15px/1.55 system-ui,"Microsoft JhengHei","Microsoft YaHei",sans-serif}}
main{{max-width:1100px;margin:0 auto;padding:24px 16px 64px}} h1{{font-size:24px;margin:0 0 4px}} h2{{font-size:18px;margin:32px 0 8px;border-bottom:1px solid var(--line);padding-bottom:4px}}
.muted{{color:var(--muted)}} .stats{{display:flex;flex-wrap:wrap;gap:8px}} .stat{{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px 12px;min-width:120px}} .stat b{{display:block;font-size:20px}}
table{{border-collapse:collapse;width:100%;font-size:14px}} td,th{{border-bottom:1px solid var(--line);padding:4px 6px;text-align:left;vertical-align:top}} th{{color:var(--muted);font-weight:600}}
code{{background:var(--chip);border-radius:4px;padding:0 4px;font-size:13px;word-break:break-all}}
.bad{{color:var(--bad)}} .warn{{color:var(--warn)}} .ok{{color:var(--ok)}} details{{margin:4px 0}} summary{{cursor:pointer}}
.grid{{display:flex;flex-wrap:wrap;gap:6px}} .tile{{width:104px;font-size:11px;color:var(--muted);text-align:center;word-break:break-all}}
.tile img{{width:96px;height:96px;image-rendering:pixelated;background:repeating-conic-gradient(var(--chip) 0 25%,transparent 0 50%) 0 0/16px 16px;border-radius:6px;display:block;margin:0 auto 2px}}
.wall{{font-size:12px;color:var(--muted);word-break:break-all;max-width:240px}} .wall img{{width:160px;height:128px;image-rendering:pixelated;background:repeating-conic-gradient(var(--chip) 0 25%,transparent 0 50%) 0 0/16px 16px;border-radius:6px;display:block}}
input[type=search]{{width:100%;max-width:360px;padding:6px 8px;border:1px solid var(--line);border-radius:6px;background:var(--card);color:var(--fg)}}
</style></head><body><main>)",
                       htmlLang(), html(title));
}

std::string pageTail() { return "</main></body></html>\n"; }

// ---------------------------------------------------------------------------
// check
// ---------------------------------------------------------------------------

struct Finding {
    std::string where, what;
};

struct Section {
    std::string          title, intro, level; // level: bad / warn / ok / muted
    std::vector<Finding> items;
};

// Text of a JSON text component ("…", {"text": …, "extra": […]}, […]), without § codes.
std::string plainText(Json const& t) {
    std::string s;
    if (t.is_string()) s = t.get<std::string>();
    else if (t.is_array())
        for (auto const& e : t) s += plainText(e);
    else if (t.is_object()) {
        if (auto x = t.find("text"); x != t.end()) s += plainText(*x);
        if (auto x = t.find("extra"); x != t.end()) s += plainText(*x);
    }
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 2, "\xC2\xA7") == 0) { // § and its code letter
            i += 2;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::string packTitle(java::Assets const& pack, std::string const& fallback) {
    if (auto m = pack.read("manifest.json")) try {
            auto j = Json::parse(*m, nullptr, true, true);
            if (auto n = plainText(j["header"]["name"]); !n.empty()) return n;
        } catch (...) {}
    // (pack.mcmeta's description is often a slogan or a link: the file name names the pack better.)
    return fallback;
}

size_t countFiles(java::Assets const& A, std::string const& prefix, std::string const& ext) {
    size_t n = 0;
    for (auto const& p : A.list(prefix))
        if (p.ends_with(ext)) ++n;
    return n;
}

int cmdCheck(Args const& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: tessera-tool check <pack> [-o <dir>] [--vanilla <jar>|off] [--lang en|zh-Hans|zh-Hant]\n";
        return 2;
    }
    std::string error;
    auto        pack = openPack(pathOf(args.positional[0]), error);
    if (!pack) {
        std::cerr << error << "\n";
        return 1;
    }
    std::string vanillaNote;
    auto        vanilla = openVanilla(args.get("vanilla"), vanillaNote);
    auto const& A       = *pack->assets;

    std::vector<Section> sections;
    char const*          unusable  = T("無法使用：", "无法使用：", "cannot be used: ");
    char const*          unreadable = T("讀不到檔案", "读不到文件", "file unreadable");

    // --- the Bedrock side ---
    size_t const bedrockBlocks = countFiles(A, "textures/blocks/", ".png") + countFiles(A, "textures/blocks/", ".tga");
    size_t const bedrockItems  = countFiles(A, "textures/items/", ".png") + countFiles(A, "textures/items/", ".tga");
    {
        Section s{T("基岩版部分", "基岩版部分", "Bedrock side"),
                  T("基岩版要有 manifest.json 才會把資料夾當成材質包。基岩版的貼圖照常作用；Tessera 另外讀 assets/ 裡的 Java 格式檔案。",
                    "基岩版要有 manifest.json 才会把文件夹当成资源包。基岩版的贴图照常生效；Tessera 另外读 assets/ 里的 Java 格式文件。",
                    "Bedrock needs manifest.json to treat the folder as a resource pack. Its Bedrock textures work as usual; Tessera "
                    "also reads the Java-format files under assets/."),
                  "ok",
                  {}};
        if (auto m = A.read("manifest.json")) {
            try {
                auto j = Json::parse(*m, nullptr, true, true);
                if (!j["header"]["uuid"].is_string()) s.items.push_back({"manifest.json", T("header 沒有 uuid", "header 没有 uuid", "header has no uuid")});
                if (!j["modules"].is_array() || j["modules"].empty())
                    s.items.push_back({"manifest.json", T("沒有 modules（要有一個 type 為 resources 的模組）", "没有 modules（要有一个 type 为 resources 的模块）",
                                                          "no modules (it needs one of type resources)")});
            } catch (std::exception const& e) {
                s.items.push_back({"manifest.json", std::string(T("不是有效的 JSON：", "不是有效的 JSON：", "not valid JSON: ")) + e.what()});
            }
        } else
            s.items.push_back({"manifest.json", T("沒有：用 tessera-tool pack 打包時會自動產生", "没有：用 tessera-tool pack 打包时会自动生成",
                                                  "missing: tessera-tool pack adds one")});
        if (!A.read("pack_icon.png") && A.read("pack.png"))
            s.items.push_back({"pack_icon.png", T("沒有（pack 指令會用 pack.png 補上）", "没有（pack 指令会用 pack.png 补上）", "missing (pack uses pack.png)")});
        if (A.list("assets/minecraft/").empty())
            s.items.push_back({"assets/minecraft/", T("沒有 Java 格式的檔案：這是純基岩版材質包，Tessera 不會改變任何東西（用 tessera-tool init 加上資料夾）",
                                                      "没有 Java 格式的文件：这是纯基岩版资源包，Tessera 不会改变任何东西（用 tessera-tool init 加上文件夹）",
                                                      "no Java-format files: a plain Bedrock pack, Tessera changes nothing (tessera-tool init adds the folders)")});
        if (!s.items.empty()) s.level = "warn";
        sections.push_back(std::move(s));
    }

    // --- blocks ---
    std::cout << T("烘焙方塊中…\n", "烘焙方块中…\n", "baking blocks...\n");
    Baked const baked = bakePack(A, vanilla.get());
    auto const& R     = baked.result;
    std::set<std::string> packBlockstates;
    for (auto const& p : A.list("assets/minecraft/blockstates/"))
        if (p.ends_with(".json")) {
            auto n = p.substr(std::string("assets/minecraft/blockstates/").size());
            n.resize(n.size() - 5);
            if (n.find('/') == std::string::npos) packBlockstates.insert(n);
        }
    {
        Section noMapping{T("基岩版沒有對應的方塊", "基岩版没有对应的方块", "No Bedrock block"),
                          T("這些方塊狀態檔的名稱在 Java→基岩版對照表裡找不到（拼錯、只有 Java 版有、或其他命名空間）。",
                            "这些方块状态文件的名称在 Java→基岩版对照表里找不到（拼错、只有 Java 版有、或其他命名空间）。",
                            "These blockstate names are not in the Java-to-Bedrock table (misspelt, Java-only, or another namespace)."),
                          "bad",
                          {}};
        Section noState{T("找不到方塊狀態檔", "找不到方块状态文件", "No blockstate file"),
                        T("材質包改了這些方塊用的模型或貼圖，但材質包和原版都沒有它的方塊狀態檔。",
                          "资源包改了这些方块用的模型或贴图，但资源包和原版都没有它的方块状态文件。",
                          "The pack changes models or textures of these blocks, but neither it nor vanilla has their blockstate file."),
                        "warn",
                        {}};
        Section model{T("缺少模型", "缺少模型", "Missing models"),
                      T("方塊狀態指到的模型找不到，該狀態保留基岩版原本的樣子。", "方块状态指到的模型找不到，该状态保留基岩版原本的样子。",
                        "A blockstate names a model that does not exist; that state keeps Bedrock's own look."),
                      "bad",
                      {}};
        Section parents{T("找不到 parent 模型", "找不到 parent 模型", "Missing parent models"),
                        T("模型的 parent 找不到：只畫模型自己寫的元素與貼圖，parent 提供的部分會缺。",
                          "模型的 parent 找不到：只画模型自己写的元素与贴图，parent 提供的部分会缺。",
                          "A model's parent does not exist: only the model's own elements and textures are drawn."),
                        "warn",
                        {}};
        std::map<std::string, std::vector<std::string>> byParent;
        for (auto const& p : R.problems) {
            auto const        colon = p.find(": ");
            std::string const who   = colon == std::string::npos ? p : p.substr(0, colon);
            std::string const why   = colon == std::string::npos ? "" : p.substr(colon + 2);
            if (why == "no mapping") noMapping.items.push_back({who, ""});
            else if (why == "no blockstate") noState.items.push_back({who, ""});
            else if (why.starts_with("parent ") && why.ends_with(" not found (ignored)")) byParent[why.substr(7, why.size() - 7 - 20)].push_back(who);
            else if (why.starts_with("missing model "))
                model.items.push_back({who, T("找不到模型 ", "找不到模型 ", "missing model ") + why.substr(14)});
            else model.items.push_back({who, why});
        }
        for (auto const& [parent, users] : byParent)
            parents.items.push_back({parent, F(T("{} 個模型用它當 parent（例如 {}）", "{} 个模型用它当 parent（例如 {}）", "parent of {} model(s) (e.g. {})"),
                                               users.size(), users.front())});
        Section skipped{T("Tessera 刻意不處理的方塊", "Tessera 刻意不处理的方块", "Blocks Tessera leaves alone"),
                        T("這些方塊的方塊狀態檔不會被烘焙。", "这些方块的方块状态文件不会被烘焙。", "These blockstate files are not baked."),
                        "warn",
                        {}};
        Section empty{T("沒有產生任何規則", "没有产生任何规则", "No rules made"),
                      T("有方塊狀態檔，但沒有任何狀態和基岩版原本不同（例如模型是 builtin/entity 這類由方塊實體畫的方塊）。",
                        "有方块状态文件，但没有任何状态和基岩版原本不同（例如模型是 builtin/entity 这类由方块实体画的方块）。",
                        "A blockstate file whose states all look as in Bedrock already (e.g. builtin/entity models, drawn by block entities)."),
                      "warn",
                      {}};
        for (auto const& b : packBlockstates) {
            if (auto why = java::blockSkipReason(b))
                skipped.items.push_back(
                    {b, why->starts_with("drawn by the game")
                            ? T("由遊戲自己畫（火、流體、活塞推出中的方塊）", "由游戏自己画（火、流体、活塞推出中的方块）", "drawn by the game itself (fire, fluids, moving pistons)")
                            : T("形狀靠鄰居決定、基岩版沒有對應狀態（柵欄、牆、鐵柵欄、歌萊植株）：保留基岩版的網格，連接材質照常作用",
                                "形状靠邻居决定、基岩版没有对应状态（栅栏、墙、铁栏杆、紫颂植株）：保留基岩版的网格，连接纹理照常生效",
                                "shape comes from neighbours Bedrock keeps no state for (fences, walls, bars, chorus plants): Bedrock's mesh "
                                "is kept, connected textures still apply")});
            else if (auto it = R.rulesPerBlock.find(b); (it == R.rulesPerBlock.end() || it->second == 0) && java::mappings().contains(b))
                empty.items.push_back({b, ""});
        }
        Section tex{T("缺少貼圖", "缺少贴图", "Missing textures"),
                    T("模型用到、但材質包（Java 與基岩版部分）和原版都找不到的貼圖；遊戲裡這些面會改用基岩版內建的同名貼圖（有的話）。",
                      "模型用到、但资源包（Java 与基岩版部分）和原版都找不到的贴图；游戏里这些面会改用基岩版内置的同名贴图（有的话）。",
                      "Textures a model uses that neither the pack (Java or Bedrock side) nor vanilla has; in game those faces fall back "
                      "to Bedrock's built-in texture of that name, if any."),
                    "bad",
                    {}};
        for (auto const& t : baked.missingTextures)
            tex.items.push_back({t, t.find('/') == std::string::npos
                                        ? F(T("像是沒有定義的貼圖變數（#{}）：某個模型用了它，但它和它的 parent 都沒有指定",
                                              "像是没有定义的贴图变量（#{}）：某个模型用了它，但它和它的 parent 都没有指定",
                                              "looks like an undefined texture variable (#{}): a model uses it, but neither it nor its parents set it"),
                                            t)
                                        : ""});
        for (auto* s : {&noMapping, &noState, &model, &tex, &parents, &skipped, &empty})
            if (!s->items.empty()) sections.push_back(std::move(*s));
        if (!baked.fromBedrock.empty()) {
            Section s{T("用材質包基岩版貼圖畫的 Java 貼圖", "用资源包基岩版贴图画的 Java 贴图", "Java textures taken from the pack's Bedrock side"),
                      T("材質包沒有這些 Java 貼圖，但有同名的基岩版貼圖（textures/blocks、textures/items）：模型會用它們，和遊戲裡一樣。",
                        "资源包没有这些 Java 贴图，但有同名的基岩版贴图（textures/blocks、textures/items）：模型会用它们，和游戏里一样。",
                        "The pack has no Java texture of these names but a Bedrock one (textures/blocks, textures/items): models use "
                        "those, as in game."),
                      "muted",
                      {}};
            for (auto const& t : baked.fromBedrock) s.items.push_back({t, ""});
            sections.push_back(std::move(s));
        }
    }

    // --- animated textures ---
    {
        Section s{T("動態貼圖", "动态贴图", "Animated textures"),
                  T("方塊與物品貼圖目前只顯示第一格畫面（水與熔岩除外，它們用基岩版自己的流體動畫）。",
                    "方块与物品贴图目前只显示第一帧（水与熔岩除外，它们用基岩版自己的流体动画）。",
                    "Block and item textures show their first frame for now (except water and lava, which use Bedrock's fluid animation)."),
                  "warn",
                  {}};
        for (auto const& p : A.list("assets/minecraft/textures/"))
            if (p.ends_with(".png.mcmeta")) {
                auto              t    = A.read(p);
                std::string const base = p.substr(0, p.size() - 11);
                bool const used = (p.starts_with("assets/minecraft/textures/block/") || p.starts_with("assets/minecraft/textures/item/"))
                               && !base.ends_with("_n") && !base.ends_with("_s");
                if (used && t && t->find("\"animation\"") != std::string::npos && p.find("/water_") == std::string::npos
                    && p.find("/lava_") == std::string::npos)
                    s.items.push_back({p.substr(26, p.size() - 26 - 11), ""});
            }
        if (!s.items.empty()) sections.push_back(std::move(s));
    }

    // --- connected textures ---
    size_t ctmFiles = 0, ctmOk = 0;
    {
        Section s{T("連接材質（OptiFine CTM）", "连接纹理（OptiFine CTM）", "Connected textures (OptiFine CTM)"), "", "ok", {}};
        static std::set<ctm::Method> const supported = {ctm::Method::Compact, ctm::Method::Fixed, ctm::Method::Random, ctm::Method::Repeat,
                                                        ctm::Method::Top, ctm::Method::Overlay, ctm::Method::OverlayFixed,
                                                        ctm::Method::OverlayRandom, ctm::Method::OverlayRepeat};
        for (auto const& p : A.list("assets/minecraft/optifine/ctm/"))
            if (p.ends_with(".properties")) {
                ++ctmFiles;
                std::string const short_ = p.substr(std::string("assets/minecraft/optifine/").size());
                auto              text   = A.read(p);
                std::string       err;
                auto              rule = text ? ctm::parse(p, *text, err) : std::nullopt;
                if (!rule) {
                    s.items.push_back({short_, unusable + (err.empty() ? std::string(unreadable) : err)});
                    continue;
                }
                bool ok = true;
                if (!supported.contains(rule->method)) {
                    s.items.push_back({short_, F(T("方法 {} 還沒支援（這個檔案不會生效）", "方法 {} 还没支持（这个文件不会生效）",
                                                   "method {} is not supported yet (this file has no effect)"),
                                                 ctm::methodName(rule->method))});
                    ok = false;
                }
                if (!rule->biomes.empty())
                    s.items.push_back({short_, T("biomes 條件還沒支援（會忽略，所有生態域都套用）", "biomes 条件还没支持（会忽略，所有生物群系都套用）",
                                                 "biomes is not supported yet (ignored: applies in every biome)")});
                for (auto const& w : rule->warnings) s.items.push_back({short_, w});
                size_t      missing = 0;
                std::string first;
                for (auto const& t : rule->tiles)
                    if (t.kind == ctm::Tile::Kind::Image && !A.read(t.path) && !missing++) first = t.path;
                if (missing) {
                    s.items.push_back({short_, F(T("{} 張圖塊找不到（例如 {}）", "{} 张图块找不到（例如 {}）", "{} tile(s) not found (e.g. {})"), missing, first)});
                    ok = false;
                }
                if (ok) ++ctmOk;
            }
        s.intro = F(T("{} 個規則檔，{} 個可以使用。", "{} 个规则文件，{} 个可以使用。", "{} rule file(s), {} usable."), ctmFiles, ctmOk);
        if (!s.items.empty()) s.level = "warn";
        if (ctmFiles) sections.push_back(std::move(s));
    }

    // --- CIT ---
    size_t citFiles = 0, citOk = 0;
    auto   modelExists = [&](std::string ref) {
        if (ref.starts_with("minecraft:")) ref = ref.substr(10);
        std::string const path = "assets/minecraft/models/" + ref + ".json";
        return A.read(path) || (vanilla && vanilla->read(path)) || java::builtinModel(ref);
    };
    char const* noModel = T("模型找不到：", "模型找不到：", "model not found: ");
    {
        Section s{T("自訂物品材質（OptiFine CIT）", "自定义物品纹理（OptiFine CIT）", "Custom item textures (OptiFine CIT)"), "", "ok", {}};
        for (auto const& p : A.list("assets/minecraft/optifine/cit/"))
            if (p.ends_with(".properties")) {
                ++citFiles;
                std::string const short_ = p.substr(std::string("assets/minecraft/optifine/").size());
                auto              text   = A.read(p);
                std::string       err;
                auto              rule = text ? cit::parse(p, *text, err) : std::nullopt;
                if (!rule) {
                    s.items.push_back({short_, unusable + (err.empty() ? std::string(unreadable) : err)});
                    continue;
                }
                bool ok = true;
                for (auto const& w : rule->warnings) s.items.push_back({short_, w});
                if (!rule->texture.empty() && !A.read(rule->texture)) {
                    s.items.push_back({short_, T("貼圖找不到：", "贴图找不到：", "texture not found: ") + rule->texture});
                    ok = false;
                }
                if (!rule->model.empty() && !modelExists(rule->model)) {
                    s.items.push_back({short_, noModel + rule->model});
                    ok = false;
                }
                if (ok) ++citOk;
            }
        s.intro = F(T("{} 個規則檔，{} 個可以使用。", "{} 个规则文件，{} 个可以使用。", "{} rule file(s), {} usable."), citFiles, citOk);
        if (!s.items.empty()) s.level = "warn";
        if (citFiles) sections.push_back(std::move(s));
    }

    // --- item definitions (1.21.4+) ---
    size_t defFiles = 0, defOk = 0;
    {
        Section s{T("物品定義（1.21.4 起的 items/*.json）", "物品定义（1.21.4 起的 items/*.json）", "Item definitions (items/*.json, 1.21.4+)"), "", "ok", {}};
        for (auto const& p : A.list("assets/minecraft/items/"))
            if (p.ends_with(".json") && p.find('/', std::string("assets/minecraft/items/").size()) == std::string::npos) {
                ++defFiles;
                std::string const short_ = p.substr(std::string("assets/minecraft/").size());
                auto              text   = A.read(p);
                std::string       err;
                auto              def = text ? itemdef::parse(p, *text, err) : std::nullopt;
                if (!def) {
                    s.items.push_back({short_, unusable + (err.empty() ? std::string(unreadable) : err)});
                    continue;
                }
                for (auto const& w : def->warnings) s.items.push_back({short_, w});
                std::set<std::string> missing;
                for (auto const& set : itemdef::outcomes(*def))
                    for (auto const& spec : set)
                        if (auto m = itemdef::splitSpec(spec).model; !m.empty() && !modelExists(m)) missing.insert(m);
                for (auto const& m : missing) s.items.push_back({short_, noModel + m});
                if (missing.empty()) ++defOk;
            }
        s.intro = F(T("{} 個物品定義，{} 個所有模型都找得到。", "{} 个物品定义，{} 个所有模型都找得到。", "{} definition(s), {} with every model found."),
                    defFiles, defOk);
        if (!s.items.empty()) s.level = "warn";
        if (defFiles) sections.push_back(std::move(s));
    }

    // --- not read by Tessera ---
    {
        struct Kind {
            char const* prefix;
            char const* hant;
            char const* hans;
            char const* en;
        };
        static constexpr Kind kinds[] = {
            {"assets/minecraft/textures/entity/", "生物與實體貼圖（基岩版實體用自己的貼圖與模型）", "生物与实体贴图（基岩版实体用自己的贴图与模型）",
             "entity textures (Bedrock entities use their own textures and models)"},
            {"assets/minecraft/models/entity/", "實體模型", "实体模型", "entity models"},
            {"assets/minecraft/optifine/cem/", "OptiFine 自訂實體模型（CEM）", "OptiFine 自定义实体模型（CEM）", "OptiFine custom entity models (CEM)"},
            {"assets/minecraft/optifine/random/", "OptiFine 隨機實體材質", "OptiFine 随机实体纹理", "OptiFine random entities"},
            {"assets/minecraft/optifine/mob/", "OptiFine 隨機實體材質（舊路徑）", "OptiFine 随机实体纹理（旧路径）", "OptiFine random entities (old path)"},
            {"assets/minecraft/optifine/sky/", "OptiFine 自訂天空", "OptiFine 自定义天空", "OptiFine custom sky"},
            {"assets/minecraft/optifine/colormap/", "OptiFine 自訂顏色表", "OptiFine 自定义颜色表", "OptiFine custom colours"},
            {"assets/minecraft/optifine/anim/", "OptiFine 自訂動畫", "OptiFine 自定义动画", "OptiFine custom animations"},
            {"assets/minecraft/optifine/emissive", "OptiFine 發光貼圖（可改用 labPBR _s 的發光通道）", "OptiFine 发光贴图（可改用 labPBR _s 的发光通道）",
             "OptiFine emissive textures (use the labPBR _s emissive channel instead)"},
            {"assets/minecraft/optifine/font/", "OptiFine 字型", "OptiFine 字体", "OptiFine fonts"},
            {"assets/minecraft/textures/gui/", "介面貼圖", "界面贴图", "GUI textures"},
            {"assets/minecraft/textures/particle/", "粒子貼圖", "粒子贴图", "particle textures"},
            {"assets/minecraft/textures/environment/", "天空、天氣貼圖", "天空、天气贴图", "sky and weather textures"},
            {"assets/minecraft/textures/painting/", "畫", "画", "paintings"},
            {"assets/minecraft/textures/trims/", "盔甲紋飾貼圖", "盔甲纹饰贴图", "armour trim textures"},
            {"assets/minecraft/textures/models/armor/", "盔甲穿戴貼圖", "盔甲穿戴贴图", "worn armour textures"},
            {"assets/minecraft/textures/entity/equipment/", "裝備穿戴貼圖", "装备穿戴贴图", "worn equipment textures"},
            {"assets/minecraft/equipment/", "裝備定義", "装备定义", "equipment definitions"},
            {"assets/minecraft/font/", "字型", "字体", "fonts"},
            {"assets/minecraft/lang/", "語言檔", "语言文件", "language files"},
            {"assets/minecraft/sounds", "音效", "音效", "sounds"},
            {"assets/minecraft/shaders/", "核心著色器", "核心着色器", "core shaders"},
            {"assets/minecraft/particles/", "粒子定義", "粒子定义", "particle definitions"},
            {"assets/minecraft/texts/", "文字（標題畫面等）", "文字（标题画面等）", "texts (title screen etc.)"},
            {"assets/minecraft/atlases/", "圖集定義", "图集定义", "atlas definitions"},
        };
        Section s{T("Tessera 不會讀的部分", "Tessera 不会读的部分", "Not read by Tessera"),
                  T("這些 Java 檔案不影響方塊與物品，Tessera 會略過（基岩版自己的檔案照常作用）。",
                    "这些 Java 文件不影响方块与物品，Tessera 会略过（基岩版自己的文件照常生效）。",
                    "These Java files do not affect blocks and items; Tessera skips them (the pack's Bedrock files work as usual)."),
                  "muted",
                  {}};
        for (auto const& k : kinds) {
            size_t n = A.list(k.prefix).size();
            if (std::string_view(k.prefix) == "assets/minecraft/textures/entity/") n -= A.list("assets/minecraft/textures/entity/equipment/").size();
            if (n) s.items.push_back({k.prefix + std::string("…"), F(T("{}：{} 個檔案", "{}：{} 个文件", "{}: {} file(s)"), T(k.hant, k.hans, k.en), n)});
        }
        std::set<std::string> others;
        for (auto const& p : A.list("assets/"))
            if (!p.starts_with("assets/minecraft/")) others.insert(p.substr(7, p.find('/', 7) - 7));
        for (auto const& ns : others)
            s.items.push_back({"assets/" + ns + "/", T("其他命名空間（模組的資源）", "其他命名空间（模组的资源）", "another namespace (a mod's assets)")});
        if (!s.items.empty()) sections.push_back(std::move(s));
    }

    // --- page ---
    std::string const title = packTitle(A, pack->label);
    size_t            bad = 0, warn = 0;
    for (auto const& s : sections)
        if (s.level == "bad") bad += s.items.size();
        else if (s.level == "warn") warn += s.items.size();
    std::string page = pageHead(T("Tessera 檢查：", "Tessera 检查：", "Tessera check: ") + title);
    page += F("<h1>{}</h1><p class=muted>{} · {} · {}</p>", html(title), T("Tessera 材質包檢查", "Tessera 资源包检查", "Tessera pack check"),
              html(pack->label), html(vanillaNote));
    page += "<div class=stats>";
    auto stat = [&](std::string const& label, std::string const& value, char const* cls = "") {
        page += F("<div class=stat><span class=muted>{}</span><b class='{}'>{}</b></div>", html(label), cls, html(value));
    };
    stat(T("方塊狀態規則", "方块状态规则", "Block-state rules"), std::to_string(R.ruleCount));
    stat(T("涉及的方塊", "涉及的方块", "Blocks"), std::to_string(R.rulesPerBlock.size()));
    stat(T("烘焙圖片", "烘焙图片", "Baked images"), std::to_string(R.textures.size()));
    stat(T("連接材質", "连接纹理", "Connected textures"), F("{}/{}", ctmOk, ctmFiles));
    stat("CIT", F("{}/{}", citOk, citFiles));
    stat(T("物品定義", "物品定义", "Item definitions"), F("{}/{}", defOk, defFiles));
    stat(T("基岩版貼圖", "基岩版贴图", "Bedrock textures"), std::to_string(bedrockBlocks + bedrockItems));
    stat(T("錯誤", "错误", "Errors"), std::to_string(bad), bad ? "bad" : "ok");
    stat(T("注意", "注意", "Warnings"), std::to_string(warn), warn ? "warn" : "ok");
    page += "</div>";
    page += F(T("<p class=muted>Java 模型 {} 個、Java 貼圖 {} 張、方塊狀態檔 {} 個；基岩版方塊貼圖 {} 張、物品貼圖 {} 張；烘焙 {} ms。圖片預覽：<code>tessera-tool preview</code>。</p>",
                "<p class=muted>Java 模型 {} 个、Java 贴图 {} 张、方块状态文件 {} 个；基岩版方块贴图 {} 张、物品贴图 {} 张；烘焙 {} ms。图片预览：<code>tessera-tool preview</code>。</p>",
                "<p class=muted>{} Java models, {} Java textures, {} blockstate files; {} Bedrock block and {} item textures; baked in {} ms. "
                "Pictures: <code>tessera-tool preview</code>.</p>"),
              countFiles(A, "assets/minecraft/models/", ".json"), countFiles(A, "assets/minecraft/textures/", ".png"), packBlockstates.size(),
              bedrockBlocks, bedrockItems, baked.ms);
    for (auto const& s : sections) {
        page += F("<h2 class='{}'>{} ({})</h2>", s.level, html(s.title), s.items.size());
        if (!s.intro.empty()) page += "<p class=muted>" + html(s.intro) + "</p>";
        if (s.items.empty()) {
            page += F("<p class=ok>{}</p>", T("沒有問題。", "没有问题。", "No problems."));
            continue;
        }
        bool const longList = s.items.size() > 40;
        if (longList) page += F("<details><summary>{}</summary>", F(T("展開 {} 項", "展开 {} 项", "Show {} items"), s.items.size()));
        page += F("<table><tr><th>{}</th><th>{}</th></tr>", T("檔案／方塊", "文件／方块", "File / block"), T("說明", "说明", "Note"));
        for (auto const& f : s.items) page += F("<tr><td><code>{}</code></td><td>{}</td></tr>", html(f.where), html(f.what));
        page += "</table>";
        if (longList) page += "</details>";
    }
    page += F("<h2>{} ({})</h2><details><summary>{}</summary><table><tr><th>{}</th><th>{}</th></tr>", T("每個方塊的規則", "每个方块的规则", "Rules per block"),
              R.rulesPerBlock.size(), T("展開", "展开", "Show"), T("方塊", "方块", "Block"), T("規則", "规则", "Rules"));
    for (auto const& [b, n] : R.rulesPerBlock) page += F("<tr><td><code>{}</code></td><td>{}</td></tr>", html(b), n);
    page += "</table></details>";
    page += pageTail();

    fs::path const out = pathOf(args.get("out", "tessera-check"));
    writeFile(out / "report.html", page);
    std::cout << F(T("{}：{} 條規則、{} 個方塊、{} 個錯誤、{} 個注意\n報告：{}\n", "{}：{} 条规则、{} 个方块、{} 个错误、{} 个注意\n报告：{}\n",
                     "{}: {} rules for {} blocks, {} errors, {} warnings\nreport: {}\n"),
                   title, R.ruleCount, R.rulesPerBlock.size(), bad, warn, u8(fs::absolute(out / "report.html")));
    return bad ? 1 : 0;
}

// ---------------------------------------------------------------------------
// preview
// ---------------------------------------------------------------------------

int cmdPreview(Args const& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: tessera-tool preview <pack> [-o <dir>] [--vanilla <jar>|off] [--block <name>] [--max <n>] [--lang ...]\n";
        return 2;
    }
    std::string error;
    auto        pack = openPack(pathOf(args.positional[0]), error);
    if (!pack) {
        std::cerr << error << "\n";
        return 1;
    }
    std::string       vanillaNote;
    auto              vanilla   = openVanilla(args.get("vanilla"), vanillaNote);
    auto const&       A         = *pack->assets;
    std::string const only      = args.get("block");
    int const         maxStates = std::max(1, std::atoi(args.get("max", "48").c_str()));
    fs::path const    out       = pathOf(args.get("out", "tessera-preview"));
    std::error_code   ec;
    fs::create_directories(out / "img", ec);

    std::cout << T("烘焙方塊中…\n", "烘焙方块中…\n", "baking blocks...\n");
    Baked const baked = bakePack(A, vanilla.get());
    Json        doc;
    try {
        doc = Json::parse(baked.result.rulesJson.empty() ? std::string("{}") : baked.result.rulesJson);
    } catch (...) {}
    std::map<std::string, std::vector<Json const*>> byBlock; // Bedrock name -> rules, file order
    if (doc.contains("rules"))
        for (auto const& r : doc["rules"]) {
            std::string b = r.value("block", std::string{});
            if (b.starts_with("minecraft:")) b = b.substr(10);
            if (!only.empty() && b.find(only) == std::string::npos) continue;
            byBlock[b].push_back(&r);
        }

    std::set<uint64_t> written;
    auto save = [&](java::Image const& img) {
        std::string const png  = java::encodePng(img);
        uint64_t const    h    = fnv(png);
        std::string const name = F("img/{:016x}.png", h);
        if (written.insert(h).second) writeFile(out / pathOf(name), png);
        return name;
    };

    std::string const title = packTitle(A, pack->label);
    std::string       page  = pageHead(T("Tessera 預覽：", "Tessera 预览：", "Tessera preview: ") + title);
    page += F("<h1>{}</h1><p class=muted>{} · {}</p>", html(title),
              T("Tessera 預覽 · 方塊從東南上方看（上、南、東三面），和遊戲裡一樣依方塊狀態挑模型",
                "Tessera 预览 · 方块从东南上方看（上、南、东三面），和游戏里一样依方块状态挑模型",
                "Tessera preview · blocks seen from the south-east, above (top, south, east), models picked by block state as in game"),
              html(vanillaNote));
    page += F("<p><input type=search placeholder='{}' oninput=\"for(const s of document.querySelectorAll('section[data-b]'))s.hidden=!s.dataset.b.includes(this.value.trim())\"></p>",
              T("篩選名稱…", "筛选名称…", "Filter by name…"));
    size_t drawn = 0, blocks = 0;
    for (auto const& [block, rules] : byBlock) {
        std::string        tiles;
        std::set<uint64_t> seen;
        int                shown = 0, hidden = 0;
        for (auto const* r : rules) {
            auto quads = ruleQuads(*r, baked.result.textures);
            if (!quads) continue;
            java::Image const img = render(*quads, 96);
            if (!seen.insert(fnv(java::encodePng(img))).second) continue; // same look as a state already shown
            if (shown >= maxStates) {
                ++hidden;
                continue;
            }
            ++shown;
            std::string const label = html(whenText(*r));
            tiles += F("<div class=tile title='{0}'><img src='{1}' alt='{0}'>{0}</div>", label, save(img));
        }
        if (!shown) continue;
        ++blocks;
        drawn += size_t(shown);
        std::string const more = hidden ? F(T("，只畫前 {} 種", "，只画前 {} 种", ", first {} drawn"), shown) : std::string();
        page += F("<section data-b='{0}'><h2>{0} <span class=muted>({1})</span></h2><div class=grid>{2}</div></section>", html(block),
                  F(T("{} 條規則，{} 種外觀{}", "{} 条规则，{} 种外观{}", "{} rules, {} looks{}"), rules.size(), shown + hidden, more), tiles);
    }

    // Connected textures: a sample wall per rule.
    std::string walls;
    size_t      wallCount = 0;
    for (auto const& p : A.list("assets/minecraft/optifine/ctm/")) {
        if (!p.ends_with(".properties")) continue;
        if (!only.empty() && p.find(only) == std::string::npos) continue;
        auto        text = A.read(p);
        std::string err;
        auto        rule = text ? ctm::parse(p, *text, err) : std::nullopt;
        if (!rule) continue;
        auto img = ctmWall(*rule, A);
        if (!img) continue;
        ++wallCount;
        walls += F("<div class=wall><img src='{}' alt=''>{} <code>{}</code></div>", save(*img), html(p.substr(std::string("assets/minecraft/optifine/ctm/").size())),
                   ctm::methodName(rule->method));
    }
    if (!walls.empty())
        page += F("<section data-b='ctm'><h2>{} <span class=muted>({})</span></h2><p class=muted>{}</p><div class=grid>{}</div></section>",
                  T("連接材質", "连接纹理", "Connected textures"), wallCount,
                  T("每個規則檔畫一面有缺口的牆，看邊緣、轉角、內角怎麼接（overlay 類規則要配合底下的方塊，這裡不畫）。",
                    "每个规则文件画一面有缺口的墙，看边缘、转角、内角怎么接（overlay 类规则要配合底下的方块，这里不画）。",
                    "One wall with a gap per rule file, to show edges, corners and inner corners (overlay rules need the block "
                    "underneath and are not drawn here)."),
                  walls);

    // Item icons: items the pack defines, or whose item model / texture it replaces (those take
    // Java's own definition, as in game).
    java::BakeOptions options;
    options.pack               = &A;
    options.vanilla            = vanilla.get();
    options.packBedrockTexture = [&](std::string const& name) { return packBedrockTexture(A, name); };
    java::ItemIcons       icons(options);
    std::string           itemTiles;
    size_t                items = 0;
    std::set<std::string> ids;
    auto leaf = [](std::string const& p, std::string const& dir, std::string const& ext) -> std::optional<std::string> {
        if (!p.starts_with(dir) || !p.ends_with(ext)) return std::nullopt;
        std::string n = p.substr(dir.size(), p.size() - dir.size() - ext.size());
        if (n.find('/') != std::string::npos) return std::nullopt;
        return n;
    };
    for (auto const& [dir, ext] : {std::pair<std::string, std::string>{"assets/minecraft/items/", ".json"},
                                   {"assets/minecraft/models/item/", ".json"}, {"assets/minecraft/textures/item/", ".png"}})
        for (auto const& p : A.list(dir))
            if (auto n = leaf(p, dir, ext)) ids.insert(*n);
    for (auto const& id : ids) {
        if (!only.empty() && id.find(only) == std::string::npos) continue;
        std::string const p    = "assets/minecraft/items/" + id + ".json";
        auto              text = A.read(p);
        if (!text && vanilla) text = vanilla->read(p);
        std::string err;
        auto        def = text ? itemdef::parse(p, *text, err) : std::nullopt;
        if (!def) continue;
        std::set<uint64_t> seen;
        int                n = 0;
        for (auto const& set : itemdef::outcomes(*def)) {
            if (set.empty() || n >= 8) continue;
            auto const spec = itemdef::splitSpec(set.front());
            auto       icon = icons.icon(spec.model, spec.tints, false);
            if (!icon || !seen.insert(fnv(java::encodePng(*icon))).second) continue;
            ++n;
            std::string const label = html(def->item + (n > 1 ? F(" #{}", n) : std::string()));
            itemTiles += F("<div class=tile title='{0}'><img src='{1}' alt='{0}'>{0}</div>", label, save(*icon));
        }
        if (n) ++items;
    }
    if (!itemTiles.empty())
        page += F("<section data-b='items'><h2>{} <span class=muted>({})</span></h2><div class=grid>{}</div></section>", T("物品圖示", "物品图标", "Item icons"),
                  F(T("{} 個物品，每個最多 8 種", "{} 个物品，每个最多 8 种", "{} items, up to 8 looks each"), items), itemTiles);
    if (!blocks && itemTiles.empty() && walls.empty())
        page += F("<p class=muted>{}</p>", T("沒有可以預覽的方塊或物品。", "没有可以预览的方块或物品。", "Nothing to preview."));
    page += pageTail();
    writeFile(out / "preview.html", page);
    std::cout << F(T("{}：{} 個方塊共 {} 種外觀、{} 面連接材質牆、{} 個物品\n預覽：{}\n", "{}：{} 个方块共 {} 种外观、{} 面连接纹理墙、{} 个物品\n预览：{}\n",
                     "{}: {} looks for {} blocks, {} connected-texture walls, {} items\npreview: {}\n"),
                   title, gLang == Lang::En ? drawn : blocks, gLang == Lang::En ? blocks : drawn, wallCount, items, u8(fs::absolute(out / "preview.html")));
    return 0;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

int cmdInit(Args const& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: tessera-tool init <Bedrock pack dir>\n";
        return 2;
    }
    fs::path const  dir = pathOf(args.positional[0]);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::cerr << F(T("找不到資料夾：{}\n", "找不到文件夹：{}\n", "no such folder: {}\n"), u8(dir));
        return 1;
    }
    if (!fs::exists(dir / "manifest.json", ec))
        std::cout << T("注意：這個資料夾沒有 manifest.json（不是基岩版材質包）；打包時 tessera-tool pack 會補上。\n",
                       "注意：这个文件夹没有 manifest.json（不是基岩版资源包）；打包时 tessera-tool pack 会补上。\n",
                       "note: this folder has no manifest.json (not a Bedrock pack yet); tessera-tool pack adds one.\n");
    char const* folders[] = {"assets/minecraft/blockstates", "assets/minecraft/models/block", "assets/minecraft/models/item",
                             "assets/minecraft/items",       "assets/minecraft/textures/block", "assets/minecraft/textures/item",
                             "assets/minecraft/optifine/ctm", "assets/minecraft/optifine/cit"};
    int made = 0;
    for (char const* f : folders)
        if (!fs::exists(dir / f, ec)) {
            fs::create_directories(dir / f, ec);
            ++made;
        }
    fs::path const readme = dir / "assets" / "TESSERA.txt";
    if (!fs::exists(readme, ec))
        writeFile(readme,
                  "Tessera - Java-format assets for Minecraft Bedrock\n"
                  "==================================================\n\n"
                  "Files here are read by the Tessera mod (LeviLamina). Write them exactly as for a Java resource pack:\n"
                  "  minecraft/blockstates/*.json      models per block state\n"
                  "  minecraft/models/block|item/*.json Blockbench / Java block and item models\n"
                  "  minecraft/textures/block|item/*.png textures (+ _n / _s labPBR maps, .mcmeta)\n"
                  "  minecraft/items/*.json            1.21.4+ item model definitions\n"
                  "  minecraft/optifine/ctm/**.properties  connected textures\n"
                  "  minecraft/optifine/cit/**.properties  custom item textures\n"
                  "The Bedrock part of the pack (textures/blocks, textures/items, ...) keeps working; a Java model that names a\n"
                  "texture you only have as a Bedrock texture (textures/blocks/stone.png for block/stone) uses that picture.\n"
                  "Check:   tessera-tool check <pack>\nPreview: tessera-tool preview <pack>\nPackage: tessera-tool pack <pack>\n\n"
                  "這裡的檔案由 Tessera 模組讀取，寫法和 Java 版材質包完全相同（方塊狀態、模型、貼圖、物品定義、OptiFine CTM／CIT）。\n"
                  "基岩版部分照常作用；Java 模型用到、但只有基岩版貼圖的名稱，會用那張基岩版貼圖。\n\n"
                  "这里的文件由 Tessera 模组读取，写法和 Java 版资源包完全相同（方块状态、模型、贴图、物品定义、OptiFine CTM／CIT）。\n"
                  "基岩版部分照常生效；Java 模型用到、但只有基岩版贴图的名称，会用那张基岩版贴图。\n");
    std::cout << F(T("已在 {} 加上 {} 個資料夾和 assets/TESSERA.txt。\n", "已在 {} 加上 {} 个文件夹和 assets/TESSERA.txt。\n",
                     "added {1} folder(s) and assets/TESSERA.txt to {0}.\n"),
                   u8(dir), made);
    return 0;
}

// ---------------------------------------------------------------------------
// pack (.mcpack writer)
// ---------------------------------------------------------------------------

class ZipWriter {
public:
    explicit ZipWriter(fs::path const& path) : mOut(path, std::ios::binary) {}
    bool ok() const { return bool(mOut); }

    void add(std::string const& name, std::string const& data) {
        uLong const crc = crc32(0L, reinterpret_cast<Bytef const*>(data.data()), uInt(data.size()));
        std::string packed;
        uint16_t    method = 0;
        if (data.size() > 64) { // raw deflate
            z_stream z{};
            deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            packed.resize(deflateBound(&z, uLong(data.size())));
            z.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
            z.avail_in  = uInt(data.size());
            z.next_out  = reinterpret_cast<Bytef*>(packed.data());
            z.avail_out = uInt(packed.size());
            deflate(&z, Z_FINISH);
            packed.resize(z.total_out);
            deflateEnd(&z);
            if (packed.size() < data.size()) method = 8;
        }
        std::string const& body   = method ? packed : data;
        uint32_t const     offset = uint32_t(mOut.tellp());
        put32(0x04034b50), put16(20), put16(0x0800), put16(method), put16(kTime), put16(kDate);
        put32(uint32_t(crc)), put32(uint32_t(body.size())), put32(uint32_t(data.size())), put16(uint16_t(name.size())), put16(0);
        mOut.write(name.data(), std::streamsize(name.size()));
        mOut.write(body.data(), std::streamsize(body.size()));
        mCentral.push_back({name, uint32_t(crc), uint32_t(body.size()), uint32_t(data.size()), method, offset});
    }

    void finish() {
        uint32_t const start = uint32_t(mOut.tellp());
        for (auto const& e : mCentral) {
            put32(0x02014b50), put16(20), put16(20), put16(0x0800), put16(e.method), put16(kTime), put16(kDate);
            put32(e.crc), put32(e.csize), put32(e.size), put16(uint16_t(e.name.size())), put16(0), put16(0), put16(0), put16(0), put32(0);
            put32(e.offset);
            mOut.write(e.name.data(), std::streamsize(e.name.size()));
        }
        uint32_t const size = uint32_t(mOut.tellp()) - start;
        put32(0x06054b50), put16(0), put16(0), put16(uint16_t(mCentral.size())), put16(uint16_t(mCentral.size())), put32(size), put32(start), put16(0);
    }

private:
    static constexpr uint16_t kTime = 0, kDate = (46 << 9) | (11 << 5) | 22; // 2026-11-22
    struct Entry {
        std::string name;
        uint32_t    crc, csize, size;
        uint16_t    method;
        uint32_t    offset;
    };
    void put16(uint16_t v) {
        char b[2] = {char(v & 0xFF), char(v >> 8)};
        mOut.write(b, 2);
    }
    void put32(uint32_t v) {
        char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char(v >> 24)};
        mOut.write(b, 4);
    }
    std::ofstream      mOut;
    std::vector<Entry> mCentral;
};

std::string newUuid() {
    std::random_device      rd;
    std::mt19937_64         g((uint64_t(rd()) << 32) ^ rd());
    std::array<uint8_t, 16> b{};
    for (auto& x : b) x = uint8_t(g());
    b[6] = uint8_t((b[6] & 0x0F) | 0x40);
    b[8] = uint8_t((b[8] & 0x3F) | 0x80);
    std::string s;
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += std::format("{:02x}", b[i]);
    }
    return s;
}

int cmdPack(Args const& args) {
    if (args.positional.empty()) {
        std::cerr << "usage: tessera-tool pack <pack dir> [-o <file.mcpack>] [--name <name>]\n";
        return 2;
    }
    fs::path const  dir = pathOf(args.positional[0]);
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || !isPackRoot(dir)) {
        std::cerr << F(T("要打包的資料夾裡要有 assets/ 或 manifest.json：{}\n", "要打包的文件夹里要有 assets/ 或 manifest.json：{}\n",
                         "the folder needs assets/ or manifest.json in it: {}\n"),
                       u8(dir));
        return 1;
    }
    std::vector<std::pair<std::string, fs::path>> files;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        std::string const rel  = u8(fs::relative(it->path(), dir));
        std::string const name = lower(u8(it->path().filename()));
        if (name == "desktop.ini" || name == "thumbs.db" || name == ".ds_store" || rel.starts_with(".git/")) continue;
        files.emplace_back(rel, it->path());
    }
    std::sort(files.begin(), files.end());
    std::string name = args.get("name");
    if (name.empty()) {
        DirAssets a(dir);
        name = packTitle(a, u8(dir.filename()));
    }
    fs::path const out = pathOf(args.get("out", u8(dir.filename()) + ".mcpack"));
    ZipWriter      zip(out);
    if (!zip.ok()) {
        std::cerr << F(T("無法寫入 {}\n", "无法写入 {}\n", "cannot write {}\n"), u8(out));
        return 1;
    }
    bool hasManifest = false, hasIcon = false;
    for (auto const& [rel, path] : files) {
        hasManifest |= rel == "manifest.json";
        hasIcon |= rel == "pack_icon.png";
    }
    fs::path const savedManifest = out.parent_path() / (out.stem().string() + ".manifest.json");
    if (!hasManifest) {
        Json m = {{"format_version", 2},
                  {"header",
                   {{"name", name},
                    {"description", T("Java 格式材質包（需要 Tessera 模組）", "Java 格式资源包（需要 Tessera 模组）", "Java-format pack (needs the Tessera mod)")},
                    {"uuid", newUuid()},
                    {"version", {1, 0, 0}},
                    {"min_engine_version", {1, 21, 0}}}},
                  {"modules", Json::array({{{"type", "resources"}, {"uuid", newUuid()}, {"version", {1, 0, 0}}}})}};
        zip.add("manifest.json", m.dump(2));
        writeFile(savedManifest, m.dump(2));
        std::cout << T("已加上 manifest.json（新的 UUID）\n", "已加上 manifest.json（新的 UUID）\n", "added manifest.json (new UUIDs)\n");
    }
    size_t n = 0;
    for (auto const& [rel, path] : files) {
        auto data = readFile(path);
        if (!data) continue;
        zip.add(rel, *data);
        ++n;
        if (!hasIcon && rel == "pack.png") {
            zip.add("pack_icon.png", *data);
            std::cout << T("已用 pack.png 加上 pack_icon.png\n", "已用 pack.png 加上 pack_icon.png\n", "added pack_icon.png (from pack.png)\n");
        }
    }
    zip.finish();
    std::cout << F(T("{} 個檔案 -> {}\n", "{} 个文件 -> {}\n", "{} file(s) -> {}\n"), n, u8(fs::absolute(out)));
    if (!hasManifest)
        std::cout << F(T("產生的 manifest 也存到了 {}：放回材質包資料夾，下次打包才會沿用同一組 UUID。\n",
                         "生成的 manifest 也存到了 {}：放回资源包文件夹，下次打包才会沿用同一组 UUID。\n",
                         "The generated manifest is also saved as {}: put it in the pack folder so the next build keeps the same UUIDs.\n"),
                       u8(savedManifest));
    return 0;
}

void usage() {
    std::cout << T("tessera-tool 0.2.0：給 Tessera 用的 Java 格式材質包工具\n\n", "tessera-tool 0.2.0：给 Tessera 用的 Java 格式资源包工具\n\n",
                   "tessera-tool 0.2.0 - Java-format resource packs for Tessera\n\n")
              << "  tessera-tool init    <Bedrock pack dir>\n"
                 "  tessera-tool check   <pack> [-o <dir>] [--vanilla <client.jar>|off]\n"
                 "  tessera-tool preview <pack> [-o <dir>] [--vanilla ...] [--block <name>] [--max <n>]\n"
                 "  tessera-tool pack    <pack dir> [-o <file.mcpack>] [--name <name>]\n"
                 "  --lang en | zh-Hans | zh-Hant\n\n"
              << T("<pack>：資料夾、.zip 或 .mcpack。原版 Java 資源從你自己的 Java 版 jar 讀（%APPDATA%\\.minecraft\\versions 裡最新的版本）。\n",
                   "<pack>：文件夹、.zip 或 .mcpack。原版 Java 资源从你自己的 Java 版 jar 读（%APPDATA%\\.minecraft\\versions 里最新的版本）。\n",
                   "<pack>: a folder, .zip or .mcpack. Vanilla Java assets come from your own client jar (newest under "
                   "%APPDATA%\\.minecraft\\versions).\n");
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> list; // arguments as UTF-8 (paths with CJK characters)
    int                      wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 1; i < wargc; ++i) {
            int const   n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
            list.push_back(std::move(s));
        }
        LocalFree(wargv);
    }
#else
    std::vector<std::string> list(argv + 1, argv + argc);
#endif
    (void)argc;
    (void)argv;
    Args const args = parseArgs(list);
    std::string const lang = lower(args.get("lang"));
    gLang = lang.empty() ? systemLang() : lang == "zh-hant" || lang == "zh-tw" ? Lang::Hant : lang == "zh-hans" || lang == "zh-cn" || lang == "zh" ? Lang::Hans : Lang::En;
    try {
        if (args.command == "init") return cmdInit(args);
        if (args.command == "check") return cmdCheck(args);
        if (args.command == "preview") return cmdPreview(args);
        if (args.command == "pack") return cmdPack(args);
    } catch (std::exception const& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    usage();
    return args.command.empty() || args.command == "help" || args.command == "--help" ? 0 : 2;
}
