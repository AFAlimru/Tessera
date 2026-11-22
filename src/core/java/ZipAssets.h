// Tessera - Java assets read straight from a zip (the player's own Java client jar).
#pragma once

#include "core/java/JavaPack.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tessera::java {

class ZipAssets : public Assets {
public:
    // Indexes the entries under `prefix` (e.g. "assets/minecraft/"). Check ok() afterwards.
    explicit ZipAssets(std::filesystem::path const& file, std::string prefix = "assets/minecraft/");
    bool ok() const { return mOk; }
    size_t size() const { return mEntries.size(); }

    std::optional<std::string> read(std::string const& path) const override;
    std::vector<std::string>   list(std::string const& prefix) const override;

private:
    struct Entry {
        uint64_t localHeader = 0, compressed = 0, size = 0;
        uint16_t method      = 0;
    };
    mutable std::ifstream                    mIn;
    mutable std::mutex                       mMutex;
    std::unordered_map<std::string, Entry>   mEntries;
    std::vector<std::string>                 mNames; // sorted
    bool                                     mOk = false;
};

// The newest Java client jar under <appdata>/.minecraft/versions (releases preferred over
// pre-releases of the same number), or empty when there is none.
std::filesystem::path findJavaClientJar(std::filesystem::path const& minecraftDir);

} // namespace tessera::java
