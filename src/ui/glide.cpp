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
        default:                return GlideEdge::Off;
    }
}

uint64_t glidePcAt(const AppSettings& s, GlideEdge e) {
    uint64_t* p = slot(const_cast<AppSettings&>(s), e);
    return p ? *p : 0;
}

GlideEdge glideEdgeOf(const AppSettings& s, uint64_t deskId) {
    if (deskId == 0) return GlideEdge::Off;
    for (GlideEdge e : kEdges) if (glidePcAt(s, e) == deskId) return e;
    return GlideEdge::Off;
}

void glidePlace(AppSettings& s, GlideEdge e, uint64_t deskId) {
    uint64_t* target = slot(s, e);
    if (!target) return;
    if (deskId != 0) {
        for (GlideEdge o : kEdges) if (glidePcAt(s, o) == deskId) *slot(s, o) = 0;
    }
    *target = deskId;
}

namespace {
constexpr uint64_t kDwellMs = 250;
constexpr float kHomeAwayPx = 40.0f;
bool horizontal(GlideEdge e) { return e == GlideEdge::Top || e == GlideEdge::Bottom; }
}

float glideAlong(const ui::Rect& b, GlideEdge e, ui::Point p) {
    float t = horizontal(e) ? (p.x - b.left) / b.width() : (p.y - b.top) / b.height();
    return std::clamp(t, 0.0f, 1.0f);
}

GlideEdge glideEdgeAt(const ui::Rect& b, ui::Point p) {
    for (GlideEdge e : kEdges) {
        float dist = e == GlideEdge::Left ? p.x - b.left : e == GlideEdge::Right ? (b.right - 1) - p.x
                   : e == GlideEdge::Top ? p.y - b.top : (b.bottom - 1) - p.y;
        float along = glideAlong(b, e, p);
        if (dist <= 1.0f && along >= 0.1f && along <= 0.9f) return e;
    }
    return GlideEdge::Off;
}

GlideEdge GlideDwell::update(GlideEdge edgeNow, uint64_t nowMs) {
    if (edgeNow != edge_) { edge_ = edgeNow; startMs_ = nowMs; fired_ = false; }
    if (edge_ == GlideEdge::Off || fired_ || nowMs - startMs_ < kDwellMs) return GlideEdge::Off;
    fired_ = true;
    return edge_;
}

void GlideHome::arrive(GlideEdge homeEdge) { home_ = homeEdge; armed_ = false; dwell_.reset(); }

bool GlideHome::update(const ui::Rect& w, ui::Point p, uint64_t nowMs) {
    if (home_ == GlideEdge::Off) return false;
    if (!armed_) {
        float away = home_ == GlideEdge::Left ? p.x - w.left : home_ == GlideEdge::Right ? w.right - p.x
                   : home_ == GlideEdge::Top ? p.y - w.top : w.bottom - p.y;
        armed_ = away >= kHomeAwayPx;
        return false;
    }
    GlideEdge at = glideEdgeAt(w, p);
    return dwell_.update(at == home_ ? at : GlideEdge::Off, nowMs) != GlideEdge::Off;
}

ui::Point glideEntryPoint(const ui::Rect& m, GlideEdge pushed, float along) {
    float x = m.left + along * m.width(), y = m.top + along * m.height();
    switch (pushed) {
        case GlideEdge::Right:  return {m.left + 2, y};
        case GlideEdge::Left:   return {m.right - 3, y};
        case GlideEdge::Bottom: return {x, m.top + 2};
        default:                return {x, m.bottom - 3};
    }
}

ui::Point glideReturnPoint(const ui::Rect& d, GlideEdge pushed, float along) {
    return glideEntryPoint(d, glideOpposite(pushed), along);
}

bool glidePaused(bool otherSessionOpen, bool fleetOpen, bool hosting, bool cursorKnown) {
    return otherSessionOpen || fleetOpen || hosting || !cursorKnown;
}

} // namespace cppdesk
