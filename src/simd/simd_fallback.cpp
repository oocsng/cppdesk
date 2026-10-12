#include "simd_kernels.hpp"

#if !defined(_WIN32)

extern "C" {

int cppdesk_avx2_tile_diff(
    const uint8_t* prevTilePtr,
    const uint8_t* currTilePtr,
    int strideBytes,
    int widthBytes,
    int heightRows)
{
    return cppdesk::SimdKernels::scalarTileDiff(prevTilePtr, currTilePtr, strideBytes, widthBytes, heightRows) ? 1 : 0;
}

int aerodesk_avx2_tile_diff(
    const uint8_t* prevTilePtr,
    const uint8_t* currTilePtr,
    int strideBytes,
    int widthBytes,
    int heightRows)
{
    return cppdesk_avx2_tile_diff(prevTilePtr, currTilePtr, strideBytes, widthBytes, heightRows);
}

uint64_t cppdesk_avx2_hash_tile(
    const uint8_t* tilePtr,
    int strideBytes,
    int widthBytes,
    int heightRows)
{
    if (!tilePtr || widthBytes <= 0 || heightRows <= 0) {
        return 0xcbf29ce484222325ULL;
    }
    uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a offset basis
    const uint64_t prime = 0x100000001b3ULL; // FNV-1a prime

    for (int r = 0; r < heightRows; ++r) {
        const uint8_t* row = tilePtr + static_cast<size_t>(r) * strideBytes;
        for (int c = 0; c < widthBytes; ++c) {
            uint8_t byteVal = row[c];
            // Normalize alpha byte in 32-bit BGRA (every 4th byte, index % 4 == 3)
            if ((c & 3) == 3) {
                byteVal = 0xFF;
            }
            hash ^= static_cast<uint64_t>(byteVal);
            hash *= prime;
        }
    }
    hash |= 1ULL; // Non-zero sentinel
    return hash;
}

uint64_t aerodesk_avx2_hash_tile(
    const uint8_t* tilePtr,
    int strideBytes,
    int widthBytes,
    int heightRows)
{
    return cppdesk_avx2_hash_tile(tilePtr, strideBytes, widthBytes, heightRows);
}

void cppdesk_avx2_xor_blocks32(
    uint8_t* data,
    const uint8_t* ksBuffer,
    size_t blocks32)
{
    if (!data || !ksBuffer || blocks32 == 0) return;
    size_t totalBytes = blocks32 * 32;
    for (size_t i = 0; i < totalBytes; ++i) {
        data[i] ^= ksBuffer[i];
    }
}

void aerodesk_avx2_xor_blocks32(
    uint8_t* data,
    const uint8_t* ksBuffer,
    size_t blocks32)
{
    cppdesk_avx2_xor_blocks32(data, ksBuffer, blocks32);
}

} // extern "C"

#endif // !defined(_WIN32)
