#pragma once

// Glide: which paired PC sits past which screen edge. Pure helpers over the saved layout; no platform headers.

#include "../core/crypto_identity.hpp"

#include <cstdint>

namespace cppdesk {

enum class GlideEdge : uint8_t { None, Left, Right, Top, Bottom };

GlideEdge glideOpposite(GlideEdge e);
uint64_t  glidePcAt(const AppSettings& s, GlideEdge e);          // 0 for None or nothing placed
GlideEdge glideEdgeOf(const AppSettings& s, uint64_t deskId);    // None if not placed
void      glidePlace(AppSettings& s, GlideEdge e, uint64_t deskId); // one edge per PC; deskId 0 clears e

} // namespace cppdesk
