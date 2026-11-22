#include "core/java/JavaPack.h"

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#include <stb/stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#include <stb/stb_image_write.h>

namespace tessera::java {

bool Image::anyOpaque() const {
    for (auto p : pixels)
        if (p >> 24) return true;
    return false;
}

std::optional<Image> decodePng(std::string const& bytes) {
    int  w = 0, h = 0, channels = 0;
    auto data = stbi_load_from_memory(
        reinterpret_cast<stbi_uc const*>(bytes.data()), int(bytes.size()), &w, &h, &channels, 4
    );
    if (!data) return std::nullopt;
    Image image;
    image.width  = w;
    image.height = h;
    image.pixels.resize(size_t(w) * size_t(h));
    std::memcpy(image.pixels.data(), data, image.pixels.size() * 4);
    stbi_image_free(data);
    return image;
}

std::string encodePng(Image const& image) {
    std::string out;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            static_cast<std::string*>(context)->append(static_cast<char const*>(data), size_t(size));
        },
        &out, image.width, image.height, 4, image.pixels.data(), image.width * 4
    );
    return out;
}

void bleedTransparent(Image& image, int passes) {
    int const w = image.width, h = image.height;
    if (w <= 0 || h <= 0) return;
    std::vector<uint8_t> known(size_t(w) * size_t(h));
    for (size_t i = 0; i < known.size(); ++i) known[i] = (image.pixels[i] >> 24) != 0;
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<std::pair<size_t, uint32_t>> fill;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                size_t const i = size_t(y) * size_t(w) + size_t(x);
                if (known[i]) continue;
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int const nx = x + dx, ny = y + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        size_t const j = size_t(ny) * size_t(w) + size_t(nx);
                        if (!known[j]) continue;
                        uint32_t const p = image.pixels[j];
                        r += p & 0xFF, g += (p >> 8) & 0xFF, b += (p >> 16) & 0xFF, ++n;
                    }
                if (n) fill.push_back({i, (r / n) | ((g / n) << 8) | ((b / n) << 16)});
            }
        if (fill.empty()) break;
        for (auto const& [i, rgb] : fill) {
            image.pixels[i] = rgb; // alpha 0
            known[i]        = 1;
        }
    }
}

} // namespace tessera::java
