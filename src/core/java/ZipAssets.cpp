#include "core/java/ZipAssets.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <tuple>

namespace tessera::java {
namespace {

uint16_t u16(unsigned char const* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t u32(unsigned char const* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

} // namespace

ZipAssets::ZipAssets(std::filesystem::path const& file, std::string prefix) {
    mIn.open(file, std::ios::binary);
    if (!mIn) return;
    mIn.seekg(0, std::ios::end);
    auto const total = uint64_t(mIn.tellg());
    if (total < 22) return;
    // End of central directory: within the last 64 KiB + 22 bytes.
    uint64_t const tail = std::min<uint64_t>(total, 65557);
    std::vector<unsigned char> buf(tail);
    mIn.seekg(std::streamoff(total - tail));
    mIn.read(reinterpret_cast<char*>(buf.data()), std::streamsize(tail));
    int64_t eocd = -1;
    for (int64_t i = int64_t(tail) - 22; i >= 0; --i)
        if (u32(&buf[size_t(i)]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) return;
    uint32_t const cdSize = u32(&buf[size_t(eocd) + 12]);
    uint32_t const cdOff  = u32(&buf[size_t(eocd) + 16]);
    if (uint64_t(cdOff) + cdSize > total) return;
    std::vector<unsigned char> cd(cdSize);
    mIn.seekg(std::streamoff(cdOff));
    mIn.read(reinterpret_cast<char*>(cd.data()), std::streamsize(cdSize));
    for (size_t at = 0; at + 46 <= cd.size();) {
        if (u32(&cd[at]) != 0x02014b50) break;
        uint16_t const method = u16(&cd[at + 10]);
        uint32_t const csize  = u32(&cd[at + 20]);
        uint32_t const usize  = u32(&cd[at + 24]);
        uint16_t const nlen   = u16(&cd[at + 28]);
        uint16_t const xlen   = u16(&cd[at + 30]);
        uint16_t const clen   = u16(&cd[at + 32]);
        uint32_t const local  = u32(&cd[at + 42]);
        if (at + 46 + nlen > cd.size()) break;
        std::string name(reinterpret_cast<char const*>(&cd[at + 46]), nlen);
        at += 46 + size_t(nlen) + xlen + clen;
        if (!name.starts_with(prefix) || name.ends_with("/")) continue;
        if (method != 0 && method != 8) continue;
        mEntries[name] = Entry{local, csize, usize, method};
        mNames.push_back(std::move(name));
    }
    std::sort(mNames.begin(), mNames.end());
    mOk = !mEntries.empty();
}

std::optional<std::string> ZipAssets::read(std::string const& path) const {
    auto it = mEntries.find(path);
    if (it == mEntries.end()) return std::nullopt;
    Entry const& e = it->second;
    std::string  packed(e.compressed, '\0');
    {
        std::lock_guard lock(mMutex);
        unsigned char   head[30];
        mIn.clear();
        mIn.seekg(std::streamoff(e.localHeader));
        mIn.read(reinterpret_cast<char*>(head), 30);
        if (!mIn || u32(head) != 0x04034b50) return std::nullopt;
        mIn.seekg(std::streamoff(e.localHeader + 30 + u16(head + 26) + u16(head + 28)));
        mIn.read(packed.data(), std::streamsize(packed.size()));
        if (!mIn) return std::nullopt;
    }
    if (e.method == 0) return packed;
    std::string out(e.size, '\0');
    z_stream    zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return std::nullopt;
    zs.next_in   = reinterpret_cast<Bytef*>(packed.data());
    zs.avail_in  = uInt(packed.size());
    zs.next_out  = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = uInt(out.size());
    int const rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) return std::nullopt;
    return out;
}

std::vector<std::string> ZipAssets::list(std::string const& prefix) const {
    std::vector<std::string> out;
    auto it = std::lower_bound(mNames.begin(), mNames.end(), prefix);
    for (; it != mNames.end() && it->starts_with(prefix); ++it) out.push_back(*it);
    return out;
}

std::filesystem::path findJavaClientJar(std::filesystem::path const& minecraftDir) {
    std::error_code ec;
    auto const      versions = minecraftDir / "versions";
    if (!std::filesystem::is_directory(versions, ec)) return {};
    // "1.21.4", "1.21.11-pre1", "26.1.2": numbers first, then a release before its pre-releases.
    using Key = std::tuple<int, int, int, int>;
    std::optional<Key>    best;
    std::filesystem::path bestJar;
    for (auto const& dir : std::filesystem::directory_iterator(versions, ec)) {
        if (!dir.is_directory()) continue;
        std::string const name = dir.path().filename().string();
        auto const        jar  = dir.path() / (name + ".jar");
        if (!std::filesystem::is_regular_file(jar, ec)) continue;
        int  part[3] = {0, 0, 0}, n = 0;
        size_t i = 0;
        bool  ok = false;
        while (i < name.size() && n < 3) {
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) break;
            int v = 0;
            while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) v = v * 10 + (name[i++] - '0');
            part[n++] = v;
            ok        = true;
            if (i < name.size() && name[i] == '.') ++i;
            else break;
        }
        if (!ok || n < 2) continue; // snapshots ("25w45a"), modded profiles
        bool const release = i == name.size();
        if (!release && name.substr(i).find_first_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos) continue;
        if (!release && name.find("-pre") == std::string::npos && name.find("-rc") == std::string::npos) continue;
        Key const key{part[0], part[1], part[2], release ? 1 : 0};
        if (!best || key > *best) best = key, bestJar = jar;
    }
    return bestJar;
}

} // namespace tessera::java
