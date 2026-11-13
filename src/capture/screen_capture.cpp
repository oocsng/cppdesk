#include "screen_capture.hpp"
#include <cstdlib>
#include "../simd/simd_kernels.hpp"
#if defined(__APPLE__)
#include "mac_screen_capture.hpp"
#endif

#if defined(_WIN32)
#include <windows.h>
#include <shlwapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#endif
#if defined(__APPLE__)
#if __has_include(<CoreGraphics/CoreGraphics.h>)
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <ImageIO/ImageIO.h>
#else
typedef uint32_t CGDirectDisplayID;
typedef struct CGImage* CGImageRef;
typedef struct CGDataProvider* CGDataProviderRef;
typedef const struct __CFData* CFDataRef;
typedef uint32_t CGBitmapInfo;
typedef int32_t CGError;
enum { kCGErrorSuccess = 0 };
static const CGDirectDisplayID kCGDirectMainDisplay = 0;
enum {
    kCGBitmapByteOrderMask = 0x7000,
    kCGBitmapByteOrder32Little = 0x2000
};
extern "C" {
    CGError CGGetActiveDisplayList(uint32_t, CGDirectDisplayID*, uint32_t*);
    CGImageRef CGDisplayCreateImage(CGDirectDisplayID);
    size_t CGImageGetWidth(CGImageRef);
    size_t CGImageGetHeight(CGImageRef);
    CGDataProviderRef CGImageGetDataProvider(CGImageRef);
    CFDataRef CGDataProviderCopyData(CGDataProviderRef);
    const uint8_t* CFDataGetBytePtr(CFDataRef);
    size_t CGImageGetBytesPerRow(CGImageRef);
    size_t CGImageGetBitsPerPixel(CGImageRef);
    CGBitmapInfo CGImageGetBitmapInfo(CGImageRef);
    void CFRelease(const void*);
    bool CGPreflightScreenCaptureAccess(void);
    bool CGRequestScreenCaptureAccess(void);
}
#endif
#elif defined(__linux__) || defined(__unix__)
#if __has_include(<X11/Xlib.h>)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "xlib_threads.hpp"
#endif
#if __has_include(<jpeglib.h>)
#include <jpeglib.h>
#include <csetjmp>

namespace {
struct SafeJpegErrorManager {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void safeJpegErrorExit(j_common_ptr cinfo) {
    auto* myerr = reinterpret_cast<SafeJpegErrorManager*>(cinfo->err);
    longjmp(myerr->setjmp_buffer, 1);
}
} // namespace
#endif
#endif
#include <zstd.h>

#include <cstring>
#include <algorithm>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>

namespace cppdesk {

#if !defined(_WIN32)
#include "../core/win_compat.hpp"
#endif

namespace {

#if defined(_WIN32)
ULONG_PTR g_gdiplusToken = 0;
std::once_flag g_gdiplusInitFlag;
CLSID g_jpegEncoderClsid = {};
bool g_hasJpegClsid = false;

bool findJpegEncoderClsid(CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;

    std::vector<uint8_t> buf(size);
    auto* pImageCodecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if ( std::wcscmp(pImageCodecInfo[j].MimeType, L"image/jpeg") == 0 ) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return true;
        }
    }
    return false;
}
#endif

uint64_t hashTileBgra(const uint8_t* frame, int frameStride, int startX, int startY, int w, int h) {
    const uint8_t* tileTopLeft = frame + static_cast<size_t>(startY) * frameStride + static_cast<size_t>(startX) * 4;
    if (SimdKernels::hasAvx2()) {
        return cppdesk_avx2_hash_tile(tileTopLeft, frameStride, w * 4, h);
    }

    uint64_t hash = 14695981039346656037ULL;
    for (int ry = 0; ry < h; ++ry) {
        const uint64_t* rowPtr = reinterpret_cast<const uint64_t*>(
            frame + (startY + ry) * frameStride + startX * 4
        );
        int qwords = (w * 4) / 8;
        for (int i = 0; i < qwords; ++i) {
            // Mask out unused alpha variations if needed, or hash full BGRA
            uint64_t v = rowPtr[i] | 0xFF000000FF000000ULL;
            hash ^= v;
            hash *= 1099511628211ULL;
        }
        int remBytes = (w * 4) % 8;
        if (remBytes > 0) {
            const uint8_t* tail = reinterpret_cast<const uint8_t*>(rowPtr + qwords);
            for (int b = 0; b < remBytes; ++b) {
                hash ^= tail[b];
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

struct ThreadZstdContext {
    ZSTD_CCtx* cctx = nullptr;
    ZSTD_DCtx* dctx = nullptr;

    ThreadZstdContext() {
        cctx = ZSTD_createCCtx();
        dctx = ZSTD_createDCtx();
    }
    ~ThreadZstdContext() {
        if (cctx) {
            ZSTD_freeCCtx(cctx);
            cctx = nullptr;
        }
        if (dctx) {
            ZSTD_freeDCtx(dctx);
            dctx = nullptr;
        }
    }
};

thread_local ThreadZstdContext tl_zstd;

} // namespace

// ---------------- TileCodec ----------------

static std::mutex g_gdiplusCodecMutex;

void TileCodec::initGdiPlus() {
#if defined(_WIN32)
    std::call_once(g_gdiplusInitFlag, []() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Gdiplus::Ok) {
            g_hasJpegClsid = findJpegEncoderClsid(&g_jpegEncoderClsid);
        }
    });
#endif
}

void TileCodec::shutdownGdiPlus() {
    // Kept alive until process exit to avoid races with background worker threads
}

std::vector<uint8_t> TileCodec::compressZstd(const void* src, size_t srcLen, int level) {
    if (!src || srcLen == 0) return {};
    size_t bound = ZSTD_compressBound(srcLen);
    std::vector<uint8_t> out(bound);
    size_t cSize = tl_zstd.cctx
        ? ZSTD_compressCCtx(tl_zstd.cctx, out.data(), out.size(), src, srcLen, level)
        : ZSTD_compress(out.data(), out.size(), src, srcLen, level);
    if (ZSTD_isError(cSize)) {
        return {};
    }
    out.resize(cSize);
    return out;
}

bool TileCodec::decompressZstd(const void* src, size_t srcLen, void* dst, size_t dstCapacity) {
    if (!src || srcLen == 0 || !dst || dstCapacity == 0) return false;
    size_t dSize = tl_zstd.dctx
        ? ZSTD_decompressDCtx(tl_zstd.dctx, dst, dstCapacity, src, srcLen)
        : ZSTD_decompress(dst, dstCapacity, src, srcLen);
    return !ZSTD_isError(dSize) && dSize == dstCapacity;
}

#if defined(_WIN32)

std::vector<uint8_t> TileCodec::encodeJpeg(const uint8_t* bgra, int width, int height, int quality) {
    std::lock_guard<std::mutex> gdiLock(g_gdiplusCodecMutex);
    initGdiPlus();
    if (!g_hasJpegClsid || !bgra || width <= 0 || height <= 0) return {};

    Gdiplus::Bitmap bmp(
        width,
        height,
        width * 4,
        PixelFormat32bppRGB,
        const_cast<BYTE*>(bgra)
    );

    IStream* pStream = SHCreateMemStream(nullptr, 0);
    if (!pStream) {
        return {};
    }

    ULONG q = static_cast<ULONG>(std::clamp(quality, 15, 100));
    Gdiplus::EncoderParameters params{};
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value = &q;

    std::vector<uint8_t> result;
    if (bmp.Save(pStream, &g_jpegEncoderClsid, &params) == Gdiplus::Ok) {
        STATSTG stat{};
        if (SUCCEEDED(pStream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
            size_t sz = static_cast<size_t>(stat.cbSize.QuadPart);
            result.resize(sz);
            LARGE_INTEGER zero{};
            pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
            ULONG bytesRead = 0;
            pStream->Read(result.data(), static_cast<ULONG>(sz), &bytesRead);
            result.resize(bytesRead);
        }
    }
    pStream->Release();
    return result;
}

bool TileCodec::decodeJpeg(const uint8_t* jpegData, size_t jpegLen, std::vector<uint8_t>& outBgra, int& outW, int& outH) {
    std::lock_guard<std::mutex> gdiLock(g_gdiplusCodecMutex);
    initGdiPlus();
    if (!jpegData || jpegLen == 0) return false;

    IStream* pStream = SHCreateMemStream(jpegData, static_cast<UINT>(jpegLen));
    if (!pStream) return false;

    bool ok = false;
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(pStream, FALSE);
    if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
        outW = static_cast<int>(bmp->GetWidth());
        outH = static_cast<int>(bmp->GetHeight());
        if (outW > 0 && outH > 0) {
            Gdiplus::Rect rect(0, 0, outW, outH);
            Gdiplus::BitmapData bdata{};
            if (bmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bdata) == Gdiplus::Ok) {
                outBgra.resize(static_cast<size_t>(outW) * outH * 4);
                for (int r = 0; r < outH; ++r) {
                    const uint8_t* srcRow = static_cast<const uint8_t*>(bdata.Scan0) + r * bdata.Stride;
                    uint8_t* dstRow = outBgra.data() + static_cast<size_t>(r) * outW * 4;
                    std::memcpy(dstRow, srcRow, static_cast<size_t>(outW) * 4);
                }
                bmp->UnlockBits(&bdata);
                ok = true;
            }
        }
    }
    delete bmp;
    pStream->Release();
    return ok;
}

bool TileCodec::decodeJpegIntoCanvas(
    const uint8_t* jpegData, size_t jpegLen,
    uint8_t* canvasBgra, int canvasW, int canvasH,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    std::lock_guard<std::mutex> gdiLock(g_gdiplusCodecMutex);
    initGdiPlus();
    if (!jpegData || jpegLen == 0 || !canvasBgra || width == 0 || height == 0) return false;
    if (static_cast<int>(x) + width > canvasW || static_cast<int>(y) + height > canvasH) return false;

    IStream* pStream = SHCreateMemStream(jpegData, static_cast<UINT>(jpegLen));
    if (!pStream) return false;

    bool ok = false;
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(pStream, FALSE);
    if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
        int outW = static_cast<int>(bmp->GetWidth());
        int outH = static_cast<int>(bmp->GetHeight());
        if (outW == width && outH == height) {
            Gdiplus::Rect rect(0, 0, outW, outH);
            Gdiplus::BitmapData bdata{};
            if (bmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bdata) == Gdiplus::Ok) {
                for (int r = 0; r < height; ++r) {
                    const uint8_t* srcRow = static_cast<const uint8_t*>(bdata.Scan0) + r * bdata.Stride;
                    uint8_t* dstRow = canvasBgra + (static_cast<size_t>(y + r) * canvasW + x) * 4;
                    SimdKernels::blitRowBgraOpaque(dstRow, srcRow, width);
                }
                bmp->UnlockBits(&bdata);
                ok = true;
            }
        }
    }
    delete bmp;
    pStream->Release();
    return ok;
}

#elif (defined(__linux__) || defined(__unix__)) && __has_include(<jpeglib.h>)

std::vector<uint8_t> TileCodec::encodeJpeg(const uint8_t* bgra, int width, int height, int quality) {
    if (!bgra || width <= 0 || height <= 0) return {};

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char* outbuffer = nullptr;
    unsigned long outsize = 0;
    jpeg_mem_dest(&cinfo, &outbuffer, &outsize);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, std::clamp(quality, 15, 100), TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    std::vector<uint8_t> rowBuffer(width * 3);
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t* srcRow = bgra + cinfo.next_scanline * width * 4;
        for (int i = 0; i < width; ++i) {
            rowBuffer[i * 3 + 0] = srcRow[i * 4 + 2]; // R
            rowBuffer[i * 3 + 1] = srcRow[i * 4 + 1]; // G
            rowBuffer[i * 3 + 2] = srcRow[i * 4 + 0]; // B
        }
        JSAMPROW rowPtr = rowBuffer.data();
        jpeg_write_scanlines(&cinfo, &rowPtr, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    std::vector<uint8_t> result;
    if (outbuffer && outsize > 0) {
        result.assign(outbuffer, outbuffer + outsize);
        free(outbuffer);
    }
    return result;
}

bool TileCodec::decodeJpeg(const uint8_t* jpegData, size_t jpegLen, std::vector<uint8_t>& outBgra, int& outW, int& outH) {
    if (!jpegData || jpegLen == 0) return false;

    struct jpeg_decompress_struct cinfo;
    SafeJpegErrorManager jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = safeJpegErrorExit;
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpegData, jpegLen);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    outW = static_cast<int>(cinfo.output_width);
    outH = static_cast<int>(cinfo.output_height);
    outBgra.resize(static_cast<size_t>(outW) * outH * 4);

    std::vector<uint8_t> rowBuffer(outW * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        int r = static_cast<int>(cinfo.output_scanline);
        JSAMPROW rowPtr = rowBuffer.data();
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);

        uint8_t* dstRow = outBgra.data() + static_cast<size_t>(r) * outW * 4;
        for (int i = 0; i < outW; ++i) {
            dstRow[i * 4 + 0] = rowBuffer[i * 3 + 2]; // B
            dstRow[i * 4 + 1] = rowBuffer[i * 3 + 1]; // G
            dstRow[i * 4 + 2] = rowBuffer[i * 3 + 0]; // R
            dstRow[i * 4 + 3] = 0xFF;                 // A
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

bool TileCodec::decodeJpegIntoCanvas(
    const uint8_t* jpegData, size_t jpegLen,
    uint8_t* canvasBgra, int canvasW, int canvasH,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    if (!jpegData || jpegLen == 0 || !canvasBgra || width == 0 || height == 0) return false;
    if (static_cast<int>(x) + width > canvasW || static_cast<int>(y) + height > canvasH) return false;

    struct jpeg_decompress_struct cinfo;
    SafeJpegErrorManager jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = safeJpegErrorExit;
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpegData, jpegLen);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    if (cinfo.image_width != width || cinfo.image_height != height) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    std::vector<uint8_t> rowBuffer(width * 3);
    std::vector<uint8_t> bgraRow(width * 4);

    while (cinfo.output_scanline < cinfo.output_height) {
        int r = static_cast<int>(cinfo.output_scanline);
        JSAMPROW rowPtr = rowBuffer.data();
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);

        for (int i = 0; i < width; ++i) {
            bgraRow[i * 4 + 0] = rowBuffer[i * 3 + 2]; // B
            bgraRow[i * 4 + 1] = rowBuffer[i * 3 + 1]; // G
            bgraRow[i * 4 + 2] = rowBuffer[i * 3 + 0]; // R
            bgraRow[i * 4 + 3] = 0xFF;                 // A
        }

        uint8_t* dstRow = canvasBgra + (static_cast<size_t>(y + r) * canvasW + x) * 4;
        SimdKernels::blitRowBgraOpaque(dstRow, bgraRow.data(), width);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

#elif defined(__APPLE__) && __has_include(<ImageIO/ImageIO.h>)

// JPEG through ImageIO, so macOS needs no libjpeg. Pixels are BGRA like everywhere else (32-bit little-endian,
// alpha first); decoded images are forced opaque.
std::vector<uint8_t> TileCodec::encodeJpeg(const uint8_t* bgra, int width, int height, int quality) {
    if (!bgra || width <= 0 || height <= 0) return {};
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, bgra, static_cast<size_t>(width) * height * 4, nullptr);
    CGImageRef image = CGImageCreate(width, height, 8, 32, static_cast<size_t>(width) * 4, space,
                                     kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, nullptr,
                                     false, kCGRenderingIntentDefault);
    std::vector<uint8_t> out;
    if (image) {
        CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
        CGImageDestinationRef dest = CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, nullptr);
        if (dest) {
            const float q = std::clamp(quality, 1, 100) / 100.0f;
            CFNumberRef qn = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat32Type, &q);
            const void* keys[] = { kCGImageDestinationLossyCompressionQuality };
            const void* values[] = { qn };
            CFDictionaryRef props = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                       &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CGImageDestinationAddImage(dest, image, props);
            if (CGImageDestinationFinalize(dest)) {
                const uint8_t* p = CFDataGetBytePtr(data);
                out.assign(p, p + CFDataGetLength(data));
            }
            CFRelease(props);
            CFRelease(qn);
            CFRelease(dest);
        }
        CFRelease(data);
        CGImageRelease(image);
    }
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    return out;
}

namespace {

// Decodes a JPEG to opaque BGRA. Bytes come from peers: only real JPEG data goes to ImageIO (not its TIFF, HEIC,
// PSD ... parsers), and an image of another size than expectW x expectH (when given), or an absurd one, is refused
// from its header, before any pixel memory is allocated.
bool decodeJpegImageIO(const uint8_t* jpegData, size_t jpegLen, std::vector<uint8_t>& outBgra, int& outW, int& outH,
                       int expectW, int expectH) {
    constexpr size_t kMaxPixels = 100u * 1000 * 1000;
    if (!jpegData || jpegLen == 0) return false;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, jpegData, static_cast<CFIndex>(jpegLen));
    CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
    CFStringRef type = source ? CGImageSourceGetType(source) : nullptr;
    CGImageRef image = (type && CFEqual(type, CFSTR("public.jpeg"))) ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
    bool ok = false;
    if (image) {
        const int w = static_cast<int>(CGImageGetWidth(image)), h = static_cast<int>(CGImageGetHeight(image));
        const bool sizeOk = w > 0 && h > 0 && static_cast<size_t>(w) * static_cast<size_t>(h) <= kMaxPixels &&
                            (expectW == 0 || w == expectW) && (expectH == 0 || h == expectH);
        if (sizeOk) {
            outBgra.assign(static_cast<size_t>(w) * h * 4, 0);
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            CGContextRef ctx = CGBitmapContextCreate(outBgra.data(), w, h, 8, static_cast<size_t>(w) * 4, space,
                                                     kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
            if (ctx) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
                CGContextRelease(ctx);
                for (size_t i = 3; i < outBgra.size(); i += 4) outBgra[i] = 0xFF;
                outW = w;
                outH = h;
                ok = true;
            }
            CGColorSpaceRelease(space);
        }
        CGImageRelease(image);
    }
    if (source) CFRelease(source);
    CFRelease(data);
    return ok;
}

} // namespace

bool TileCodec::decodeJpeg(const uint8_t* jpegData, size_t jpegLen, std::vector<uint8_t>& outBgra, int& outW, int& outH) {
    return decodeJpegImageIO(jpegData, jpegLen, outBgra, outW, outH, 0, 0);
}

bool TileCodec::decodeJpegIntoCanvas(
    const uint8_t* jpegData, size_t jpegLen,
    uint8_t* canvasBgra, int canvasW, int canvasH,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    if (!jpegData || jpegLen == 0 || !canvasBgra || width == 0 || height == 0) return false;
    if (static_cast<int>(x) + width > canvasW || static_cast<int>(y) + height > canvasH) return false;
    std::vector<uint8_t> tile;
    int w = 0, h = 0;
    if (!decodeJpegImageIO(jpegData, jpegLen, tile, w, h, width, height)) return false;
    for (int r = 0; r < height; ++r) {
        uint8_t* dstRow = canvasBgra + (static_cast<size_t>(y + r) * canvasW + x) * 4;
        SimdKernels::blitRowBgraOpaque(dstRow, tile.data() + static_cast<size_t>(r) * width * 4, width);
    }
    return true;
}

#else

std::vector<uint8_t> TileCodec::encodeJpeg(const uint8_t*, int, int, int) {
    return {};
}

bool TileCodec::decodeJpeg(const uint8_t*, size_t, std::vector<uint8_t>&, int&, int&) {
    return false;
}

bool TileCodec::decodeJpegIntoCanvas(const uint8_t*, size_t, uint8_t*, int, int, uint16_t, uint16_t, uint16_t, uint16_t) {
    return false;
}

#endif

EncodedTile TileCodec::encodeRect(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    const uint8_t* bgraRect,
    QualityPreset preset)
{
    EncodedTile tile;
    tile.x = x;
    tile.y = y;
    tile.width = width;
    tile.height = height;

    size_t rawSize = static_cast<size_t>(width) * height * 4;

    if (preset == QualityPreset::Ultra) {
        auto zstdData = compressZstd(bgraRect, rawSize, 1);
        // Prefer lossless Zstd for UI/text unless the tile is large and poorly compressible
        if (!zstdData.empty() && zstdData.size() <= (rawSize * 45) / 100) {
            tile.encoding = TileEncoding::Zstd;
            tile.data = std::move(zstdData);
            return tile;
        }
        auto jpegData = encodeJpeg(bgraRect, width, height, 90);
        if (!jpegData.empty() && (zstdData.empty() || jpegData.size() < zstdData.size())) {
            tile.encoding = TileEncoding::Jpeg;
            tile.data = std::move(jpegData);
            return tile;
        }
        if (!zstdData.empty()) {
            tile.encoding = TileEncoding::Zstd;
            tile.data = std::move(zstdData);
            return tile;
        }
    } else if (preset == QualityPreset::Balanced) {
        auto zstdData = compressZstd(bgraRect, rawSize, 1);
        if (!zstdData.empty() && zstdData.size() <= (rawSize * 22) / 100) {
            tile.encoding = TileEncoding::Zstd;
            tile.data = std::move(zstdData);
            return tile;
        }
        auto jpegData = encodeJpeg(bgraRect, width, height, 75);
        if (!jpegData.empty() && (zstdData.empty() || jpegData.size() < zstdData.size())) {
            tile.encoding = TileEncoding::Jpeg;
            tile.data = std::move(jpegData);
            return tile;
        }
        if (!zstdData.empty()) {
            tile.encoding = TileEncoding::Zstd;
            tile.data = std::move(zstdData);
            return tile;
        }
    } else {
        // LowBandwidth: JPEG is preferred; only fallback if JPEG failed
        auto jpegData = encodeJpeg(bgraRect, width, height, 50);
        if (!jpegData.empty()) {
            tile.encoding = TileEncoding::Jpeg;
            tile.data = std::move(jpegData);
            return tile;
        }
        auto zstdData = compressZstd(bgraRect, rawSize, 1);
        if (!zstdData.empty()) {
            tile.encoding = TileEncoding::Zstd;
            tile.data = std::move(zstdData);
            return tile;
        }
    }

    tile.encoding = TileEncoding::RawBGRA;
    tile.data.assign(bgraRect, bgraRect + rawSize);
    return tile;
}

bool TileCodec::decodeTileIntoCanvas(
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    TileEncoding encoding,
    const uint8_t* tileData, size_t dataSize,
    uint8_t* canvasBgra,
    int canvasW,
    int canvasH)
{
    if (!canvasBgra || width == 0 || height == 0) return false;
    if (static_cast<int>(x) + width > canvasW ||
        static_cast<int>(y) + height > canvasH) {
        return false;
    }

    size_t rectBytes = static_cast<size_t>(width) * height * 4;
    thread_local std::vector<uint8_t> tl_decodeScratch;
    const uint8_t* pixelSrc = nullptr;

    if (encoding == TileEncoding::Jpeg) {
        return decodeJpegIntoCanvas(tileData, dataSize, canvasBgra, canvasW, canvasH, x, y, width, height);
    } else if (encoding == TileEncoding::Zstd) {
        if (!tileData || dataSize == 0) return false;
        if (tl_decodeScratch.size() < rectBytes) {
            tl_decodeScratch.resize(rectBytes);
        }
        if (!decompressZstd(tileData, dataSize, tl_decodeScratch.data(), rectBytes)) {
            return false;
        }
        pixelSrc = tl_decodeScratch.data();
    } else if (encoding == TileEncoding::RawBGRA) {
        if (!tileData || dataSize != rectBytes) return false;
        pixelSrc = tileData;
    } else {
        return false;
    }

    // Single-pass AVX2 row blit into canvas with opaque alpha (0xFF) enforcement
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (int r = 0; r < height; ++r) {
        uint8_t* dstRow = canvasBgra + (static_cast<size_t>(y + r) * canvasW + x) * 4;
        const uint8_t* srcRow = pixelSrc + static_cast<size_t>(r) * rowBytes;
        SimdKernels::blitRowBgraOpaque(dstRow, srcRow, width);
    }
    return true;
}

bool TileCodec::decodeTileIntoCanvas(
    const EncodedTile& tile,
    uint8_t* canvasBgra,
    int canvasW,
    int canvasH)
{
    return decodeTileIntoCanvas(
        tile.x, tile.y, tile.width, tile.height,
        tile.encoding,
        tile.data.data(), tile.data.size(),
        canvasBgra, canvasW, canvasH
    );
}

// ---------------- TileThreadPool (Option 2A) ----------------

TileThreadPool& TileThreadPool::instance() {
    static TileThreadPool pool;
    return pool;
}

TileThreadPool::TileThreadPool() {
    unsigned int hw = std::thread::hardware_concurrency();
    size_t numWorkers = std::clamp(hw, 2u, 8u);
    workers_.reserve(numWorkers);
    for (size_t i = 0; i < numWorkers; ++i) {
        workers_.emplace_back(&TileThreadPool::workerLoop, this);
    }
}

TileThreadPool::~TileThreadPool() {
    stop_.store(true);
    cvTask_.notify_all();
    for (auto& t : workers_) {
        try {
            if (t.joinable()) {
                t.join();
            }
        } catch (...) {}
    }
}

void TileThreadPool::workerLoop() {
    thread_local std::vector<uint8_t> workerScratch;
    while (!stop_.load()) {
        RectTask* encodeTask = nullptr;
        DecodeTask* decodeTask = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cvTask_.wait(lock, [&]() {
                size_t total = activeBatch_.size() + activeDecodeBatch_.size();
                return stop_.load() || nextTaskIdx_.load() < total;
            });
            if (stop_.load()) break;

            size_t idx = nextTaskIdx_.fetch_add(1);
            if (idx < activeBatch_.size()) {
                encodeTask = activeBatch_[idx];
            } else if (idx - activeBatch_.size() < activeDecodeBatch_.size()) {
                decodeTask = activeDecodeBatch_[idx - activeBatch_.size()];
            }
        }

        if (encodeTask) {
            const uint8_t* pixels = nullptr;
            if (!encodeTask->bgraPixels.empty()) {
                pixels = encodeTask->bgraPixels.data();
            } else if (encodeTask->frameBase) {
                size_t rawSize = static_cast<size_t>(encodeTask->rw) * encodeTask->rh * 4;
                if (workerScratch.size() < rawSize) {
                    workerScratch.resize(rawSize);
                }
                for (int r = 0; r < encodeTask->rh; ++r) {
                    const uint8_t* srcRow = encodeTask->frameBase + static_cast<size_t>(encodeTask->ry + r) * encodeTask->frameStride + static_cast<size_t>(encodeTask->rx) * 4;
                    uint8_t* dstRow = workerScratch.data() + static_cast<size_t>(r) * encodeTask->rw * 4;
                    std::memcpy(dstRow, srcRow, static_cast<size_t>(encodeTask->rw) * 4);
                }
                pixels = workerScratch.data();
            }

            if (pixels) {
                encodeTask->result = TileCodec::encodeRect(
                    encodeTask->rx, encodeTask->ry, encodeTask->rw, encodeTask->rh,
                    pixels, encodeTask->preset
                );
            }
        } else if (decodeTask) {
            if (decodeTask->data && decodeTask->canvasBgra) {
                TileCodec::decodeTileIntoCanvas(
                    decodeTask->rx, decodeTask->ry, decodeTask->rw, decodeTask->rh,
                    decodeTask->encoding,
                    decodeTask->data, decodeTask->dataSize,
                    decodeTask->canvasBgra, decodeTask->canvasW, decodeTask->canvasH
                );
            }
        }

        if ((encodeTask || decodeTask) && remainingTasks_.fetch_sub(1) == 1) {
            std::lock_guard<std::mutex> lock(mutex_);
            cvDone_.notify_all();
        }
    }
}

void TileThreadPool::parallelEncode(std::vector<RectTask>& tasks) {
    if (tasks.empty()) return;
    if (tasks.size() == 1 || workers_.empty()) {
        const uint8_t* pixels = nullptr;
        thread_local std::vector<uint8_t> localScratch;
        if (!tasks[0].bgraPixels.empty()) {
            pixels = tasks[0].bgraPixels.data();
        } else if (tasks[0].frameBase) {
            size_t rawSize = static_cast<size_t>(tasks[0].rw) * tasks[0].rh * 4;
            if (localScratch.size() < rawSize) {
                localScratch.resize(rawSize);
            }
            for (int r = 0; r < tasks[0].rh; ++r) {
                const uint8_t* srcRow = tasks[0].frameBase + static_cast<size_t>(tasks[0].ry + r) * tasks[0].frameStride + static_cast<size_t>(tasks[0].rx) * 4;
                uint8_t* dstRow = localScratch.data() + static_cast<size_t>(r) * tasks[0].rw * 4;
                std::memcpy(dstRow, srcRow, static_cast<size_t>(tasks[0].rw) * 4);
            }
            pixels = localScratch.data();
        }
        if (pixels) {
            tasks[0].result = TileCodec::encodeRect(
                tasks[0].rx, tasks[0].ry, tasks[0].rw, tasks[0].rh,
                pixels, tasks[0].preset
            );
        }
        return;
    }

    std::lock_guard<std::mutex> dispatchLock(dispatchMutex_);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        activeBatch_.clear();
        activeDecodeBatch_.clear();
        activeBatch_.reserve(tasks.size());
        for (auto& t : tasks) {
            activeBatch_.push_back(&t);
        }
        nextTaskIdx_.store(0);
        remainingTasks_.store(tasks.size());
        cvTask_.notify_all();

        cvDone_.wait(lock, [&]() {
            return remainingTasks_.load() == 0;
        });
        activeBatch_.clear();
    }
}

void TileThreadPool::parallelDecode(std::vector<DecodeTask>& tasks) {
    if (tasks.empty()) return;
    if (tasks.size() == 1 || workers_.empty()) {
        for (auto& t : tasks) {
            if (t.data && t.canvasBgra) {
                TileCodec::decodeTileIntoCanvas(
                    t.rx, t.ry, t.rw, t.rh,
                    t.encoding,
                    t.data, t.dataSize,
                    t.canvasBgra, t.canvasW, t.canvasH
                );
            }
        }
        return;
    }

    std::lock_guard<std::mutex> dispatchLock(dispatchMutex_);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        activeBatch_.clear();
        activeDecodeBatch_.clear();
        activeDecodeBatch_.reserve(tasks.size());
        for (auto& t : tasks) {
            activeDecodeBatch_.push_back(&t);
        }
        nextTaskIdx_.store(0);
        remainingTasks_.store(tasks.size());
        cvTask_.notify_all();

        cvDone_.wait(lock, [&]() {
            return remainingTasks_.load() == 0;
        });
        activeDecodeBatch_.clear();
    }
}

// ---------------- ScreenCapturer::DxgiImpl ----------------

struct ScreenCapturer::DxgiImpl {
#if defined(_WIN32)
    ID3D11Device*           device = nullptr;
    ID3D11DeviceContext*    context = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D*        stagingTex = nullptr;
#endif
    int                     texW = 0;
    int                     texH = 0;
    std::vector<uint8_t>    metadataBuf;

    void release() {
#if defined(_WIN32)
        if (stagingTex) { stagingTex->Release(); stagingTex = nullptr; }
        if (duplication) { duplication->Release(); duplication = nullptr; }
        if (context) { context->Release(); context = nullptr; }
        if (device) { device->Release(); device = nullptr; }
#endif
        texW = 0;
        texH = 0;
        metadataBuf.clear();
    }

    ~DxgiImpl() { release(); }
};

ScreenCapturer::ScreenCapturer()
    : dxgi_(std::make_unique<DxgiImpl>())
{
    TileCodec::initGdiPlus();
    enumerateMonitors();
    selectMonitor(0);
}

ScreenCapturer::~ScreenCapturer() {
    releaseDxgi();
    releaseGdiResources();
}

std::vector<MonitorDesc> ScreenCapturer::enumerateMonitors() {
    monitors_.clear();

#if defined(_WIN32)
    struct EnumCtx {
        std::vector<MonitorDesc>* list;
    } ctx{ &monitors_ };

    EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<EnumCtx*>(lParam);
            MONITORINFOEXA mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoA(hMon, &mi)) {
                MonitorDesc d;
                d.index = static_cast<int32_t>(c->list->size());
                d.x = mi.rcMonitor.left;
                d.y = mi.rcMonitor.top;
                d.width = mi.rcMonitor.right - mi.rcMonitor.left;
                d.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
                d.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                d.name = "Display " + std::to_string(d.index + 1) +
                         " (" + std::to_string(d.width) + "x" + std::to_string(d.height) + ")";
                d.isVirtual = false;
                d.virtualId = 0;
                c->list->push_back(d);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx)
    );
#elif defined(__APPLE__)
    uint32_t maxDisplays = 32;
    CGDirectDisplayID displayIds[32];
    uint32_t numDisplays = 0;
    if (CGGetActiveDisplayList(maxDisplays, displayIds, &numDisplays) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < numDisplays; ++i) {
            CGRect bounds = CGDisplayBounds(displayIds[i]);
            MonitorDesc d;
            d.index = static_cast<int32_t>(i);
            d.x = static_cast<int32_t>(bounds.origin.x);
            d.y = static_cast<int32_t>(bounds.origin.y);
            d.width = static_cast<int32_t>(bounds.size.width);
            d.height = static_cast<int32_t>(bounds.size.height);
            d.isPrimary = CGDisplayIsMain(displayIds[i]);
            d.name = "Display " + std::to_string(d.index + 1) +
                     " (" + std::to_string(d.width) + "x" + std::to_string(d.height) + ")";
            d.isVirtual = false;
            d.virtualId = 0;
            monitors_.push_back(d);
        }
    }
#elif defined(__linux__) || defined(__unix__)
#if __has_include(<X11/Xlib.h>)
    ensureXlibThreads();
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy) {
        int screen = DefaultScreen(dpy);
        Window root = RootWindow(dpy, screen);
#if __has_include(<X11/extensions/Xrandr.h>)
        XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
        if (res) {
            RROutput primaryOutput = XRRGetOutputPrimary(dpy, root);
            int validIdx = 0;
            for (int i = 0; i < res->noutput; ++i) {
                XRROutputInfo* outputInfo = XRRGetOutputInfo(dpy, res, res->outputs[i]);
                if (outputInfo && outputInfo->connection == RR_Connected && outputInfo->crtc) {
                    XRRCrtcInfo* crtcInfo = XRRGetCrtcInfo(dpy, res, outputInfo->crtc);
                    if (crtcInfo && crtcInfo->width > 0 && crtcInfo->height > 0) {
                        MonitorDesc d;
                        d.index = validIdx++;
                        d.x = crtcInfo->x;
                        d.y = crtcInfo->y;
                        d.width = static_cast<int32_t>(crtcInfo->width);
                        d.height = static_cast<int32_t>(crtcInfo->height);
                        d.isPrimary = (primaryOutput != 0) ? (res->outputs[i] == primaryOutput) : (d.index == 0);
                        d.name = outputInfo->name ? std::string(outputInfo->name) : ("Display " + std::to_string(d.index + 1));
                        d.isVirtual = false;
                        d.virtualId = 0;
                        monitors_.push_back(d);
                    }
                    if (crtcInfo) XRRFreeCrtcInfo(crtcInfo);
                }
                if (outputInfo) XRRFreeOutputInfo(outputInfo);
            }
            XRRFreeScreenResources(res);
        }
#endif
        if (monitors_.empty()) {
            MonitorDesc d;
            d.index = 0;
            d.x = 0;
            d.y = 0;
            d.width = DisplayWidth(dpy, screen);
            d.height = DisplayHeight(dpy, screen);
            d.isPrimary = true;
            d.name = "X11 Display (" + std::to_string(d.width) + "x" + std::to_string(d.height) + ")";
            d.isVirtual = false;
            d.virtualId = 0;
            monitors_.push_back(d);
        }
        XCloseDisplay(dpy);
    }
#endif
#endif

    // If host has 0 physical monitors and auto-headless is active, provision fallback virtual display
    if (monitors_.empty() && autoHeadless_) {
        VirtualDisplayManager::instance().ensureHeadlessDisplay();
    }

    // Merge active virtual displays
    auto vDisplays = VirtualDisplayManager::instance().activeDisplays();
    for (const auto& vd : vDisplays) {
        MonitorDesc d;
        d.index = static_cast<int32_t>(monitors_.size());
        d.x = vd.x;
        d.y = vd.y;
        d.width = static_cast<int32_t>(vd.width);
        d.height = static_cast<int32_t>(vd.height);
        d.isPrimary = monitors_.empty();
        d.name = vd.name;
        d.isVirtual = true;
        d.virtualId = vd.id;
        monitors_.push_back(d);
    }

    if (monitors_.empty()) {
        MonitorDesc fallback;
        fallback.index = 0;
        fallback.x = 0;
        fallback.y = 0;
#if defined(_WIN32)
        fallback.width = std::max(800, GetSystemMetrics(SM_CXSCREEN));
        fallback.height = std::max(600, GetSystemMetrics(SM_CYSCREEN));
#else
        fallback.width = 800;
        fallback.height = 600;
#endif
        fallback.isPrimary = true;
        fallback.name = "Primary Display";
        fallback.isVirtual = false;
        fallback.virtualId = 0;
        monitors_.push_back(fallback);
    }
    return monitors_;
}

bool ScreenCapturer::selectMonitor(int monitorIndex) {
    if (monitors_.empty()) enumerateMonitors();

    if (monitorIndex == -1) {
        activeMonitorIdx_ = -1;
        activeMonitor_.index = -1;
#if defined(_WIN32)
        activeMonitor_.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        activeMonitor_.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        activeMonitor_.width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
        activeMonitor_.height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
#else
        // The bounding box of every monitor.
        int minX = monitors_[0].x, minY = monitors_[0].y;
        int maxX = monitors_[0].x + monitors_[0].width, maxY = monitors_[0].y + monitors_[0].height;
        for (const auto& m : monitors_) {
            minX = std::min<int>(minX, m.x);
            minY = std::min<int>(minY, m.y);
            maxX = std::max<int>(maxX, m.x + m.width);
            maxY = std::max<int>(maxY, m.y + m.height);
        }
        activeMonitor_.x = minX;
        activeMonitor_.y = minY;
        activeMonitor_.width = std::max(1, maxX - minX);
        activeMonitor_.height = std::max(1, maxY - minY);
#endif
        activeMonitor_.isPrimary = false;
        activeMonitor_.name = "All Displays (Grid View)";
        activeMonitor_.isVirtual = false;
        activeMonitor_.virtualId = 0;

        frameW_ = activeMonitor_.width;
        frameH_ = activeMonitor_.height;
        currentFrame_.assign(static_cast<size_t>(frameW_) * frameH_ * 4, 0);
        prevTileHashes_.clear();
        hasValidFrame_ = false;
        dxgiRecoveryState_ = DxgiRecoveryState::Disabled;
        lastDxgiAttemptTick_ = 0;
        releaseDxgi();
        return true;
    }

    if (monitorIndex < 0 || monitorIndex >= static_cast<int>(monitors_.size())) {
        monitorIndex = 0;
    }
    activeMonitorIdx_ = monitorIndex;
    activeMonitor_ = monitors_[monitorIndex];
    frameW_ = activeMonitor_.width;
    frameH_ = activeMonitor_.height;
    currentFrame_.assign(static_cast<size_t>(frameW_) * frameH_ * 4, 0);
    prevTileHashes_.clear();
    hasValidFrame_ = false;

    if (activeMonitor_.isVirtual) {
        releaseDxgi();
        dxgiRecoveryState_ = DxgiRecoveryState::Disabled;
        lastDxgiAttemptTick_ = 0;
        return true;
    }

    dxgiRecoveryState_ = DxgiRecoveryState::Active;
    lastDxgiAttemptTick_ = 0;

    initDxgiForMonitor(monitorIndex);
    return true;
}

void ScreenCapturer::releaseDxgi() {
    if (dxgi_) dxgi_->release();
    dxgiInitialized_ = false;
}

void ScreenCapturer::triggerDxgiAccessLostForTest() {
    dxgiRecoveryState_ = DxgiRecoveryState::FallbackGdi;
    lastDxgiAttemptTick_ = GetTickCount64();
    releaseDxgi();
}

#if defined(_WIN32)
bool ScreenCapturer::initDxgiForMonitor(int monitorIndex) {
    releaseDxgi();

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        return false;
    }

    IDXGIAdapter1* matchedAdapter = nullptr;
    IDXGIOutput* matchedOutput = nullptr;
    int globalOutIdx = 0;

    for (UINT a = 0; ; ++a) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT o = 0; ; ++o) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (globalOutIdx == monitorIndex) {
                matchedAdapter = adapter;
                matchedOutput = output;
                break;
            }
            output->Release();
            ++globalOutIdx;
        }
        if (matchedOutput) break;
        adapter->Release();
    }
    factory->Release();

    if (!matchedAdapter || !matchedOutput) {
        if (matchedOutput) matchedOutput->Release();
        if (matchedAdapter) matchedAdapter->Release();
        return false;
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL chosenLevel = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDevice(
        matchedAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels,
        4,
        D3D11_SDK_VERSION,
        &dxgi_->device,
        &chosenLevel,
        &dxgi_->context
    );
    matchedAdapter->Release();

    if (FAILED(hr) || !dxgi_->device) {
        matchedOutput->Release();
        releaseDxgi();
        return false;
    }

    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = matchedOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&dxgiOutput1));
    matchedOutput->Release();
    if (FAILED(hr) || !dxgiOutput1) {
        releaseDxgi();
        return false;
    }

    hr = dxgiOutput1->DuplicateOutput(dxgi_->device, &dxgi_->duplication);
    dxgiOutput1->Release();
    if (FAILED(hr) || !dxgi_->duplication) {
        releaseDxgi();
        return false;
    }

    DXGI_OUTDUPL_DESC duplDesc{};
    dxgi_->duplication->GetDesc(&duplDesc);

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = duplDesc.ModeDesc.Width;
    texDesc.Height = duplDesc.ModeDesc.Height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    DXGI_FORMAT srcFmt = duplDesc.ModeDesc.Format;
    if (srcFmt == DXGI_FORMAT_B8G8R8A8_UNORM || srcFmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
        texDesc.Format = srcFmt;
    } else {
        dxgiRecoveryState_ = DxgiRecoveryState::FallbackGdi;   // let GDI handle HDR desktops
        releaseDxgi();
        return false;
    }
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texDesc.MiscFlags = 0;

    hr = dxgi_->device->CreateTexture2D(&texDesc, nullptr, &dxgi_->stagingTex);
    if (FAILED(hr) || !dxgi_->stagingTex) {
        releaseDxgi();
        return false;
    }

    dxgi_->texW = static_cast<int>(texDesc.Width);
    dxgi_->texH = static_cast<int>(texDesc.Height);
    dxgiInitialized_ = true;
    return true;
}
#else
bool ScreenCapturer::initDxgiForMonitor(int) { return false; }
#endif

#if defined(_WIN32)
bool ScreenCapturer::captureViaDxgi(bool& outFrameUpdated, std::vector<RECT>& outDirtyRects, bool& outHasExplicitDirtyRects) {
    outFrameUpdated = false;
    outHasExplicitDirtyRects = false;
    outDirtyRects.clear();
    if (!dxgiInitialized_ || !dxgi_->duplication) return false;

    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    UINT timeoutMs = hasValidFrame_ ? 10 : 120;
    HRESULT hr = dxgi_->duplication->AcquireNextFrame(timeoutMs, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return hasValidFrame_;
    }
    if (FAILED(hr)) {
        dxgiRecoveryState_ = DxgiRecoveryState::FallbackGdi;
        lastDxgiAttemptTick_ = GetTickCount64();
        releaseDxgi();
        return false;
    }

    // Optimization: When AccumulatedFrames == 0, only mouse pointer moved or changed.
    // The desktop bitmap itself was not updated by the OS / DWM.
    if (frameInfo.AccumulatedFrames == 0) {
        if (desktopResource) {
            desktopResource->Release();
        }
        dxgi_->duplication->ReleaseFrame();
        outFrameUpdated = false;
        outHasExplicitDirtyRects = true;
        return hasValidFrame_;
    }

    ID3D11Texture2D* acquiredTex = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&acquiredTex));
    desktopResource->Release();

    if (SUCCEEDED(hr) && acquiredTex) {
        dxgi_->context->CopyResource(dxgi_->stagingTex, acquiredTex);
        acquiredTex->Release();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = dxgi_->context->Map(dxgi_->stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            if (dxgi_->texW != frameW_ || dxgi_->texH != frameH_) {
                frameW_ = dxgi_->texW;
                frameH_ = dxgi_->texH;
                activeMonitor_.width = frameW_;
                activeMonitor_.height = frameH_;
                currentFrame_.resize(static_cast<size_t>(frameW_) * frameH_ * 4);
                prevTileHashes_.clear();
            }
            size_t rowBytes = static_cast<size_t>(frameW_) * 4;
            for (int r = 0; r < frameH_; ++r) {
                const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData) + r * mapped.RowPitch;
                uint8_t* dstRow = currentFrame_.data() + static_cast<size_t>(r) * rowBytes;
                std::memcpy(dstRow, srcRow, rowBytes);
            }
            dxgi_->context->Unmap(dxgi_->stagingTex, 0);
            hasValidFrame_ = true;
            outFrameUpdated = true;

            // Query hardware dirty rects and move rects if metadata is available
            if (frameInfo.TotalMetadataBufferSize > 0) {
                if (dxgi_->metadataBuf.size() < frameInfo.TotalMetadataBufferSize) {
                    dxgi_->metadataBuf.resize(frameInfo.TotalMetadataBufferSize);
                }

                UINT moveBufSize = frameInfo.TotalMetadataBufferSize;
                hr = dxgi_->duplication->GetFrameMoveRects(
                    moveBufSize,
                    reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(dxgi_->metadataBuf.data()),
                    &moveBufSize
                );
                UINT moveCount = SUCCEEDED(hr) ? (moveBufSize / sizeof(DXGI_OUTDUPL_MOVE_RECT)) : 0;

                UINT dirtyBufSize = frameInfo.TotalMetadataBufferSize - moveBufSize;
                BYTE* dirtyStart = dxgi_->metadataBuf.data() + moveBufSize;
                hr = dxgi_->duplication->GetFrameDirtyRects(
                    dirtyBufSize,
                    reinterpret_cast<RECT*>(dirtyStart),
                    &dirtyBufSize
                );
                UINT dirtyCount = SUCCEEDED(hr) ? (dirtyBufSize / sizeof(RECT)) : 0;

                const auto* pMoves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(dxgi_->metadataBuf.data());
                for (UINT i = 0; i < moveCount; ++i) {
                    outDirtyRects.push_back(pMoves[i].DestinationRect);
                }

                const auto* pDirty = reinterpret_cast<const RECT*>(dirtyStart);
                for (UINT i = 0; i < dirtyCount; ++i) {
                    outDirtyRects.push_back(pDirty[i]);
                }
                outHasExplicitDirtyRects = true;
            }
        }
    }

    dxgi_->duplication->ReleaseFrame();
    return hasValidFrame_;
}
#else
bool ScreenCapturer::captureViaDxgi(bool&, std::vector<RECT>&, bool&) { return false; }
#endif

#if defined(_WIN32)
void ScreenCapturer::releaseGdiResources() {
    if (gdiMemDC_) {
        DeleteDC(gdiMemDC_);
        gdiMemDC_ = nullptr;
    }
    if (gdiSection_) {
        DeleteObject(gdiSection_);
        gdiSection_ = nullptr;
    }
    gdiBits_ = nullptr;
    gdiWidth_ = 0;
    gdiHeight_ = 0;
}
#else
void ScreenCapturer::releaseGdiResources() {}
#endif

#if defined(_WIN32)
bool ScreenCapturer::captureViaGdi() {
    int w = std::max(1, activeMonitor_.width);
    int h = std::max(1, activeMonitor_.height);

    if (frameW_ != w || frameH_ != h || currentFrame_.size() != static_cast<size_t>(w) * h * 4) {
        frameW_ = w;
        frameH_ = h;
        currentFrame_.resize(static_cast<size_t>(w) * h * 4);
        prevTileHashes_.clear();
    }

    HDC hScreenDC = GetDC(nullptr);
    bool createdDC = false;
    if (!hScreenDC) {
        hScreenDC = CreateDCA("DISPLAY", nullptr, nullptr, nullptr);
        if (!hScreenDC) return false;
        createdDC = true;
    }

    auto releaseScreenDC = [&]() {
        if (createdDC) {
            DeleteDC(hScreenDC);
        } else {
            ReleaseDC(nullptr, hScreenDC);
        }
    };

    // Reallocate cached persistent DC and DIB section only if dimensions change or handles missing
    if (!gdiMemDC_ || !gdiSection_ || gdiWidth_ != w || gdiHeight_ != h) {
        releaseGdiResources();
        gdiMemDC_ = CreateCompatibleDC(hScreenDC);
        if (!gdiMemDC_) {
            releaseScreenDC();
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // Top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        gdiSection_ = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &gdiBits_, nullptr, 0);
        if (!gdiSection_ || !gdiBits_) {
            releaseGdiResources();
            releaseScreenDC();
            return false;
        }
        SelectObject(gdiMemDC_, gdiSection_);
        gdiWidth_ = w;
        gdiHeight_ = h;
    }

    BOOL bltOk = BitBlt(gdiMemDC_, 0, 0, w, h, hScreenDC, activeMonitor_.x, activeMonitor_.y, SRCCOPY);
    GdiFlush();
    releaseScreenDC();

    if (bltOk) {
        std::memcpy(currentFrame_.data(), gdiBits_, static_cast<size_t>(w) * h * 4);
        hasValidFrame_ = true;
    } else if (!hasValidFrame_) {
        // Fallback synthetic pattern if running inside a headless/sandboxed desktop session
        for (int y = 0; y < h; ++y) {
            uint8_t* row = currentFrame_.data() + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                row[x * 4 + 0] = static_cast<uint8_t>((x + y) & 0xFF);
                row[x * 4 + 1] = static_cast<uint8_t>((y * 2) & 0xFF);
                row[x * 4 + 2] = static_cast<uint8_t>((x * 2) & 0xFF);
                row[x * 4 + 3] = 0xFF;
            }
        }
        hasValidFrame_ = true;
    }

    return hasValidFrame_;
}
#else
bool ScreenCapturer::captureViaGdi() { return false; }
#endif

#if defined(__APPLE__)
namespace {

// Hosted CI machines can never be given the Screen Recording permission, yet the tile pipeline's tests need frames.
// With CPPDESK_CAPTURE_TEST_PATTERN set, a fixed gradient stands in whenever the real stream is not running. Never
// set outside tests.
bool fillMacTestPattern(std::vector<uint8_t>& frame, int& w, int& h) {
    static const bool enabled = std::getenv("CPPDESK_CAPTURE_TEST_PATTERN") != nullptr;
    if (!enabled) return false;
    w = 1280;
    h = 800;
    frame.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* px = frame.data() + (static_cast<size_t>(y) * w + x) * 4;
            px[0] = static_cast<uint8_t>(x * 255 / w);
            px[1] = static_cast<uint8_t>(y * 255 / h);
            px[2] = static_cast<uint8_t>((x + y) & 0xFF);
            px[3] = 0xFF;
        }
    }
    return true;
}

} // namespace
#endif

bool ScreenCapturer::captureViaCoreGraphics() {
#if defined(__APPLE__)
    CGDirectDisplayID displayId = kCGDirectMainDisplay;
    if (activeMonitorIdx_ >= 0 && activeMonitorIdx_ < static_cast<int>(monitors_.size())) {
        uint32_t maxDisplays = 32;
        CGDirectDisplayID displayIds[32];
        uint32_t numDisplays = 0;
        if (CGGetActiveDisplayList(maxDisplays, displayIds, &numDisplays) == kCGErrorSuccess &&
            static_cast<uint32_t>(activeMonitorIdx_) < numDisplays) {
            displayId = displayIds[activeMonitorIdx_];
        }
    }

    // A ScreenCaptureKit stream of the display pushes frames; (re)start it for a new display, and again, at most
    // every 2 seconds, after macOS stopped it (permission revoked, display change, sleep) or it never started.
    const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!macStream_) macStream_ = std::make_unique<MacCaptureStream>();
    bool justStarted = false;
    if (macStreamDisplay_ != displayId ||
        (macStream_->state() != MacCaptureState::Running && nowMs - macLastStartMs_ >= 2000)) {
        macStreamDisplay_ = displayId;
        macLastStartMs_ = nowMs;
        justStarted = macStream_->start(displayId, 60, 3840);
    }

    int w = 0, h = 0;
    bool isNew = false;
    bool haveFrame = false;
    if (macStream_->state() == MacCaptureState::Running) {
        haveFrame = macStream_->copyLatest(currentFrame_, w, h, isNew);
        // A new stream delivers its first frame a moment after it started: wait for it (up to 2 s) instead of
        // reporting a failed capture.
        for (int i = 0; !haveFrame && justStarted && i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            haveFrame = macStream_->copyLatest(currentFrame_, w, h, isNew);
        }
    }
    if (!haveFrame && !fillMacTestPattern(currentFrame_, w, h)) return false;
    // Frames are in pixels while the monitor's x, y (and the pointer) are in points; the host loop sees the pixel
    // size here and tells the viewer, and the injector reads the display's points from CoreGraphics itself.
    frameW_ = w;
    frameH_ = h;
    activeMonitor_.width = w;
    activeMonitor_.height = h;
    hasValidFrame_ = true;
    return true;
#else
    return false;
#endif
}

bool ScreenCapturer::captureViaX11() {
#if (defined(__linux__) || defined(__unix__)) && __has_include(<X11/Xlib.h>)
    ensureXlibThreads();
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    int capX = activeMonitor_.x;
    int capY = activeMonitor_.y;
    int capW = activeMonitor_.width;
    int capH = activeMonitor_.height;

    if (capW <= 0 || capH <= 0) {
        capW = DisplayWidth(dpy, screen);
        capH = DisplayHeight(dpy, screen);
    }

    frameW_ = capW;
    frameH_ = capH;
    currentFrame_.resize(static_cast<size_t>(frameW_) * frameH_ * 4);

    bool captured = false;

#if __has_include(<X11/extensions/XShm.h>)
    if (XShmQueryExtension(dpy)) {
        XShmSegmentInfo shminfo{};
        XImage* img = XShmCreateImage(dpy, DefaultVisual(dpy, screen), DefaultDepth(dpy, screen),
                                      ZPixmap, nullptr, &shminfo, capW, capH);
        if (img) {
            shminfo.shmid = shmget(IPC_PRIVATE, img->bytes_per_line * img->height, IPC_CREAT | 0777);
            if (shminfo.shmid >= 0) {
                shminfo.shmaddr = img->data = static_cast<char*>(shmat(shminfo.shmid, nullptr, 0));
                shminfo.readOnly = False;
                if (shminfo.shmaddr != reinterpret_cast<char*>(-1) && XShmAttach(dpy, &shminfo)) {
                    if (XShmGetImage(dpy, root, img, capX, capY, AllPlanes)) {
                        for (int r = 0; r < capH; ++r) {
                            const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(img->data + r * img->bytes_per_line);
                            uint8_t* dstRow = currentFrame_.data() + static_cast<size_t>(r) * frameW_ * 4;
                            std::memcpy(dstRow, srcRow, static_cast<size_t>(frameW_) * 4);
                        }
                        captured = true;
                    }
                    XShmDetach(dpy, &shminfo);
                }
                if (shminfo.shmaddr != reinterpret_cast<char*>(-1)) {
                    shmdt(shminfo.shmaddr);
                }
                shmctl(shminfo.shmid, IPC_RMID, nullptr);
            }
            img->data = nullptr;
            XDestroyImage(img);
        }
    }
#endif

    if (!captured) {
        XImage* img = XGetImage(dpy, root, capX, capY, capW, capH, AllPlanes, ZPixmap);
        if (img) {
            for (int r = 0; r < capH; ++r) {
                const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(img->data + r * img->bytes_per_line);
                uint8_t* dstRow = currentFrame_.data() + static_cast<size_t>(r) * frameW_ * 4;
                std::memcpy(dstRow, srcRow, static_cast<size_t>(frameW_) * 4);
            }
            XDestroyImage(img);
            captured = true;
        }
    }

    XCloseDisplay(dpy);
    hasValidFrame_ = captured;
    return captured;
#else
    return false;
#endif
}

CursorState ScreenCapturer::captureCursorState() const {
    CursorState cs;
#if defined(_WIN32)
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) {
        cs.visible = (ci.flags & CURSOR_SHOWING) != 0;
        int relX = ci.ptScreenPos.x - activeMonitor_.x;
        int relY = ci.ptScreenPos.y - activeMonitor_.y;
        if (activeMonitor_.isVirtual && (relX < 0 || relX > frameW_ || relY < 0 || relY > frameH_)) {
            cs.visible = false;
        }
        cs.normX = std::clamp(static_cast<float>(relX) / std::max(1, frameW_), 0.0f, 1.0f);
        cs.normY = std::clamp(static_cast<float>(relY) / std::max(1, frameH_), 0.0f, 1.0f);
    } else {
        POINT pt{};
        GetCursorPos(&pt);
        int relX = pt.x - activeMonitor_.x;
        int relY = pt.y - activeMonitor_.y;
        if (activeMonitor_.isVirtual && (relX < 0 || relX > frameW_ || relY < 0 || relY > frameH_)) {
            cs.visible = false;
        } else {
            cs.visible = true;
        }
        cs.normX = std::clamp(static_cast<float>(relX) / std::max(1, frameW_), 0.0f, 1.0f);
        cs.normY = std::clamp(static_cast<float>(relY) / std::max(1, frameH_), 0.0f, 1.0f);
    }
#elif defined(__APPLE__)
    CGEventRef ev = CGEventCreate(NULL);
    if (ev) {
        CGPoint pt = CGEventGetLocation(ev);
        CFRelease(ev);
        int relX = static_cast<int>(pt.x) - activeMonitor_.x;
        int relY = static_cast<int>(pt.y) - activeMonitor_.y;
        cs.visible = true;
        cs.normX = std::clamp(static_cast<float>(relX) / std::max(1, frameW_), 0.0f, 1.0f);
        cs.normY = std::clamp(static_cast<float>(relY) / std::max(1, frameH_), 0.0f, 1.0f);
    }
#elif defined(__linux__) || defined(__unix__)
#if __has_include(<X11/Xlib.h>)
    ensureXlibThreads();
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy) {
        Window root = DefaultRootWindow(dpy);
        Window rootRet, childRet;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        if (XQueryPointer(dpy, root, &rootRet, &childRet, &rootX, &rootY, &winX, &winY, &mask)) {
            int relX = rootX - activeMonitor_.x;
            int relY = rootY - activeMonitor_.y;
            cs.visible = true;
            cs.normX = std::clamp(static_cast<float>(relX) / std::max(1, frameW_), 0.0f, 1.0f);
            cs.normY = std::clamp(static_cast<float>(relY) / std::max(1, frameH_), 0.0f, 1.0f);
        }
        XCloseDisplay(dpy);
    }
#endif
#endif
    return cs;
}

bool ScreenCapturer::captureDirtyTiles(
    bool forceKeyframe,
    QualityPreset preset,
    std::vector<EncodedTile>& outTiles,
    bool& outIsKeyframe,
    CursorState& outCursor)
{
    outTiles.clear();
    bool dxgiUpdated = false;
    bool captured = false;
    std::vector<RECT> dxgiDirtyRects;
    bool hasExplicitDirtyRects = false;

    // Check if background recovery from FallbackGdi should be attempted (500ms cooldown)
    if (dxgiRecoveryState_ == DxgiRecoveryState::FallbackGdi && activeMonitorIdx_ >= 0 && !activeMonitor_.isVirtual) {
#if defined(_WIN32)
        uint64_t now = GetTickCount64();
#else
        auto steadyNow = std::chrono::steady_clock::now();
        uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(steadyNow.time_since_epoch()).count());
#endif
        if (now - lastDxgiAttemptTick_ >= 500) {
            lastDxgiAttemptTick_ = now;
            if (initDxgiForMonitor(activeMonitorIdx_)) {
                dxgiRecoveryState_ = DxgiRecoveryState::Active;
                forceKeyframe = true;
            }
        }
    }

    auto tCapStart = std::chrono::steady_clock::now();
    if (activeMonitor_.isVirtual) {
        captured = VirtualDisplayManager::instance().captureSoftwareSurface(activeMonitor_.virtualId, currentFrame_, frameW_, frameH_);
        if (captured) {
            hasValidFrame_ = true;
        } else {
            return false;
        }
    } else {
#if defined(_WIN32)
        if (dxgiInitialized_ && dxgiRecoveryState_ == DxgiRecoveryState::Active) {
            captured = captureViaDxgi(dxgiUpdated, dxgiDirtyRects, hasExplicitDirtyRects);
        }
        if (!captured || !hasValidFrame_) {
            if (!captureViaGdi()) {
                return false;
            }
        }
#elif defined(__APPLE__)
        if (!captureViaCoreGraphics()) {
            return false;
        }
#elif defined(__linux__) || defined(__unix__)
        if (!captureViaX11()) {
            return false;
        }
#endif
    }
    auto tCapEnd = std::chrono::steady_clock::now();
    lastCaptureLatencyMs_ = std::chrono::duration<float, std::milli>(tCapEnd - tCapStart).count();

    if (privacyCloak_.isEnabled() && !currentFrame_.empty() && frameW_ > 0 && frameH_ > 0) {
        auto rects = privacyCloak_.findSensitiveWindowRects();
        if (!rects.empty()) {
            privacyCloak_.redactCaptureBuffer(
                currentFrame_.data(), frameW_, frameH_,
                activeMonitor_.x, activeMonitor_.y, rects
            );
            for (const auto& r : rects) {
                RECT winR;
                winR.left = r.left - activeMonitor_.x;
                winR.top = r.top - activeMonitor_.y;
                winR.right = r.right - activeMonitor_.x;
                winR.bottom = r.bottom - activeMonitor_.y;
                dxgiDirtyRects.push_back(winR);
            }
        }
    }

    outCursor = captureCursorState();
    auto tEncStart = std::chrono::steady_clock::now();

    int cols = (frameW_ + TILE_SIZE - 1) / TILE_SIZE;
    int rows = (frameH_ + TILE_SIZE - 1) / TILE_SIZE;
    size_t totalTiles = static_cast<size_t>(cols) * rows;

    if (prevTileHashes_.size() != totalTiles) {
        prevTileHashes_.assign(totalTiles, 0);
        forceKeyframe = true;
    }
    outIsKeyframe = forceKeyframe;

    // If DXGI reported explicit dirty rects and this is not a keyframe:
    // When dxgiDirtyRects is empty, exactly 0 tiles changed!
    if (!forceKeyframe && hasExplicitDirtyRects && dxgiDirtyRects.empty()) {
        auto tEncEnd = std::chrono::steady_clock::now();
        lastEncodeLatencyMs_ = std::chrono::duration<float, std::milli>(tEncEnd - tEncStart).count();
        return true;
    }

    int stride = frameW_ * 4;
    if (reusableDirtyMask_.size() != totalTiles) {
        reusableDirtyMask_.assign(totalTiles, 0);
    } else {
        std::memset(reusableDirtyMask_.data(), 0, totalTiles);
    }
    uint8_t* dirtyMask = reusableDirtyMask_.data();
    size_t dirtyCount = 0;

    if (!forceKeyframe && hasExplicitDirtyRects && !dxgiDirtyRects.empty()) {
        // Fast path: Only compute tile hashes for tiles intersecting hardware dirty rectangles
        if (reusableCandidateTiles_.size() != totalTiles) {
            reusableCandidateTiles_.assign(totalTiles, 0);
        } else {
            std::memset(reusableCandidateTiles_.data(), 0, totalTiles);
        }
        uint8_t* candidateTiles = reusableCandidateTiles_.data();
        for (const auto& r : dxgiDirtyRects) {
            if (r.right <= r.left || r.bottom <= r.top) continue;
            int minTx = std::clamp(static_cast<int>(r.left) / TILE_SIZE, 0, cols - 1);
            int maxTx = std::clamp((static_cast<int>(r.right) - 1) / TILE_SIZE, 0, cols - 1);
            int minTy = std::clamp(static_cast<int>(r.top) / TILE_SIZE, 0, rows - 1);
            int maxTy = std::clamp((static_cast<int>(r.bottom) - 1) / TILE_SIZE, 0, rows - 1);

            for (int ty = minTy; ty <= maxTy; ++ty) {
                for (int tx = minTx; tx <= maxTx; ++tx) {
                    size_t idx = static_cast<size_t>(ty) * cols + tx;
                    candidateTiles[idx] = 1;
                }
            }
        }

        for (int ty = 0; ty < rows; ++ty) {
            int py = ty * TILE_SIZE;
            int th = std::min(TILE_SIZE, frameH_ - py);
            for (int tx = 0; tx < cols; ++tx) {
                size_t idx = static_cast<size_t>(ty) * cols + tx;
                if (!candidateTiles[idx]) continue;

                int px = tx * TILE_SIZE;
                int tw = std::min(TILE_SIZE, frameW_ - px);
                uint64_t h = hashTileBgra(currentFrame_.data(), stride, px, py, tw, th);
                if (prevTileHashes_[idx] != h) {
                    prevTileHashes_[idx] = h;
                    dirtyMask[idx] = 1;
                    ++dirtyCount;
                }
            }
        }
    } else {
        // Full scan path: keyframe or GDI fallback
        for (int ty = 0; ty < rows; ++ty) {
            int py = ty * TILE_SIZE;
            int th = std::min(TILE_SIZE, frameH_ - py);
            for (int tx = 0; tx < cols; ++tx) {
                int px = tx * TILE_SIZE;
                int tw = std::min(TILE_SIZE, frameW_ - px);
                size_t idx = static_cast<size_t>(ty) * cols + tx;

                uint64_t h = hashTileBgra(currentFrame_.data(), stride, px, py, tw, th);
                if (forceKeyframe || prevTileHashes_[idx] != h) {
                    prevTileHashes_[idx] = h;
                    dirtyMask[idx] = 1;
                    ++dirtyCount;
                }
            }
        }
    }

    if (dirtyCount == 0) {
        auto tEncEnd = std::chrono::steady_clock::now();
        lastEncodeLatencyMs_ = std::chrono::duration<float, std::milli>(tEncEnd - tEncStart).count();
        return true;
    }

    // Merge contiguous dirty tiles into rectangles up to 512x256 for optimal compression & low overhead
    constexpr int MAX_MERGE_COLS = 8; // 8 * 64 = 512 px
    constexpr int MAX_MERGE_ROWS = 4; // 4 * 64 = 256 px

    reusableTasks_.clear();
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            size_t idx = static_cast<size_t>(ty) * cols + tx;
            if (!dirtyMask[idx]) continue;

            // Expand horizontally
            int spanCols = 1;
            while (spanCols < MAX_MERGE_COLS && (tx + spanCols) < cols &&
                   dirtyMask[static_cast<size_t>(ty) * cols + (tx + spanCols)]) {
                ++spanCols;
            }

            // Expand vertically if full horizontal run is dirty
            int spanRows = 1;
            while (spanRows < MAX_MERGE_ROWS && (ty + spanRows) < rows) {
                bool rowAllDirty = true;
                for (int c = 0; c < spanCols; ++c) {
                    if (!dirtyMask[static_cast<size_t>(ty + spanRows) * cols + (tx + c)]) {
                        rowAllDirty = false;
                        break;
                    }
                }
                if (!rowAllDirty) break;
                ++spanRows;
            }

            // Clear mask for merged region
            for (int r = 0; r < spanRows; ++r) {
                for (int c = 0; c < spanCols; ++c) {
                    dirtyMask[static_cast<size_t>(ty + r) * cols + (tx + c)] = 0;
                }
            }

            uint16_t rx = static_cast<uint16_t>(tx * TILE_SIZE);
            uint16_t ry = static_cast<uint16_t>(ty * TILE_SIZE);
            uint16_t rw = static_cast<uint16_t>(std::min(frameW_ - static_cast<int>(rx), spanCols * TILE_SIZE));
            uint16_t rh = static_cast<uint16_t>(std::min(frameH_ - static_cast<int>(ry), spanRows * TILE_SIZE));

            TileThreadPool::RectTask task;
            task.rx = rx;
            task.ry = ry;
            task.rw = rw;
            task.rh = rh;
            task.preset = preset;
            task.frameBase = currentFrame_.data();
            task.frameStride = frameW_ * 4;
            reusableTasks_.push_back(std::move(task));
        }
    }

    if (!reusableTasks_.empty()) {
        TileThreadPool::instance().parallelEncode(reusableTasks_);
        outTiles.reserve(reusableTasks_.size());
        for (auto& t : reusableTasks_) {
            outTiles.push_back(std::move(t.result));
        }
    }

    auto tEncEnd = std::chrono::steady_clock::now();
    lastEncodeLatencyMs_ = std::chrono::duration<float, std::milli>(tEncEnd - tEncStart).count();

    return true;
}

} // namespace cppdesk
