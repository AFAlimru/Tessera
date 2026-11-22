// Tessera - platform-independent UV remapping over raw vertex arrays.
#pragma once

#include <vector>

#include "core/Rules.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace tessera {

struct V2 {
    float x, y;
};
struct V3 {
    float x, y, z;
};

struct UvRect {
    float u0{0}, v0{0}, u1{0}, v1{0};
    [[nodiscard]] bool valid() const { return u1 > u0 && v1 > v0; }
    [[nodiscard]] bool contains(float u, float v) const {
        constexpr float e = 1e-5f;
        return u >= u0 - e && u <= u1 + e && v >= v0 - e && v <= v1 + e;
    }
    [[nodiscard]] bool operator==(UvRect const&) const = default;
};

struct Replacement {
    UvRect   src;
    UvRect   dst;
    uint8_t  sideMask{kAllSides};
    TintMode tint{TintMode::Keep};
    uint32_t tintRgba{0xFFFFFFFF};
    uint16_t pbrHandle{0}; // target's PBR texture handle (Vibrant Visuals); 0 = unknown
    bool     hasPbr{false};
    // Wildcard source ("*" / "*:up"): matches any quad and lays the target out from the
    // quad's position in the block, like a Java model face without explicit uv.
    bool     anySource{false};
    uint8_t  facingMask{kAllSides}; // which way the quad faces (Top = up, Bottom = down)
    uint8_t  rotX{0}, rotY{0};      // model rotation (quarter turns); faces/uv are model-space
    // "*:east@0.125": only quads lying on that plane (model-space position along the
    // facing axis, 0..1). Lets hollow blocks (hoppers, composters) give their inner
    // faces their own image. Negative = any plane.
    float    plane{-1.0f};
    // Biome-tinted targets (grass sides): one destination per variant of the block's own
    // texture; the variant the quad shows (its source rect) picks the destination.
    std::vector<std::pair<UvRect, UvRect>> variants;
};

struct QuadReport {
    size_t firstVertex;
    Side   side;
    V3     localMin, localMax; // relative to the estimated block origin
    UvRect uv;                 // bounding box of the quad's UVs before remapping
    int    replacement;        // index into replacements, -1 = untouched
    Side   facing;             // which way the quad faces (from its winding)
    bool   tilted;             // not axis-aligned: wildcard sources leave it alone
};

struct VertexStreams {
    V3*       positions{};
    V2*       uv0{};
    uint32_t* colors{};      // may be null
    uint16_t* pbrIndices{};  // may be null
    size_t    count{};       // valid length of every non-null stream
};

// Which way a quad (four vertices) faces, and whether it is tilted (not lying flat in
// an axis plane, or turned within it). origin = the block's cell corner.
struct QuadFacing {
    Side side;
    bool tilted;
};
QuadFacing quadFacing(V3 const* fourVertices, V3 const& origin);
// Java's default face uv (0..1) for a point relative to the cell corner.
V2 faceUv(Side facing, V3 const& local);

// Remap quads in [begin, end). Vertices are assumed to come in groups of four.
// Returns the number of quads changed. `report` (optional) sees every quad.
size_t remapRange(
    VertexStreams const&                          s,
    size_t                                        begin,
    size_t                                        end,
    std::span<Replacement const>                  replacements,
    std::function<void(QuadReport const&)> const& report = {},
    V3 const*                                     originHint = nullptr // the block's cell corner in vertex space
);

} // namespace tessera
