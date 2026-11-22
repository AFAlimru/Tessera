// The Chunker Java->Bedrock block table (third_party/chunker, MIT), zlib-compressed TSV:
// min_data_version \t java_name \t java_properties \t bedrock_name \t bedrock_states
#include "core/java/JavaPack.h"

#include <zlib.h>

#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace {
#include "../../../third_party/chunker/GeneratedChunkerMappings.inc"
} // namespace

namespace tessera::java {

namespace {

std::string_view stripNamespace(std::string_view name) {
    auto const colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

MappingTable load() {
    std::string table(kChunkerMappingRawSize, '\0');
    uLongf      size   = static_cast<uLongf>(table.size());
    auto const  result = uncompress(
        reinterpret_cast<Bytef*>(table.data()), &size, kChunkerMappingCompressed,
        static_cast<uLong>(sizeof(kChunkerMappingCompressed))
    );
    if (result != Z_OK) throw std::runtime_error("cannot decompress the Chunker block table");
    table.resize(size);

    MappingTable     out;
    std::string_view rest{table};
    while (!rest.empty()) {
        auto const       end  = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest                  = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (line.empty() || line.front() == '#') continue;
        std::string_view cols[5];
        int              n = 0;
        for (; n < 5; ++n) {
            auto const tab = line.find('\t');
            cols[n]        = line.substr(0, tab);
            if (tab == std::string_view::npos) {
                ++n;
                break;
            }
            line = line.substr(tab + 1);
        }
        if (n < 5) continue;
        int version = 0;
        for (char c : cols[0])
            if (c >= '0' && c <= '9') version = version * 10 + (c - '0');
        std::string_view bedrock = cols[3];
        if (bedrock.starts_with("minecraft:")) bedrock = bedrock.substr(10);
        auto& slot = out[std::string(stripNamespace(cols[1]))];
        auto  it   = slot.find(std::string(cols[2]));
        if (it == slot.end() || version >= it->second.dataVersion)
            slot[std::string(cols[2])] = BedrockState{version, std::string(bedrock), std::string(cols[4])};
    }
    return out;
}

} // namespace

MappingTable const& mappings() {
    static MappingTable    table;
    static std::once_flag once;
    std::call_once(once, [] { table = load(); });
    return table;
}

} // namespace tessera::java
