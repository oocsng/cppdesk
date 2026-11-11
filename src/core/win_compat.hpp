#pragma once

// Stand-ins for the handful of Win32 calls that portable code paths still make, so a non-Windows build
// does not need an #if at every call site. Windows builds get the real ones from <windows.h>.

#if !defined(_WIN32)

#include <chrono>
#include <cstdint>

using LONG = int32_t;

struct RECT {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
};

struct POINT {
    int32_t x = 0;
    int32_t y = 0;
};

inline uint64_t GetTickCount64() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}


// The BMP header layouts <windows.h> would provide.
#pragma pack(push, 2)
struct BITMAPFILEHEADER {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
};
#pragma pack(pop)
struct BITMAPINFOHEADER {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};

#endif
