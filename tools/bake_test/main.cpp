// Offline check of the C++ Java-pack baker against tools/convert_java_pack.py.
//
//   bake_test <pack dir> <vanilla dir> <out dir>
//
// Both inputs are extracted folders (the pack zip and the Java client jar). Writes
// <out>/state_textures.json and <out>/textures/blocks/jt/<name>.png, the same layout
// the Python converter writes, so tools/compare_bake.py can compare them.
#include "core/java/JavaPack.h"
#include "core/java/ZipAssets.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;
using namespace tessera::java;

namespace {

class DirAssets : public Assets {
public:
    explicit DirAssets(fs::path root) : mRoot(std::move(root)) {
        for (auto const& e : fs::recursive_directory_iterator(mRoot))
            if (e.is_regular_file()) {
                auto rel = fs::relative(e.path(), mRoot).generic_u8string();
                mFiles.emplace_back(rel.begin(), rel.end());
            }
    }
    std::optional<std::string> read(std::string const& path) const override {
        std::ifstream in(mRoot / fs::path(std::u8string(path.begin(), path.end())), std::ios::binary);
        if (!in) return std::nullopt;
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }
    std::vector<std::string> list(std::string const& prefix) const override {
        std::vector<std::string> out;
        for (auto const& f : mFiles)
            if (f.starts_with(prefix)) out.push_back(f);
        return out;
    }

private:
    fs::path                 mRoot;
    std::vector<std::string> mFiles;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: bake_test <pack dir> <vanilla dir|-> <out dir> [fallback textures dir]\n";
        return 2;
    }
    auto const start = std::chrono::steady_clock::now();
    DirAssets                  pack(argv[1]);
    std::optional<DirAssets>   vanilla;
    std::optional<ZipAssets> jar;
    if (std::string(argv[2]).ends_with(".jar")) jar.emplace(argv[2]);
    else if (std::string(argv[2]) != "-") vanilla.emplace(argv[2]); // "-": no vanilla files
    auto const loaded = std::chrono::steady_clock::now();

    BakeOptions options;
    options.pack    = &pack;
    options.vanilla = vanilla ? static_cast<Assets const*>(&*vanilla) : jar ? static_cast<Assets const*>(&*jar) : nullptr;
    // Optional 4th argument: a folder of Java textures standing in for the game's own
    // textures (what the Bedrock fallback does in game).
    std::optional<DirAssets> textures;
    if (argc > 4) {
        textures.emplace(argv[4]);
        options.fallbackTexture = [&](std::string const& name) -> std::optional<Image> {
            auto bytes = textures->read("assets/minecraft/textures/" + name + ".png");
            return bytes ? decodePng(*bytes) : std::nullopt;
        };
    }
    BakeResult const result = bake(options);
    auto const       baked  = std::chrono::steady_clock::now();

    fs::path const out = argv[3];
    fs::create_directories(out / "textures" / "blocks" / "jt");
    std::ofstream(out / "state_textures.json", std::ios::binary) << result.rulesJson;
    for (auto const& [name, image] : result.textures)
        std::ofstream(out / "textures" / "blocks" / "jt" / (name + ".png"), std::ios::binary) << encodePng(image);

    auto ms = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
    std::cout << "rules: " << result.ruleCount << ", textures: " << result.textures.size() << "\n";
    std::cout << "time: listing " << ms(start, loaded) << " ms, baking " << ms(loaded, baked) << " ms\n";
    std::cout << "problems: " << result.problems.size() << "\n";
    for (size_t i = 0; i < result.problems.size(); ++i) std::cout << "  ! " << result.problems[i] << "\n";
    return 0;
}
