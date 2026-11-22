#include "core/Remap.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace tessera {

static Side classify(V3 const& mn, V3 const& mx, V3 const& origin) {
    float ext[3] = {mx.x - mn.x, mx.y - mn.y, mx.z - mn.z};
    int   axis   = 0;
    for (int a = 1; a < 3; ++a)
        if (ext[a] < ext[axis]) axis = a;
    if (ext[axis] > 0.01f) return Side::Center; // not axis-aligned (cross plants, tilted quads)
    float c = (axis == 0 ? mn.x - origin.x : axis == 1 ? mn.y - origin.y : mn.z - origin.z);
    if (c > 0.35f && c < 0.65f) return Side::Center;
    bool positive = c >= 0.5f;
    switch (axis) {
    case 0: return positive ? Side::East : Side::West;
    case 1: return positive ? Side::Top : Side::Bottom;
    default: return positive ? Side::South : Side::North;
    }
}

// Which way a quad faces. The winding of its vertices gives the normal, which is
// what tells an inner wall of a hopper from the outer one. Whether the game winds
// front faces clockwise or not is learnt from quads lying on the cell boundary,
// whose outward direction is certain; until enough of those are seen, position is
// used (floor = down, other horizontals = up, verticals face away from the centre).
static std::atomic<int> gWindingVotes{0};

struct Facing {
    Side side;
    bool tilted;
};

static Facing facingOf(V3 const* p, V3 const& mn, V3 const& mx, V3 const& origin) {
    float ext[3] = {mx.x - mn.x, mx.y - mn.y, mx.z - mn.z};
    int   axis   = 0;
    for (int a = 1; a < 3; ++a)
        if (ext[a] < ext[axis]) axis = a;
    bool tilted = ext[axis] > 0.01f;
    // Flat in an axis plane but turned within it (the side faces of a lever's
    // handle): its corners sit along the bounding box's edges, not at its corners.
    if (!tilted) {
        for (int k = 0; k < 4 && !tilted; ++k) {
            float const c[3]  = {p[k].x, p[k].y, p[k].z};
            float const lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
            for (int a = 0; a < 3; ++a)
                if (a != axis && std::abs(c[a] - lo[a]) > 0.004f && std::abs(c[a] - hi[a]) > 0.004f) tilted = true;
        }
    }

    V3 const    e1{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
    V3 const    e2{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
    float const n[3]  = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    float const coord = axis == 0 ? mn.x - origin.x : axis == 1 ? mn.y - origin.y : mn.z - origin.z;

    int sign = 0; // +1: faces the positive direction of `axis`
    if (!tilted && std::abs(n[axis]) > 1e-6f) {
        int const raw = n[axis] > 0 ? 1 : -1;
        if (coord <= 0.001f || coord >= 0.999f) { // on the cell boundary: outward is known
            int const outward = coord >= 0.999f ? 1 : -1;
            if (std::abs(gWindingVotes.load(std::memory_order_relaxed)) < 100000)
                gWindingVotes.fetch_add(raw == outward ? 1 : -1, std::memory_order_relaxed);
        }
        int const votes = gWindingVotes.load(std::memory_order_relaxed);
        if (std::abs(votes) >= 16) sign = votes > 0 ? raw : -raw;
    }
    if (sign == 0) {
        if (axis == 1) sign = coord <= 0.001f ? -1 : 1;
        else sign = coord < 0.5f ? -1 : 1;
    }
    Side side;
    switch (axis) {
    case 1: side = sign > 0 ? Side::Top : Side::Bottom; break;
    case 0: side = sign > 0 ? Side::East : Side::West; break;
    default: side = sign > 0 ? Side::South : Side::North; break;
    }
    return {side, tilted};
}

// Position of a model-space point along the axis `facing` looks down, 0..1.
static float planeOf(Side facing, V3 const& l) {
    switch (facing) {
    case Side::Top:
    case Side::Bottom: return l.y;
    case Side::East:
    case Side::West: return l.x;
    default: return l.z;
    }
}

// Java's default face uv: the face's own position projected onto the block, 0..1.
static V2 projectedUv(Side facing, V3 const& l) {
    auto c = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    switch (facing) {
    case Side::Top: return {c(l.x), c(l.z)};
    case Side::Bottom: return {c(l.x), c(1 - l.z)};
    case Side::North: return {c(1 - l.x), c(1 - l.y)};
    case Side::South: return {c(l.x), c(1 - l.y)};
    case Side::West: return {c(l.z), c(1 - l.y)};
    default: return {c(1 - l.z), c(1 - l.y)};
    }
}

// Undo a Java blockstate rotation (x first, then y, about the block centre):
// world-space point/direction -> model space.
static V3 unrotatePoint(V3 p, uint8_t rx, uint8_t ry) {
    for (int i = 0; i < ry; ++i) p = {p.z, p.y, 1 - p.x};
    for (int i = 0; i < rx; ++i) p = {p.x, 1 - p.z, p.y};
    return p;
}
static Side unrotateSide(Side s, uint8_t rx, uint8_t ry) {
    int v[3];
    switch (s) {
    case Side::Top: v[0] = 0, v[1] = 1, v[2] = 0; break;
    case Side::Bottom: v[0] = 0, v[1] = -1, v[2] = 0; break;
    case Side::North: v[0] = 0, v[1] = 0, v[2] = -1; break;
    case Side::South: v[0] = 0, v[1] = 0, v[2] = 1; break;
    case Side::East: v[0] = 1, v[1] = 0, v[2] = 0; break;
    case Side::West: v[0] = -1, v[1] = 0, v[2] = 0; break;
    default: return s;
    }
    for (int i = 0; i < ry; ++i) {
        int x = v[0];
        v[0] = v[2], v[2] = -x;
    }
    for (int i = 0; i < rx; ++i) {
        int y = v[1];
        v[1] = -v[2], v[2] = y;
    }
    if (v[1]) return v[1] > 0 ? Side::Top : Side::Bottom;
    if (v[2]) return v[2] > 0 ? Side::South : Side::North;
    return v[0] > 0 ? Side::East : Side::West;
}

QuadFacing quadFacing(V3 const* p, V3 const& origin) {
    V3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
    for (int k = 0; k < 4; ++k) {
        mn = {std::min(mn.x, p[k].x), std::min(mn.y, p[k].y), std::min(mn.z, p[k].z)};
        mx = {std::max(mx.x, p[k].x), std::max(mx.y, p[k].y), std::max(mx.z, p[k].z)};
    }
    auto const f = facingOf(p, mn, mx, origin);
    return {f.side, f.tilted};
}

V2 faceUv(Side facing, V3 const& local) { return projectedUv(facing, local); }

size_t remapRange(
    VertexStreams const&                          s,
    size_t                                        begin,
    size_t                                        end,
    std::span<Replacement const>                  replacements,
    std::function<void(QuadReport const&)> const& report,
    V3 const*                                     originHint
) {
    end = std::min(end, s.count);
    if (!s.positions || !s.uv0 || end <= begin) return 0;

    // Block origin: the minimum corner of everything this block emitted.
    V3 origin{INFINITY, INFINITY, INFINITY};
    for (size_t i = begin; i < end; ++i) {
        origin.x = std::min(origin.x, s.positions[i].x);
        origin.y = std::min(origin.y, s.positions[i].y);
        origin.z = std::min(origin.z, s.positions[i].z);
    }
    origin = {std::floor(origin.x + 1e-3f), std::floor(origin.y + 1e-3f), std::floor(origin.z + 1e-3f)};
    // The vertices alone mislead when faces are culled (a cube showing only its
    // top looks one block higher), so prefer the cell corner the caller knows,
    // as long as it is plausibly the same cell.
    if (originHint && std::abs(originHint->x - origin.x) <= 1.01f && std::abs(originHint->y - origin.y) <= 1.01f
        && std::abs(originHint->z - origin.z) <= 1.01f)
        origin = *originHint;

    // How many different planes this block shows toward each direction. A direction
    // with a single plane (a flat rail, whose Java model is built from several
    // layers) is everything there is to see that way, so it takes the composite
    // image rather than one layer of it.
    bool multiPlane[6] = {};
    {
        float seen[6]    = {};
        bool  hasSeen[6] = {};
        for (size_t q = begin; q + 4 <= end; q += 4) {
            V3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
            for (size_t k = q; k < q + 4; ++k) {
                auto const& p = s.positions[k];
                mn            = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
                mx            = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
            }
            Facing const f = facingOf(s.positions + q, mn, mx, origin);
            if (f.tilted) continue;
            auto const  i = size_t(f.side);
            float const c = planeOf(f.side, {mn.x - origin.x, mn.y - origin.y, mn.z - origin.z});
            if (!hasSeen[i]) hasSeen[i] = true, seen[i] = c;
            else if (std::abs(seen[i] - c) > 0.01f) multiPlane[i] = true;
        }
    }

    size_t changed = 0;
    for (size_t q = begin; q + 4 <= end; q += 4) {
        V3     mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
        UvRect uv{INFINITY, INFINITY, -INFINITY, -INFINITY};
        for (size_t k = q; k < q + 4; ++k) {
            auto const& p = s.positions[k];
            mn            = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
            mx            = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
            auto const& t = s.uv0[k];
            uv            = {std::min(uv.u0, t.x), std::min(uv.v0, t.y), std::max(uv.u1, t.x), std::max(uv.v1, t.y)};
        }
        Side const   side   = classify(mn, mx, origin);
        Facing const fq     = facingOf(s.positions + q, mn, mx, origin);
        Side const   facing = fq.side;
        V3 const     first{s.positions[q].x - origin.x, s.positions[q].y - origin.y, s.positions[q].z - origin.z};
        float cu   = (uv.u0 + uv.u1) * 0.5f;
        float cv   = (uv.v0 + uv.v1) * 0.5f;

        // Plane-specific images come first in the list: the nearest plane wins (a
        // torch's own face over the glow box a pixel away), then the first of the rest.
        int   hit      = -1;
        float bestDist = INFINITY;
        for (size_t r = 0; r < replacements.size(); ++r) {
            auto const& rep = replacements[r];
            if (!(rep.sideMask & sideBit(side))) continue;
            if (!rep.anySource) {
                if (!rep.src.contains(cu, cv)) continue;
            } else {
                // Tilted quads (a lever's handle) have no flat image to take: leave them as they are.
                if (fq.tilted) continue;
                Side const modelFacing = unrotateSide(facing, rep.rotX, rep.rotY);
                if (!(rep.facingMask & sideBit(modelFacing))) continue;
                if (rep.plane >= 0.0f) {
                    if (!multiPlane[size_t(facing)]) continue;
                    float const d =
                        std::abs(planeOf(modelFacing, unrotatePoint(first, rep.rotX, rep.rotY)) - rep.plane);
                    if (d <= 0.035f && d < bestDist) bestDist = d, hit = int(r);
                    continue;
                }
            }
            if (hit < 0) hit = int(r);
            break;
        }
        if (hit >= 0) {
            auto const& rep = replacements[size_t(hit)];
            UvRect      dst = rep.dst;
            for (auto const& [from, to] : replacements[size_t(hit)].variants)
                if (from.contains(cu, cv)) {
                    dst = to;
                    break;
                }
            float su = rep.src.u1 - rep.src.u0, sv = rep.src.v1 - rep.src.v0;
            float du = dst.u1 - dst.u0, dv = dst.v1 - dst.v0;
            for (size_t k = q; k < q + 4; ++k) {
                auto& t = s.uv0[k];
                if (rep.anySource) {
                    auto const& p     = s.positions[k];
                    V3 const    model = unrotatePoint({p.x - origin.x, p.y - origin.y, p.z - origin.z}, rep.rotX, rep.rotY);
                    V2 const    f     = projectedUv(unrotateSide(facing, rep.rotX, rep.rotY), model);
                    t.x           = dst.u0 + f.x * du;
                    t.y           = dst.v0 + f.y * dv;
                } else {
                    t.x = dst.u0 + (t.x - rep.src.u0) / su * du;
                    t.y = dst.v0 + (t.y - rep.src.v0) / sv * dv;
                }
                if (s.colors) {
                    if (rep.tint == TintMode::None) s.colors[k] = (s.colors[k] & 0xFF000000u) | 0x00FFFFFFu;
                    else if (rep.tint == TintMode::Color)
                        s.colors[k] = (s.colors[k] & 0xFF000000u) | (rep.tintRgba & 0x00FFFFFFu);
                }
                if (s.pbrIndices && rep.hasPbr) s.pbrIndices[k] = rep.pbrHandle;
            }
            ++changed;
        }
        if (report)
            report({q, side, {mn.x - origin.x, mn.y - origin.y, mn.z - origin.z},
                    {mx.x - origin.x, mx.y - origin.y, mx.z - origin.z}, uv, hit, facing, fq.tilted});
    }
    return changed;
}

} // namespace tessera
