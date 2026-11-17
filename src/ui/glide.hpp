#pragma once

// Glide: which paired PC sits past which screen edge. Pure helpers over the saved layout; no platform headers.

#include "../core/crypto_identity.hpp"
#include "canvas.hpp"

#include <cstdint>

namespace cppdesk {

enum class GlideEdge : uint8_t { None, Left, Right, Top, Bottom };

GlideEdge glideOpposite(GlideEdge e);
uint64_t  glidePcAt(const AppSettings& s, GlideEdge e);          // 0 for None or nothing placed
GlideEdge glideEdgeOf(const AppSettings& s, uint64_t deskId);    // None if not placed
void      glidePlace(AppSettings& s, GlideEdge e, uint64_t deskId); // one edge per PC; deskId 0 clears e

// bounds: right/bottom exclusive, screen pixels, top-left origin. On an edge = within 1 px of it.
// Inside the outer 10 % at either end of that edge -> None.
GlideEdge glideEdgeAt(const ui::Rect& bounds, ui::Point p);

class GlideDwell {                       // fires once per rest; must leave the edge to fire again
public:
    GlideEdge update(GlideEdge edgeNow, uint64_t nowMs);   // returns the edge on the tick dwell reaches 250 ms, else None
    void reset() { *this = GlideDwell(); }
private:
    GlideEdge edge_ = GlideEdge::None;
    uint64_t  startMs_ = 0;
    bool      fired_ = false;
};

class GlideHome {                        // inside the full-screen glide window, DIP coordinates
public:
    void arrive(GlideEdge homeEdge);     // disarmed until the cursor is >= 40 px from homeEdge
    bool update(const ui::Rect& window, ui::Point p, uint64_t nowMs);   // true once: armed + 250 ms at homeEdge (10 % rule)
private:
    GlideEdge  home_ = GlideEdge::None;
    bool       armed_ = false;
    GlideDwell dwell_;
};

// Where the cursor enters the remote screen: `along` is 0..1 along the pushed edge.
ui::Point glideEntryPoint(const ui::Rect& monitor, GlideEdge pushed, float along);   // 2 px inside the opposite edge
ui::Point glideReturnPoint(const ui::Rect& desktop, GlideEdge pushed, float along);  // 2 px inside the pushed edge
float glideAlong(const ui::Rect& bounds, GlideEdge e, ui::Point p);                  // 0..1 position along e
bool glidePaused(bool otherSessionOpen, bool fleetOpen, bool hosting, bool cursorKnown);   // true if any of the first three, or !cursorKnown

} // namespace cppdesk
