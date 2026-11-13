#pragma once

// Pixel helpers for the macOS capture path, plain C++ so every platform's tests can check them.

#include <cstddef>
#include <cstdint>

namespace cppdesk {

// Copies a BGRA frame whose rows are `srcStride` bytes apart into a tightly packed destination
// (width * height * 4 bytes), setting every alpha byte to 0xFF. Row padding is not copied.
void copyBgraRows(const uint8_t* src, size_t srcStride, int width, int height, uint8_t* dst);

// The size to capture a width x height screen at: scaled down (never up) so the long edge is at most
// maxLongEdge, aspect kept, both sides even and at least 2.
void fitCaptureSize(int width, int height, int maxLongEdge, int& outWidth, int& outHeight);

// True when a running capture stream no longer matches its display: frames come at frameWidth x frameHeight, but the
// display's pixel size now fits a different capture size (a resolution change; the stream keeps its old output size).
// False while no frame has arrived (frame size 0).
bool captureSizeStale(int frameWidth, int frameHeight, int displayPixelWidth, int displayPixelHeight, int maxLongEdge);

} // namespace cppdesk
