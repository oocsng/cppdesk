#include "glide.hpp"

namespace cppdesk {

namespace {
uint64_t* slot(AppSettings& s, GlideEdge e) {
    switch (e) {
        case GlideEdge::Left:   return &s.glideLeft;
        case GlideEdge::Right:  return &s.glideRight;
        case GlideEdge::Top:    return &s.glideTop;
        case GlideEdge::Bottom: return &s.glideBottom;
        default:                return nullptr;
    }
}
constexpr GlideEdge kEdges[] = {GlideEdge::Left, GlideEdge::Right, GlideEdge::Top, GlideEdge::Bottom};
} // namespace

GlideEdge glideOpposite(GlideEdge e) {
    switch (e) {
        case GlideEdge::Left:   return GlideEdge::Right;
        case GlideEdge::Right:  return GlideEdge::Left;
        case GlideEdge::Top:    return GlideEdge::Bottom;
        case GlideEdge::Bottom: return GlideEdge::Top;
        default:                return GlideEdge::None;
    }
}

uint64_t glidePcAt(const AppSettings& s, GlideEdge e) {
    uint64_t* p = slot(const_cast<AppSettings&>(s), e);
    return p ? *p : 0;
}

GlideEdge glideEdgeOf(const AppSettings& s, uint64_t deskId) {
    if (deskId == 0) return GlideEdge::None;
    for (GlideEdge e : kEdges) if (glidePcAt(s, e) == deskId) return e;
    return GlideEdge::None;
}

void glidePlace(AppSettings& s, GlideEdge e, uint64_t deskId) {
    uint64_t* target = slot(s, e);
    if (!target) return;
    if (deskId != 0) {
        for (GlideEdge o : kEdges) if (glidePcAt(s, o) == deskId) *slot(s, o) = 0;
    }
    *target = deskId;
}

} // namespace cppdesk
