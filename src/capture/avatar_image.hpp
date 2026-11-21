#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace cppdesk::avatar {

constexpr int kSize = 96;                       // stored/wire pictures are kSize x kSize
constexpr size_t kMaxWireBytes = 16 * 1024;     // largest picture accepted from a peer
constexpr uint64_t kMaxInputBytes = 10ull << 20; // largest file the picker will read

// Decodes a JPEG file, centre-crops to a square, scales to kSize and re-encodes at quality 85.
// nullopt on any failure or when the result exceeds kMaxWireBytes.
std::optional<std::vector<uint8_t>> normaliseJpeg(const std::vector<uint8_t>& fileBytes);

// Trust-boundary check for a picture received from a peer: 1..kMaxWireBytes bytes, decodes, 1..256 px each side.
bool validateWireJpeg(const std::vector<uint8_t>& jpeg);

// Decodes to premultiplied BGRA with alpha 0 outside the inscribed circle and a 1px antialiased edge.
bool decodeCircleBgra(const std::vector<uint8_t>& jpeg, std::vector<uint8_t>& outBgra, int& outW, int& outH);

} // namespace cppdesk::avatar
