#include "qr_matrix.hpp"

#include <cstddef>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace cppdesk {

namespace {

struct VersionInfo {
    int version;
    int size;
    int totalCodewords;
    int dataCodewords;
    int ecCodewords;
    int numBlocks;
    int dataPerBlock;
    int ecPerBlock;
    int maxDataBytes;
};

// Error Correction Level L configurations for Versions 1-6
const VersionInfo kVersionTable[7] = {
    {0,  0,   0,   0,  0, 0,  0,  0,   0}, // 1-indexed placeholder
    {1, 21,  26,  19,  7, 1, 19,  7,  17},
    {2, 25,  44,  34, 10, 1, 34, 10,  32},
    {3, 29,  70,  55, 15, 1, 55, 15,  53},
    {4, 33, 100,  80, 20, 1, 80, 20,  78},
    {5, 37, 134, 108, 26, 1, 108, 26, 106},
    {6, 41, 172, 136, 36, 2,  68, 18, 134}
};

class GaloisField {
public:
    GaloisField() {
        int val = 1;
        for (int i = 0; i < 255; ++i) {
            expTable_[i] = static_cast<uint8_t>(val);
            logTable_[val] = static_cast<uint8_t>(i);
            val <<= 1;
            if (val & 0x100) {
                val ^= 285; // primitive polynomial x^8 + x^4 + x^3 + x^2 + 1
            }
        }
        expTable_[255] = expTable_[0];
        logTable_[0] = 0;
    }

    uint8_t multiply(uint8_t a, uint8_t b) const {
        if (a == 0 || b == 0) return 0;
        return expTable_[(logTable_[a] + logTable_[b]) % 255];
    }

    std::vector<uint8_t> buildGeneratorPoly(int degree) const {
        std::vector<uint8_t> poly(degree + 1, 0);
        poly[0] = 1;
        for (int i = 0; i < degree; ++i) {
            uint8_t factor = expTable_[i];
            for (int j = i; j >= 0; --j) {
                poly[j + 1] ^= multiply(poly[j], factor);
            }
        }
        return poly;
    }

    std::vector<uint8_t> computeErrorCorrection(const std::vector<uint8_t>& data, int ecCount) const {
        std::vector<uint8_t> gen = buildGeneratorPoly(ecCount);
        std::vector<uint8_t> remainder(ecCount, 0);

        for (size_t i = 0; i < data.size(); ++i) {
            uint8_t lead = data[i] ^ remainder[0];
            for (int j = 0; j < ecCount - 1; ++j) {
                remainder[j] = remainder[j + 1] ^ multiply(lead, gen[j + 1]);
            }
            remainder[ecCount - 1] = multiply(lead, gen[ecCount]);
        }
        return remainder;
    }

private:
    uint8_t expTable_[256] = {};
    uint8_t logTable_[256] = {};
};

const GaloisField g_gf;

uint16_t computeFormatBits(int mask) {
    uint32_t data = (1 << 3) | (mask & 7); // ECL L = 01 binary
    uint32_t rem = data << 10;
    for (int i = 14; i >= 10; --i) {
        if (rem & (1 << i)) {
            rem ^= (0x537 << (i - 10));
        }
    }
    uint32_t format = ((data << 10) | rem) ^ 0x5412;
    return static_cast<uint16_t>(format);
}

void applyFinderPattern(std::vector<uint8_t>& modules, std::vector<uint8_t>& reserved, int N, int ox, int oy) {
    for (int dy = -1; dy <= 7; ++dy) {
        for (int dx = -1; dx <= 7; ++dx) {
            int x = ox + dx;
            int y = oy + dy;
            if (x < 0 || x >= N || y < 0 || y >= N) continue;
            reserved[y * N + x] = 1;

            if (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6) {
                if (dx == 0 || dx == 6 || dy == 0 || dy == 6) {
                    modules[y * N + x] = 1;
                } else if (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4) {
                    modules[y * N + x] = 1;
                } else {
                    modules[y * N + x] = 0;
                }
            } else {
                modules[y * N + x] = 0; // white separator
            }
        }
    }
}

void applyAlignmentPattern(std::vector<uint8_t>& modules, std::vector<uint8_t>& reserved, int N, int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            int x = cx + dx;
            int y = cy + dy;
            if (x < 0 || x >= N || y < 0 || y >= N) continue;
            reserved[y * N + x] = 1;
            if (std::abs(dx) == 2 || std::abs(dy) == 2 || (dx == 0 && dy == 0)) {
                modules[y * N + x] = 1;
            } else {
                modules[y * N + x] = 0;
            }
        }
    }
}

int evaluatePenalty(const std::vector<uint8_t>& m, int N) {
    int penalty = 0;

    // N1: Horizontal and vertical runs of >= 5 modules
    for (int y = 0; y < N; ++y) {
        int runLen = 1;
        for (int x = 1; x < N; ++x) {
            if (m[y * N + x] == m[y * N + (x - 1)]) {
                runLen++;
            } else {
                if (runLen >= 5) penalty += 3 + (runLen - 5);
                runLen = 1;
            }
        }
        if (runLen >= 5) penalty += 3 + (runLen - 5);
    }

    for (int x = 0; x < N; ++x) {
        int runLen = 1;
        for (int y = 1; y < N; ++y) {
            if (m[y * N + x] == m[(y - 1) * N + x]) {
                runLen++;
            } else {
                if (runLen >= 5) penalty += 3 + (runLen - 5);
                runLen = 1;
            }
        }
        if (runLen >= 5) penalty += 3 + (runLen - 5);
    }

    // N2: 2x2 blocks of same color
    for (int y = 0; y < N - 1; ++y) {
        for (int x = 0; x < N - 1; ++x) {
            uint8_t c = m[y * N + x];
            if (c == m[y * N + (x + 1)] &&
                c == m[(y + 1) * N + x] &&
                c == m[(y + 1) * N + (x + 1)]) {
                penalty += 3;
            }
        }
    }

    // N3: Finder-like sequences (1 0 1 1 1 0 1 0 0 0 0 or 0 0 0 0 1 0 1 1 1 0 1)
    const uint8_t pat1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
    const uint8_t pat2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};

    for (int y = 0; y < N; ++y) {
        for (int x = 0; x <= N - 11; ++x) {
            bool match1 = true, match2 = true;
            for (int k = 0; k < 11; ++k) {
                if (m[y * N + (x + k)] != pat1[k]) match1 = false;
                if (m[y * N + (x + k)] != pat2[k]) match2 = false;
            }
            if (match1) penalty += 40;
            if (match2) penalty += 40;
        }
    }

    for (int x = 0; x < N; ++x) {
        for (int y = 0; y <= N - 11; ++y) {
            bool match1 = true, match2 = true;
            for (int k = 0; k < 11; ++k) {
                if (m[(y + k) * N + x] != pat1[k]) match1 = false;
                if (m[(y + k) * N + x] != pat2[k]) match2 = false;
            }
            if (match1) penalty += 40;
            if (match2) penalty += 40;
        }
    }

    // N4: Ratio of dark modules
    int darkCount = 0;
    for (int i = 0; i < N * N; ++i) {
        if (m[i]) darkCount++;
    }
    int pct = (darkCount * 100) / (N * N);
    int diff = std::abs(pct - 50);
    penalty += (diff / 5) * 10;

    return penalty;
}

void stampFormatInfo(std::vector<uint8_t>& target, int N, uint16_t formatBits) {
    // 15 format modules
    // Top-left finder
    target[8 * N + 0] = (formatBits >> 14) & 1;
    target[8 * N + 1] = (formatBits >> 13) & 1;
    target[8 * N + 2] = (formatBits >> 12) & 1;
    target[8 * N + 3] = (formatBits >> 11) & 1;
    target[8 * N + 4] = (formatBits >> 10) & 1;
    target[8 * N + 5] = (formatBits >> 9) & 1;
    target[8 * N + 7] = (formatBits >> 8) & 1;
    target[8 * N + 8] = (formatBits >> 7) & 1;
    target[7 * N + 8] = (formatBits >> 6) & 1;
    target[5 * N + 8] = (formatBits >> 5) & 1;
    target[4 * N + 8] = (formatBits >> 4) & 1;
    target[3 * N + 8] = (formatBits >> 3) & 1;
    target[2 * N + 8] = (formatBits >> 2) & 1;
    target[1 * N + 8] = (formatBits >> 1) & 1;
    target[0 * N + 8] = (formatBits >> 0) & 1;

    // Split copy around other finders
    // Bottom-left finder (bits 14 down to 8)
    for (int k = 0; k < 7; ++k) {
        target[(N - 1 - k) * N + 8] = (formatBits >> (14 - k)) & 1;
    }

    // Top-right finder (bits 7 down to 0)
    for (int k = 0; k < 8; ++k) {
        target[8 * N + (N - 8 + k)] = (formatBits >> (7 - k)) & 1;
    }
}

} // namespace

QrMatrix::QrMatrix(int size)
    : size_(size)
    , modules_(size * size, 0)
{
}

bool QrMatrix::getModule(int x, int y) const {
    if (x < 0 || x >= size_ || y < 0 || y >= size_) {
        return false;
    }
    return modules_[y * size_ + x] != 0;
}

void QrMatrix::setModule(int x, int y, bool isDark) {
    if (x >= 0 && x < size_ && y >= 0 && y < size_) {
        modules_[y * size_ + x] = isDark ? 1 : 0;
    }
}

QrMatrix QrMatrix::generate(const std::string& text, int minVersion, int maxVersion) {
    minVersion = std::clamp(minVersion, 1, 6);
    maxVersion = std::clamp(maxVersion, minVersion, 6);

    if (text.size() > static_cast<size_t>(kVersionTable[maxVersion].maxDataBytes)) {
        return QrMatrix{};
    }

    int textLen = static_cast<int>(text.length());
    int chosenVersion = maxVersion;
    for (int v = minVersion; v <= maxVersion; ++v) {
        if (textLen <= kVersionTable[v].maxDataBytes) {
            chosenVersion = v;
            break;
        }
    }

    const VersionInfo& vinfo = kVersionTable[chosenVersion];
    int N = vinfo.size;

    // 1. Bitstream Construction
    std::vector<uint8_t> dataCodewords;
    dataCodewords.reserve(vinfo.dataCodewords);

    uint32_t bitBuffer = 0;
    int bitsCount = 0;

    auto pushBits = [&](uint32_t val, int count) {
        bitBuffer = (bitBuffer << count) | (val & ((1U << count) - 1));
        bitsCount += count;
        while (bitsCount >= 8) {
            bitsCount -= 8;
            dataCodewords.push_back(static_cast<uint8_t>((bitBuffer >> bitsCount) & 0xFF));
        }
    };

    // Mode: Byte (0100)
    pushBits(0x04, 4);
    // Character count indicator (8 bits for V1-6 in Byte mode)
    pushBits(static_cast<uint32_t>(textLen), 8);
    // Payload bytes
    for (char c : text) {
        pushBits(static_cast<uint8_t>(c), 8);
    }
    // Terminator (up to 4 zero bits)
    int maxDataBits = vinfo.dataCodewords * 8;
    int currentBits = static_cast<int>(dataCodewords.size()) * 8 + bitsCount;
    int termBits = std::min(4, maxDataBits - currentBits);
    if (termBits > 0) {
        pushBits(0, termBits);
    }
    // Pad to byte boundary
    if (bitsCount > 0) {
        pushBits(0, 8 - bitsCount);
    }
    // Pad bytes 0xEC and 0x11
    uint8_t padByte = 0xEC;
    while (static_cast<int>(dataCodewords.size()) < vinfo.dataCodewords) {
        dataCodewords.push_back(padByte);
        padByte = (padByte == 0xEC) ? 0x11 : 0xEC;
    }

    // 2. Reed-Solomon Error Correction and Interleaving
    std::vector<uint8_t> finalCodewords;
    finalCodewords.reserve(vinfo.totalCodewords);

    if (vinfo.numBlocks == 1) {
        std::vector<uint8_t> ec = g_gf.computeErrorCorrection(dataCodewords, vinfo.ecCodewords);
        finalCodewords.insert(finalCodewords.end(), dataCodewords.begin(), dataCodewords.end());
        finalCodewords.insert(finalCodewords.end(), ec.begin(), ec.end());
    } else {
        // Multi-block (Version 6 has 2 blocks of 68 data / 18 EC)
        int bcount = vinfo.numBlocks;
        int dPerBlock = vinfo.dataPerBlock;
        int ecPerBlock = vinfo.ecPerBlock;

        std::vector<std::vector<uint8_t>> dBlocks(bcount);
        std::vector<std::vector<uint8_t>> ecBlocks(bcount);

        for (int b = 0; b < bcount; ++b) {
            dBlocks[b].assign(
                dataCodewords.begin() + static_cast<std::ptrdiff_t>(b) * dPerBlock,
                dataCodewords.begin() + static_cast<std::ptrdiff_t>(b + 1) * dPerBlock
            );
            ecBlocks[b] = g_gf.computeErrorCorrection(dBlocks[b], ecPerBlock);
        }

        // Interleave data codewords
        for (int i = 0; i < dPerBlock; ++i) {
            for (int b = 0; b < bcount; ++b) {
                finalCodewords.push_back(dBlocks[b][i]);
            }
        }
        // Interleave EC codewords
        for (int i = 0; i < ecPerBlock; ++i) {
            for (int b = 0; b < bcount; ++b) {
                finalCodewords.push_back(ecBlocks[b][i]);
            }
        }
    }

    // 3. Matrix Setup & Function Patterns
    const size_t cells = static_cast<size_t>(N) * N;
    std::vector<uint8_t> baseModules(cells, 0);
    std::vector<uint8_t> reserved(cells, 0);

    // Finders
    applyFinderPattern(baseModules, reserved, N, 0, 0);
    applyFinderPattern(baseModules, reserved, N, N - 7, 0);
    applyFinderPattern(baseModules, reserved, N, 0, N - 7);

    // Alignment patterns for Version >= 2
    if (chosenVersion >= 2) {
        int alignCenter = 4 * chosenVersion + 10;
        applyAlignmentPattern(baseModules, reserved, N, alignCenter, alignCenter);
    }

    // Timing patterns
    for (int i = 8; i < N - 8; ++i) {
        // Horizontal row 6
        reserved[6 * N + i] = 1;
        baseModules[6 * N + i] = (i % 2 == 0) ? 1 : 0;

        // Vertical col 6
        reserved[i * N + 6] = 1;
        baseModules[i * N + 6] = (i % 2 == 0) ? 1 : 0;
    }

    // Dark module at (8, 4V + 9)
    int darkModY = 4 * chosenVersion + 9;
    reserved[darkModY * N + 8] = 1;
    baseModules[darkModY * N + 8] = 1;

    // Reserve format information modules
    for (int i = 0; i < 9; ++i) {
        reserved[8 * N + i] = 1;
        reserved[i * N + 8] = 1;
    }
    for (int i = N - 8; i < N; ++i) {
        reserved[8 * N + i] = 1;
    }
    for (int i = N - 7; i < N; ++i) {
        reserved[i * N + 8] = 1;
    }

    // 4. Zig-zag Data Module Traversal
    std::vector<uint8_t> dataModules = baseModules;
    int col = N - 1;
    bool upward = true;
    size_t bitIdx = 0;
    size_t totalBits = finalCodewords.size() * 8;

    while (col > 0) {
        if (col == 6) {
            col--; // Skip vertical timing column
        }
        for (int step = 0; step < N; ++step) {
            int y = upward ? (N - 1 - step) : step;
            for (int c = 0; c < 2; ++c) {
                int x = col - c;
                if (reserved[y * N + x]) continue;
                bool bitVal = false;
                if (bitIdx < totalBits) {
                    size_t bytePos = bitIdx / 8;
                    int bitOffset = 7 - static_cast<int>(bitIdx % 8);
                    bitVal = ((finalCodewords[bytePos] >> bitOffset) & 1) != 0;
                    bitIdx++;
                }
                dataModules[y * N + x] = bitVal ? 1 : 0;
            }
        }
        col -= 2;
        upward = !upward;
    }

    // 5. Mask Evaluation & Selection
    int bestScore = 0x7FFFFFFF;
    std::vector<uint8_t> bestModules;

    for (int mask = 0; mask < 8; ++mask) {
        std::vector<uint8_t> candidate = dataModules;

        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                if (reserved[y * N + x]) continue;
                bool maskBit = false;
                switch (mask) {
                    case 0: maskBit = ((y + x) % 2 == 0); break;
                    case 1: maskBit = (y % 2 == 0); break;
                    case 2: maskBit = (x % 3 == 0); break;
                    case 3: maskBit = ((y + x) % 3 == 0); break;
                    case 4: maskBit = (((y / 2) + (x / 3)) % 2 == 0); break;
                    case 5: maskBit = (((y * x) % 2 + (y * x) % 3) == 0); break;
                    case 6: maskBit = ((((y * x) % 2 + (y * x) % 3) % 2) == 0); break;
                    case 7: maskBit = ((((y + x) % 2 + (y * x) % 3) % 2) == 0); break;
                }
                candidate[y * N + x] ^= (maskBit ? 1 : 0);
            }
        }

        uint16_t formatBits = computeFormatBits(mask);
        stampFormatInfo(candidate, N, formatBits);

        int score = evaluatePenalty(candidate, N);
        if (score < bestScore) {
            bestScore = score;
            bestModules = std::move(candidate);
        }
    }

    QrMatrix result(N);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            result.setModule(x, y, bestModules[y * N + x] != 0);
        }
    }

    return result;
}

} // namespace cppdesk
