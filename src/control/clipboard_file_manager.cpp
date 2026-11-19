#include "clipboard_file_manager.hpp"
#include "../core/crypto_identity.hpp"
#include "../capture/screen_capture.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include "../core/spawn.hpp"
#include "mac_clipboard.hpp"
#else
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include "../core/spawn.hpp"
#include "x11_clipboard.hpp"
#include <openssl/evp.h>
#endif

#include <filesystem>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <chrono>
#include <cmath>
#include <limits>

namespace cppdesk {

namespace {

inline uint64_t currentTimeMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count());
}

constexpr uint64_t MAX_TRANSFER_FILE_BYTES = 2ULL * 1024ULL * 1024ULL * 1024ULL; // 2 GB cap

std::string sanitizeFilename(const std::string& rawName) {
    std::filesystem::path p(rawName);
    std::string base = p.filename().string();
    if (base.empty() || base == "." || base == "..") base = "received_file.bin";
    for (char& c : base) {
        if (c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
            static_cast<unsigned char>(c) < 32) {
            c = '_';
        }
    }
    while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();
    if (base.empty()) base = "received_file.bin";
    std::string stemUpper = base.substr(0, base.find('.'));
    for (char& c : stemUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* kReserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    for (const char* r : kReserved) {
        if (stemUpper == r) { base = "_" + base; break; }
    }
    return base;
}

std::string formatBytesSize(uint64_t bytes) {
    char buf[64];
    if (bytes < 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return std::string(buf);
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
#if defined(_WIN32)
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
#else
    std::string out;
    out.reserve(w.size());
    for (wchar_t wc : w) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp <= 0x7Fu) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0x10FFFFu) {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
#endif
}

} // namespace

std::string FileTransferItem::formatSpeed(double bytesPerSec) {
    if (!std::isfinite(bytesPerSec) || bytesPerSec <= 0.0) {
        return "0 KB/s";
    }
    char buf[64];
    if (bytesPerSec >= 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f MB/s", bytesPerSec / (1024.0 * 1024.0));
    } else if (bytesPerSec >= 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.0f KB/s", bytesPerSec / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f B/s", bytesPerSec);
    }
    return std::string(buf);
}

std::string FileTransferItem::formatEta(double etaSec) {
    if (!std::isfinite(etaSec) || etaSec < 0.0) return "";
    if (etaSec >= 3600.0) return "ETA > 1h";
    int s = static_cast<int>(etaSec + 0.5);
    if (s >= 3600) {
        return "ETA > 1h";
    }
    if (s < 60) {
        return "ETA " + std::to_string(s) + "s";
    }
    int m = s / 60;
    int remS = s % 60;
    return "ETA " + std::to_string(m) + "m " + std::to_string(remS) + "s";
}

#if defined(_WIN32)
struct FileTransferManager::IncrementalSha256 {
    BCRYPT_ALG_HANDLE  hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool               valid = false;

    IncrementalSha256() {
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
            if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) >= 0) {
                valid = true;
            }
        }
    }

    ~IncrementalSha256() {
        if (hHash) BCryptDestroyHash(hHash);
        if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    }

    bool ok() const { return valid; }

    bool update(const void* data, size_t len) {
        if (!valid) return false;
        if (!data || len == 0) return true;
        const auto* p = static_cast<const uint8_t*>(data);
        while (len > 0) {
            const ULONG chunk = static_cast<ULONG>(std::min<size_t>(len, 0x7FFFFFFFu));
            if (BCryptHashData(hHash, const_cast<PUCHAR>(p), chunk, 0) < 0) {
                valid = false;
                return false;
            }
            p += chunk;
            len -= chunk;
        }
        return true;
    }

    std::string finishHex() {
        if (!valid || !hHash) return {};
        std::array<uint8_t, 32> digest{};
        NTSTATUS st = BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        BCryptDestroyHash(hHash);
        hHash = nullptr;
        valid = false;
        if (st < 0) return {};
        return CryptoUtils::toHex(digest.data(), digest.size());
    }
};
#elif defined(__APPLE__)
struct FileTransferManager::IncrementalSha256 {
    CC_SHA256_CTX ctx;
    bool valid = false;

    IncrementalSha256() {
        CC_SHA256_Init(&ctx);
        valid = true;
    }

    ~IncrementalSha256() = default;

    bool ok() const { return valid; }

    bool update(const void* data, size_t len) {
        if (!valid) return false;
        if (!data || len == 0) return true;
        const auto* p = static_cast<const uint8_t*>(data);
        while (len > 0) {
            const CC_LONG chunk = static_cast<CC_LONG>(std::min<size_t>(len, 0x7FFFFFFFu));
            CC_SHA256_Update(&ctx, p, chunk);
            p += chunk;
            len -= chunk;
        }
        return true;
    }

    std::string finishHex() {
        if (!valid) return {};
        std::array<uint8_t, 32> digest{};
        CC_SHA256_Final(digest.data(), &ctx);
        valid = false;
        return CryptoUtils::toHex(digest.data(), digest.size());
    }
};
#else
struct FileTransferManager::IncrementalSha256 {
    EVP_MD_CTX* ctx_ = nullptr;
    bool        valid = false;

    IncrementalSha256() {
        ctx_ = EVP_MD_CTX_new();
        valid = ctx_ && EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr) == 1;
    }

    ~IncrementalSha256() {
        if (ctx_) EVP_MD_CTX_free(ctx_);
    }

    bool ok() const { return valid; }

    bool update(const void* data, size_t len) {
        if (!valid) return false;
        if (!data || len == 0) return true;
        if (EVP_DigestUpdate(ctx_, data, len) != 1) valid = false;
        return valid;
    }

    std::string finishHex() {
        if (!valid) return {};
        std::array<uint8_t, 32> digest{};
        unsigned int outLen = 0;
        const bool good = EVP_DigestFinal_ex(ctx_, digest.data(), &outLen) == 1 && outLen == digest.size();
        valid = false;
        if (!good) return {};
        return CryptoUtils::toHex(digest.data(), digest.size());
    }
};
#endif

// ---------------- ClipboardHistoryManager & Classify ----------------

static std::string classifyClipboardText(const std::string& text) {
    if (text.empty()) return "Text";
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
        std::string sub = text.substr(start);
        if (sub.rfind("http://", 0) == 0 || sub.rfind("https://", 0) == 0 || sub.rfind("www.", 0) == 0) {
            return "URL";
        }
    }
    if ((text.size() >= 3 && text[1] == ':' && (text[2] == '\\' || text[2] == '/')) ||
        (text.size() >= 2 && text[0] == '\\' && text[1] == '\\') ||
        (text.size() >= 1 && text[0] == '/' && text.find(' ') == std::string::npos)) {
        return "Path";
    }
    static const char* codeKeywords[] = {
        "{", "}", "class ", "struct ", "function ", "def ", "public:", "private:",
        "#include", "import ", "const ", "let ", "var ", "=>", "namespace ", "return;"
    };
    for (const char* kw : codeKeywords) {
        if (text.find(kw) != std::string::npos) {
            return "Code";
        }
    }
    return "Text";
}

void ClipboardHistoryManager::addItem(const std::string& text, bool isFromRemote) {
    if (text.empty()) return;
    bool allWs = std::all_of(text.begin(), text.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    });
    if (allWs) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!items_.empty() && items_.front().text == text) {
        return;
    }

    ClipboardHistoryItem item;
    item.id = nextId_++;
    item.text = text;
    item.typeBadge = classifyClipboardText(text);
    item.timestampMs = currentTimeMs();
    item.isFromRemote = isFromRemote;
    item.charCount = text.size();

    std::string preview;
    preview.reserve(std::min<size_t>(text.size(), 120));
    for (char c : text) {
        if (preview.size() >= 120) break;
        if (c == '\r' || c == '\n' || c == '\t') preview += ' ';
        else preview += c;
    }
    item.previewText = std::move(preview);

    items_.insert(items_.begin(), std::move(item));
    if (items_.size() > 50) {
        items_.pop_back();
    }
}

std::vector<ClipboardHistoryItem> ClipboardHistoryManager::items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

std::vector<ClipboardHistoryItem> ClipboardHistoryManager::search(const std::string& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (query.empty()) return items_;

    auto toLowerAscii = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string qLower = query;
    std::transform(qLower.begin(), qLower.end(), qLower.begin(), toLowerAscii);

    std::vector<ClipboardHistoryItem> res;
    for (const auto& it : items_) {
        std::string txtLower = it.text;
        std::transform(txtLower.begin(), txtLower.end(), txtLower.begin(), toLowerAscii);
        std::string typeLower = it.typeBadge;
        std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), toLowerAscii);
        if (txtLower.find(qLower) != std::string::npos || typeLower.find(qLower) != std::string::npos) {
            res.push_back(it);
        }
    }
    return res;
}

bool ClipboardHistoryManager::copyItemToClipboard(uint32_t id) {
    std::string txt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& it : items_) {
            if (it.id == id) {
                txt = it.text;
                break;
            }
        }
    }
    if (txt.empty()) return false;
    return ClipboardManager::setClipboardUtf8(txt);
}

void ClipboardHistoryManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
}

bool ClipboardHistoryManager::deleteItem(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::remove_if(items_.begin(), items_.end(), [id](const ClipboardHistoryItem& item) {
        return item.id == id;
    });
    if (it != items_.end()) {
        items_.erase(it, items_.end());
        return true;
    }
    return false;
}

size_t ClipboardHistoryManager::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

// ---------------- ClipboardManager ----------------

#if defined(_WIN32)

ClipboardManager::ClipboardManager() {
    lastSeqNumber_ = GetClipboardSequenceNumber();
}

std::string ClipboardManager::getClipboardUtf8() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return {};
    }
    bool opened = false;
    for (int i = 0; i < 8; ++i) {
        if (OpenClipboard(nullptr)) {
            opened = true;
            break;
        }
        Sleep(5);
    }
    if (!opened) {
        return {};
    }

    std::string result;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const wchar_t* wText = static_cast<const wchar_t*>(GlobalLock(hData));
        if (wText) {
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wText, -1, nullptr, 0, nullptr, nullptr);
            if (utf8Len > 1) {
                result.resize(static_cast<size_t>(utf8Len - 1));
                WideCharToMultiByte(CP_UTF8, 0, wText, -1, result.data(), utf8Len, nullptr, nullptr);
            }
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return result;
}

bool ClipboardManager::setClipboardUtf8(const std::string& utf8Text) {
    if (utf8Text.empty()) return false;

    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), -1, nullptr, 0);
    if (wLen <= 0) return false;

    bool opened = false;
    for (int i = 0; i < 8; ++i) {
        if (OpenClipboard(nullptr)) {
            opened = true;
            break;
        }
        Sleep(5);
    }
    if (!opened) {
        return false;
    }
    EmptyClipboard();

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wLen) * sizeof(wchar_t));
    if (!hGlobal) {
        CloseClipboard();
        return false;
    }

    bool ok = false;
    wchar_t* wBuf = static_cast<wchar_t*>(GlobalLock(hGlobal));
    if (wBuf) {
        MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), -1, wBuf, wLen);
        GlobalUnlock(hGlobal);
        if (SetClipboardData(CF_UNICODETEXT, hGlobal)) {
            ok = true;
        } else {
            GlobalFree(hGlobal);
        }
    } else {
        GlobalFree(hGlobal);
    }

    CloseClipboard();
    return ok;
}

bool ClipboardManager::getClipboardImageJpeg(std::vector<uint8_t>& outJpeg, uint32_t& outW, uint32_t& outH, int maxDim) {
    if (!IsClipboardFormatAvailable(CF_BITMAP)) {
        return false;
    }
    if (!OpenClipboard(nullptr)) {
        return false;
    }

    HBITMAP hBmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (!hBmp) {
        CloseClipboard();
        return false;
    }

    BITMAP bm{};
    if (!GetObjectW(hBmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        CloseClipboard();
        return false;
    }

    int srcW = bm.bmWidth;
    int srcH = bm.bmHeight;
    int dstW = srcW;
    int dstH = srcH;
    if (maxDim > 0 && (dstW > maxDim || dstH > maxDim)) {
        if (dstW >= dstH) {
            dstH = std::max(1, static_cast<int>(static_cast<double>(dstH) * maxDim / dstW));
            dstW = maxDim;
        } else {
            dstW = std::max(1, static_cast<int>(static_cast<double>(dstW) * maxDim / dstH));
            dstH = maxDim;
        }
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcSrc = CreateCompatibleDC(hdcScreen);
    HDC hdcDst = CreateCompatibleDC(hdcScreen);
    HGDIOBJ hOldSrc = SelectObject(hdcSrc, hBmp);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dstW;
    bi.bmiHeader.biHeight = -dstH; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBmpDst = CreateDIBSection(hdcDst, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    HGDIOBJ hOldDst = nullptr;
    if (hBmpDst && pBits) {
        hOldDst = SelectObject(hdcDst, hBmpDst);
        SetStretchBltMode(hdcDst, HALFTONE);
        SetBrushOrgEx(hdcDst, 0, 0, nullptr);
        StretchBlt(hdcDst, 0, 0, dstW, dstH, hdcSrc, 0, 0, srcW, srcH, SRCCOPY);
    }

    std::vector<uint8_t> encoded;
    if (pBits) {
        encoded = TileCodec::encodeJpeg(static_cast<const uint8_t*>(pBits), dstW, dstH, 80);
    }

    if (hOldDst) SelectObject(hdcDst, hOldDst);
    if (hBmpDst) DeleteObject(hBmpDst);
    if (hOldSrc) SelectObject(hdcSrc, hOldSrc);
    DeleteDC(hdcDst);
    DeleteDC(hdcSrc);
    ReleaseDC(nullptr, hdcScreen);
    CloseClipboard();

    if (encoded.empty()) {
        return false;
    }
    outJpeg = std::move(encoded);
    outW = static_cast<uint32_t>(dstW);
    outH = static_cast<uint32_t>(dstH);
    return true;
}

bool ClipboardManager::pollLocalChange(std::string& outNewText) {
    uint32_t seq = GetClipboardSequenceNumber();
    if (seq == lastSeqNumber_) {
        return false;
    }
    lastSeqNumber_ = seq;

    std::string txt = getClipboardUtf8();
    if (txt.empty() || txt.size() > 256 * 1024) {
        return false;
    }

    std::string h = CryptoUtils::sha256Hex(txt);
    if (h == lastAppliedHash_) {
        return false;
    }
    lastAppliedHash_ = h;
    history_.addItem(txt, false);
    outNewText = std::move(txt);
    return true;
}

void ClipboardManager::applyRemoteClipboard(const std::string& utf8Text) {
    if (utf8Text.empty()) return;
    lastAppliedHash_ = CryptoUtils::sha256Hex(utf8Text);
    setClipboardUtf8(utf8Text);
    history_.addItem(utf8Text, true);
    lastSeqNumber_ = GetClipboardSequenceNumber();
}

#else

ClipboardManager::ClipboardManager() {
    lastSeqNumber_ = 1;
}

static std::string g_posixClipboardText;
static std::mutex g_posixClipboardMutex;

std::string ClipboardManager::getClipboardUtf8() {
#if defined(__linux__)
    if (X11Clipboard::instance().available()) return X11Clipboard::instance().getText().value_or("");
#elif defined(__APPLE__)
    if (std::string text; macClipboardGetText(text)) return text;
#endif
    std::lock_guard<std::mutex> lock(g_posixClipboardMutex);
    return g_posixClipboardText;
}

bool ClipboardManager::setClipboardUtf8(const std::string& utf8Text) {
#if defined(__linux__)
    if (X11Clipboard::instance().available()) return X11Clipboard::instance().setText(utf8Text);
#elif defined(__APPLE__)
    if (macClipboardSetText(utf8Text)) return true;
#endif
    std::lock_guard<std::mutex> lock(g_posixClipboardMutex);
    g_posixClipboardText = utf8Text;
    return true;
}

bool ClipboardManager::getClipboardImageJpeg(std::vector<uint8_t>& /*outJpeg*/, uint32_t& /*outW*/, uint32_t& /*outH*/, int /*maxDim*/) {
    return false;
}

bool ClipboardManager::pollLocalChange(std::string& outNewText) {
    std::string txt = getClipboardUtf8();
    if (txt.empty() || txt.size() > 256 * 1024) {
        return false;
    }
    std::string h = CryptoUtils::sha256Hex(txt);
    if (h == lastAppliedHash_) {
        return false;
    }
    lastAppliedHash_ = h;
    history_.addItem(txt, false);
    outNewText = std::move(txt);
    return true;
}

void ClipboardManager::applyRemoteClipboard(const std::string& utf8Text) {
    if (utf8Text.empty()) return;
    lastAppliedHash_ = CryptoUtils::sha256Hex(utf8Text);
    setClipboardUtf8(utf8Text);
    history_.addItem(utf8Text, true);
    lastSeqNumber_++;
}

#endif

// ---------------- FileTransferManager ----------------

static constexpr const char* kDefaultReceivedFolder = "CppDesk_Received";

FileTransferManager::FileTransferManager() {
    std::filesystem::path dl;
#if defined(_WIN32)
    char profileDir[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, profileDir))) {
        dl = std::filesystem::path(profileDir) / "Downloads" / kDefaultReceivedFolder;
    }
#else
    const char* home = std::getenv("HOME");
    if (home) {
        dl = std::filesystem::path(home) / "Downloads" / kDefaultReceivedFolder;
    }
#endif
    if (dl.empty()) {
        dl = kDefaultReceivedFolder;
    }
    std::error_code ec;
    std::filesystem::create_directories(dl, ec);
    receiveDir_ = dl.string();
}

FileTransferManager::~FileTransferManager() {
    abortActiveTransfers();
}

void FileTransferManager::setReceiveDirectory(const std::string& dirPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    receiveDir_ = dirPath;
    std::error_code ec;
    std::filesystem::create_directories(receiveDir_, ec);
}

std::string FileTransferManager::receiveDirectory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return receiveDir_;
}

void FileTransferManager::pushItemLocked(FileTransferItem item) {
    items_.insert(items_.begin(), std::move(item));
    if (items_.size() > 150) {
        items_.pop_back();
    }
}

void FileTransferManager::openReceiveDirectoryInExplorer() const {
    std::string dir = receiveDirectory();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    cppdesk::spawn::detached({"open", dir});
#else
    cppdesk::spawn::detached({"xdg-open", dir});
#endif
}

uint32_t FileTransferManager::startOutgoingFile(const std::string& filePath, const SendPacketFn& sendPacket,
                                               FileOfferTarget targetHint, float dropNx, float dropNy) {
    return startOutgoingFile(std::filesystem::path(filePath), sendPacket, targetHint, dropNx, dropNy);
}

uint32_t FileTransferManager::startOutgoingFile(const std::filesystem::path& p, const SendPacketFn& sendPacket,
                                               FileOfferTarget targetHint, float dropNx, float dropNy) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
        return 0;
    }

    uint64_t fsize = std::filesystem::file_size(p, ec);
    if (fsize > MAX_TRANSFER_FILE_BYTES) {
        return 0;
    }

    // string() can throw for names the ANSI code page cannot represent; fall back to UTF-8 rather than unwinding the session thread.
    auto narrow = [](const std::filesystem::path& x) {
        try {
            return x.string();
        } catch (...) {
            auto u = x.u8string();
            return std::string(reinterpret_cast<const char*>(u.data()), u.size());
        }
    };
    const std::string filePath = narrow(p);

    auto out = std::make_unique<ActiveOutgoing>();
    out->filePath = filePath;
    out->fileName = sanitizeFilename(narrow(p.filename()));
    out->totalBytes = fsize;
    out->offset = 0;
    out->startTimeMs = currentTimeMs();
    out->lastSampleTimeMs = out->startTimeMs;
    out->lastSampleBytes = 0;
    out->emaSpeedBps = 0.0;
    out->hasher = std::make_unique<IncrementalSha256>();
    if (!out->hasher->ok()) {
        return 0;
    }
    out->stream.open(p, std::ios::binary);
    if (!out->stream.is_open()) {
        return 0;
    }

    uint32_t tid = 0;
    std::string fname;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tid = nextTransferId_++;
        out->transferId = tid;
        fname = out->fileName;

        FileTransferItem item;
        item.transferId = tid;
        item.fileName = fname;
        item.savedPath = filePath;
        item.totalBytes = fsize;
        item.transferredBytes = 0;
        item.isOutgoing = true;
        item.status = TransferStatus::InProgress;
        item.statusText = "Sending (0%)";
        item.startTimeMs = out->startTimeMs;
        item.speedBps = 0.0;
        item.etaSeconds = -1.0;
        pushItemLocked(std::move(item));

        outgoingQueue_.push_back(std::move(out));
    }

    if (sendPacket) {
        ByteWriter w;
        w.writeU32(tid);
        w.writeU64(fsize);
        w.writeString(fname);
        w.writeU8(static_cast<uint8_t>(targetHint));
        w.writeF32(dropNx);
        w.writeF32(dropNy);
        if (!sendPacket(PacketType::FILE_OFFER, w.buffer())) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = outgoingQueue_.begin(); it != outgoingQueue_.end(); ++it) {
                if ((*it)->transferId == tid) {
                    (*it)->stream.close();
                    outgoingQueue_.erase(it);
                    break;
                }
            }
            for (auto& item : items_) {
                if (item.transferId == tid && item.isOutgoing) {
                    item.status = TransferStatus::Failed;
                    item.statusText = "Failed (Send Error)";
                    break;
                }
            }
            return 0;
        }
    }
    return tid;
}

int FileTransferManager::startOutgoingPath(const std::string& path, const SendPacketFn& sendPacket,
                                          FileOfferTarget targetHint, float dropNx, float dropNy) {
    std::filesystem::path p(path);
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        return 0;
    }

    if (std::filesystem::is_directory(p, ec)) {
        int queued = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(p, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file(ec)) {
                if (startOutgoingFile(entry.path().string(), sendPacket, targetHint, dropNx, dropNy) > 0) {
                    ++queued;
                }
            }
        }
        return queued;
    }

    if (std::filesystem::is_regular_file(p, ec)) {
        return (startOutgoingFile(path, sendPacket, targetHint, dropNx, dropNy) > 0) ? 1 : 0;
    }

    return 0;
}

bool FileTransferManager::pumpOutgoingChunks(const SendPacketFn& sendPacket, int maxChunks) {
    if (!sendPacket) return false;
    bool didWork = false;

    for (int i = 0; i < maxChunks; ++i) {
        uint32_t tid = 0;
        uint64_t offset = 0;
        std::vector<uint8_t> chunkBuf;
        bool finished = false;
        bool failed = false;
        std::string failReason;
        std::string finalShaHex;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (outgoingQueue_.empty()) break;

            auto& active = outgoingQueue_.front();
            tid = active->transferId;
            offset = active->offset;

            if (active->offset < active->totalBytes) {
                if (!active->stream.good()) {
                    failed = true;
                    failReason = "Failed (Read Error)";
                } else {
                    size_t toRead = static_cast<size_t>(
                        std::min<uint64_t>(FILE_CHUNK_SIZE, active->totalBytes - active->offset)
                    );
                    chunkBuf.resize(toRead);
                    active->stream.read(reinterpret_cast<char*>(chunkBuf.data()), static_cast<std::streamsize>(toRead));
                    std::streamsize got = active->stream.gcount();
                    if (got > 0) {
                        chunkBuf.resize(static_cast<size_t>(got));
                        if (active->hasher && !active->hasher->update(chunkBuf.data(), chunkBuf.size())) {
                            failed = true;
                            failReason = "Failed (Hash Error)";
                            chunkBuf.clear();
                        } else {
                            active->offset += static_cast<uint64_t>(got);
                        }
                    } else {
                        // Premature EOF or I/O failure before totalBytes
                        failed = true;
                        failReason = "Failed (Read Error)";
                        chunkBuf.clear();
                    }
                }
            }

            if (!failed && active->offset >= active->totalBytes) {
                finished = true;
            }

            if (finished) {
                if (active->hasher) {
                    finalShaHex = active->hasher->finishHex();
                }
                if (finalShaHex.empty()) {
                    finished = false;
                    failed = true;
                    failReason = "Failed (Hash Error)";
                }
            }

            uint64_t nowMs = currentTimeMs();
            uint64_t elapsedSampleMs = nowMs - active->lastSampleTimeMs;
            if (elapsedSampleMs >= 200) {
                double dt = static_cast<double>(elapsedSampleMs) / 1000.0;
                double instantSpeed = static_cast<double>(active->offset - active->lastSampleBytes) / dt;
                active->emaSpeedBps = (active->emaSpeedBps <= 0.0)
                    ? instantSpeed
                    : (0.25 * instantSpeed + 0.75 * active->emaSpeedBps);
                active->lastSampleTimeMs = nowMs;
                active->lastSampleBytes = active->offset;
            }

            for (auto& item : items_) {
                if (item.transferId == tid && item.isOutgoing) {
                    item.transferredBytes = active->offset;
                    item.speedBps = active->emaSpeedBps;
                    if (active->emaSpeedBps > 1024.0 && item.totalBytes > item.transferredBytes) {
                        item.etaSeconds = static_cast<double>(item.totalBytes - item.transferredBytes) / active->emaSpeedBps;
                    } else {
                        item.etaSeconds = -1.0;
                    }
                    if (failed) {
                        item.status = TransferStatus::Failed;
                        item.statusText = failReason;
                        item.speedBps = 0.0;
                        item.etaSeconds = -1.0;
                    } else if (finished) {
                        item.status = TransferStatus::Completed;
                        item.sha256Hex = finalShaHex;
                        item.speedBps = 0.0;
                        item.etaSeconds = -1.0;
                        uint64_t totalElapsedMs = nowMs - item.startTimeMs;
                        if (totalElapsedMs > 0) {
                            item.avgSpeedBps = (static_cast<double>(item.totalBytes) * 1000.0) / static_cast<double>(totalElapsedMs);
                        }
                        item.statusText = "Sent (" + formatBytesSize(item.totalBytes) + ") • " + FileTransferItem::formatSpeed(item.avgSpeedBps);
                    } else {
                        int pct = static_cast<int>(item.progressFraction() * 100.0f);
                        std::string st = "Sending (" + std::to_string(pct) + "%) • " + FileTransferItem::formatSpeed(item.speedBps);
                        if (item.etaSeconds >= 0.0) {
                            st += " • " + FileTransferItem::formatEta(item.etaSeconds);
                        }
                        item.statusText = st;
                    }
                    break;
                }
            }

            if (finished || failed) {
                active->stream.close();
                outgoingQueue_.erase(outgoingQueue_.begin());
            }
        }

        if (failed) {
            ByteWriter w;
            w.writeU32(tid);
            sendPacket(PacketType::FILE_CANCEL, w.buffer());
            didWork = true;
            continue;
        }

        auto failSend = [&](uint32_t failedTid) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = outgoingQueue_.begin(); it != outgoingQueue_.end(); ++it) {
                if ((*it)->transferId == failedTid) {
                    (*it)->stream.close();
                    outgoingQueue_.erase(it);
                    break;
                }
            }
            for (auto& item : items_) {
                if (item.transferId == failedTid && item.isOutgoing) {
                    item.status = TransferStatus::Failed;
                    item.statusText = "Failed (Send Error)";
                    item.speedBps = 0.0;
                    item.etaSeconds = -1.0;
                    break;
                }
            }
        };

        if (!chunkBuf.empty()) {
            ByteWriter w;
            w.writeU32(tid);
            w.writeU64(offset);
            w.writeU32(static_cast<uint32_t>(chunkBuf.size()));
            w.writeBytes(chunkBuf.data(), chunkBuf.size());
            if (!sendPacket(PacketType::FILE_CHUNK, w.buffer())) {
                failSend(tid);
                break;
            }
            didWork = true;
        }

        if (finished) {
            ByteWriter w;
            w.writeU32(tid);
            w.writeString(finalShaHex);
            if (!sendPacket(PacketType::FILE_COMPLETE, w.buffer())) {
                failSend(tid);
                break;
            }
            didWork = true;
        }
    }
    return didWork;
}

bool FileTransferManager::cancelTransfer(uint32_t transferId, const SendPacketFn& sendPacket) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = outgoingQueue_.begin(); it != outgoingQueue_.end(); ++it) {
            if ((*it)->transferId == transferId) {
                (*it)->stream.close();
                outgoingQueue_.erase(it);
                found = true;
                break;
            }
        }
        for (auto it = incomingStreams_.begin(); it != incomingStreams_.end(); ++it) {
            if ((*it)->transferId == transferId) {
                (*it)->stream.close();
                std::error_code ec;
                std::filesystem::remove((*it)->partPath, ec);
                incomingStreams_.erase(it);
                found = true;
                break;
            }
        }
        for (auto& item : items_) {
            if (item.transferId == transferId && item.status == TransferStatus::InProgress) {
                item.status = TransferStatus::Cancelled;
                item.statusText = "Cancelled";
                item.speedBps = 0.0;
                item.etaSeconds = -1.0;
                found = true;
                break;
            }
        }
    }
    if (found && sendPacket) {
        ByteWriter w;
        w.writeU32(transferId);
        sendPacket(PacketType::FILE_CANCEL, w.buffer());
    }
    return found;
}

void FileTransferManager::abortActiveTransfers() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& out : outgoingQueue_) {
        out->stream.close();
    }
    outgoingQueue_.clear();

    for (auto& inc : incomingStreams_) {
        inc->stream.close();
        std::error_code ec;
        std::filesystem::remove(inc->partPath, ec);
    }
    incomingStreams_.clear();

    for (auto& item : items_) {
        if (item.status == TransferStatus::InProgress) {
            item.status = TransferStatus::Cancelled;
            item.statusText = "Interrupted";
            item.speedBps = 0.0;
            item.etaSeconds = -1.0;
        }
    }
}

void FileTransferManager::handleFileOffer(uint32_t transferId, uint64_t totalBytes, const std::string& fileName,
                                         FileOfferTarget targetHint, float dropNx, float dropNy) {
    (void)dropNx;
    (void)dropNy;
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;

    std::string baseDir = receiveDir_;
    if (targetHint == FileOfferTarget::Desktop) {
#if defined(_WIN32)
        PWSTR pDesk = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &pDesk)) && pDesk) {
            std::filesystem::path deskPath(pDesk);
            CoTaskMemFree(pDesk);
            baseDir = deskPath.string();
        }
#else
        const char* home = std::getenv("HOME");
        if (home) {
            std::filesystem::path deskPath = std::filesystem::path(home) / "Desktop";
            if (std::filesystem::exists(deskPath, ec)) {
                baseDir = deskPath.string();
            }
        }
#endif
    }
    std::filesystem::create_directories(baseDir, ec);

    std::string cleanName = sanitizeFilename(fileName);
    std::filesystem::path targetPath = std::filesystem::path(baseDir) / cleanName;

    // Reject duplicate active transferId
    for (const auto& existing : incomingStreams_) {
        if (existing->transferId == transferId) {
            return;
        }
    }

    // Verify file size limit and available disk space
    constexpr uint64_t kDiskReserveBytes = 16ULL * 1024 * 1024;
    bool spaceOk = (totalBytes <= MAX_TRANSFER_FILE_BYTES);
    if (spaceOk) {
        auto spaceInfo = std::filesystem::space(baseDir, ec);
        if (!ec && (spaceInfo.available == 0 ||
                    totalBytes > spaceInfo.available ||
                    spaceInfo.available - totalBytes < kDiskReserveBytes)) {
            spaceOk = false;
        }
    }

    if (!spaceOk) {
        FileTransferItem item;
        item.transferId = transferId;
        item.fileName = cleanName;
        item.totalBytes = totalBytes;
        item.isOutgoing = false;
        item.status = TransferStatus::Failed;
        item.statusText = "Rejected (Size/Disk Limit)";
        pushItemLocked(std::move(item));
        return;
    }

    if (incomingStreams_.size() >= 10) {
        FileTransferItem item;
        item.transferId = transferId;
        item.fileName = cleanName;
        item.totalBytes = totalBytes;
        item.isOutgoing = false;
        item.status = TransferStatus::Failed;
        item.statusText = "Rejected (Queue Full)";
        pushItemLocked(std::move(item));
        return;
    }

    // Reserve a unique filename against both existing files/.part files and active incoming streams
    auto isPathTaken = [&](const std::filesystem::path& cand) -> bool {
        std::string candStr = cand.string();
        std::string candPart = candStr + ".part";
        std::error_code checkEc;
        if (std::filesystem::exists(cand, checkEc) || std::filesystem::exists(candPart, checkEc)) {
            return true;
        }
        for (const auto& active : incomingStreams_) {
            if (active->savePath == candStr || active->partPath == candPart) {
                return true;
            }
        }
        return false;
    };

    bool foundUnique = !isPathTaken(targetPath);
    if (!foundUnique) {
        std::string stem = targetPath.stem().string();
        std::string ext = targetPath.extension().string();
        for (int n = 1; n < 1000; ++n) {
            auto candidate = std::filesystem::path(baseDir) / (stem + " (" + std::to_string(n) + ")" + ext);
            if (!isPathTaken(candidate)) {
                targetPath = candidate;
                foundUnique = true;
                break;
            }
        }
    }
    if (!foundUnique) {
        FileTransferItem item;
        item.transferId = transferId;
        item.fileName = cleanName;
        item.totalBytes = totalBytes;
        item.isOutgoing = false;
        item.status = TransferStatus::Failed;
        item.statusText = "Rejected (Filename Collision)";
        pushItemLocked(std::move(item));
        return;
    }

    std::string partPathStr = targetPath.string() + ".part";
    auto inc = std::make_unique<ActiveIncoming>();
    inc->transferId = transferId;
    inc->fileName = targetPath.filename().string();
    inc->partPath = partPathStr;
    inc->savePath = targetPath.string();
    inc->totalBytes = totalBytes;
    inc->receivedBytes = 0;
    inc->startTimeMs = currentTimeMs();
    inc->lastSampleTimeMs = inc->startTimeMs;
    inc->lastSampleBytes = 0;
    inc->emaSpeedBps = 0.0;
    inc->hasher = std::make_unique<IncrementalSha256>();
    bool hasherOk = inc->hasher && inc->hasher->ok();
    if (hasherOk) {
        inc->stream.open(partPathStr, std::ios::binary | std::ios::trunc);
    }

    FileTransferItem item;
    item.transferId = transferId;
    item.fileName = inc->fileName;
    item.savedPath = inc->savePath;
    item.totalBytes = totalBytes;
    item.transferredBytes = 0;
    item.isOutgoing = false;
    item.startTimeMs = inc->startTimeMs;
    item.speedBps = 0.0;
    item.etaSeconds = -1.0;
    if (hasherOk && inc->stream.is_open()) {
        item.status = TransferStatus::InProgress;
        item.statusText = (totalBytes == 0) ? "Receiving (0 B)" : "Receiving (0%)";
        incomingStreams_.push_back(std::move(inc));
    } else {
        item.status = TransferStatus::Failed;
        item.statusText = hasherOk ? "Write Failed" : "Hash Init Failed";
    }
    pushItemLocked(std::move(item));
}

void FileTransferManager::handleFileChunk(uint32_t transferId, uint64_t offset, const uint8_t* chunkData, size_t chunkLen) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = incomingStreams_.begin(); it != incomingStreams_.end(); ++it) {
        auto& inc = *it;
        if (inc->transferId == transferId) {
            if (chunkLen == 0 || inc->receivedBytes + chunkLen > inc->totalBytes || offset != inc->receivedBytes) {
                if (inc->stream.is_open()) inc->stream.close();
                std::error_code ec;
                std::filesystem::remove(inc->partPath, ec);
                for (auto& item : items_) {
                    if (item.transferId == transferId && !item.isOutgoing) {
                        item.status = TransferStatus::Failed;
                        item.statusText = "Transfer aborted: size limit exceeded or offset mismatch";
                        break;
                    }
                }
                incomingStreams_.erase(it);
                return;
            }

            if (inc->stream.is_open()) {
                inc->stream.write(reinterpret_cast<const char*>(chunkData), static_cast<std::streamsize>(chunkLen));
                if (!inc->stream.good()) {
                    inc->stream.close();
                    std::error_code ec;
                    std::filesystem::remove(inc->partPath, ec);
                    for (auto& item : items_) {
                        if (item.transferId == transferId && !item.isOutgoing) {
                            item.status = TransferStatus::Failed;
                            item.statusText = "Transfer aborted: disk write error";
                            break;
                        }
                    }
                    incomingStreams_.erase(it);
                    return;
                }
                if (inc->hasher && !inc->hasher->update(chunkData, chunkLen)) {
                    inc->stream.close();
                    std::error_code ec;
                    std::filesystem::remove(inc->partPath, ec);
                    for (auto& item : items_) {
                        if (item.transferId == transferId && !item.isOutgoing) {
                            item.status = TransferStatus::Failed;
                            item.statusText = "Transfer aborted: hash error";
                            break;
                        }
                    }
                    incomingStreams_.erase(it);
                    return;
                }
                inc->receivedBytes += chunkLen;
            }

            uint64_t nowMs = currentTimeMs();
            uint64_t elapsedSampleMs = nowMs - inc->lastSampleTimeMs;
            if (elapsedSampleMs >= 200) {
                double dt = static_cast<double>(elapsedSampleMs) / 1000.0;
                double instantSpeed = static_cast<double>(inc->receivedBytes - inc->lastSampleBytes) / dt;
                inc->emaSpeedBps = (inc->emaSpeedBps <= 0.0)
                    ? instantSpeed
                    : (0.25 * instantSpeed + 0.75 * inc->emaSpeedBps);
                inc->lastSampleTimeMs = nowMs;
                inc->lastSampleBytes = inc->receivedBytes;
            }

            for (auto& item : items_) {
                if (item.transferId == transferId && !item.isOutgoing) {
                    item.transferredBytes = inc->receivedBytes;
                    item.speedBps = inc->emaSpeedBps;
                    if (inc->emaSpeedBps > 1024.0 && item.totalBytes > item.transferredBytes) {
                        item.etaSeconds = static_cast<double>(item.totalBytes - item.transferredBytes) / inc->emaSpeedBps;
                    } else {
                        item.etaSeconds = -1.0;
                    }
                    int pct = static_cast<int>(item.progressFraction() * 100.0f);
                    std::string st = "Receiving (" + std::to_string(pct) + "%) • " + FileTransferItem::formatSpeed(item.speedBps);
                    if (item.etaSeconds >= 0.0) {
                        st += " • " + FileTransferItem::formatEta(item.etaSeconds);
                    }
                    item.statusText = st;
                    break;
                }
            }
            break;
        }
    }
}

void FileTransferManager::handleFileComplete(uint32_t transferId, const std::string& sha256Hex) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string computedSha;
    std::string partPath;
    std::string finalPath;
    uint64_t receivedBytes = 0;
    uint64_t totalBytes = 0;
    bool foundStream = false;

    for (auto it = incomingStreams_.begin(); it != incomingStreams_.end(); ++it) {
        if ((*it)->transferId == transferId) {
            (*it)->stream.flush();
            (*it)->stream.close();
            if ((*it)->hasher) {
                computedSha = (*it)->hasher->finishHex();
            }
            partPath = (*it)->partPath;
            finalPath = (*it)->savePath;
            receivedBytes = (*it)->receivedBytes;
            totalBytes = (*it)->totalBytes;
            foundStream = true;
            incomingStreams_.erase(it);
            break;
        }
    }

    bool bytesComplete = foundStream && (receivedBytes == totalBytes);
    bool hashValid = bytesComplete && !sha256Hex.empty() && !computedSha.empty() && (computedSha == sha256Hex);
    bool renameOk = false;
    std::error_code ec;
    if (foundStream) {
        if (hashValid) {
            std::filesystem::rename(partPath, finalPath, ec);
            if (!ec) {
                renameOk = true;
            } else {
                std::error_code rmEc;
                std::filesystem::remove(partPath, rmEc);
            }
        } else {
            std::filesystem::remove(partPath, ec);
        }
    }

    uint64_t nowMs = currentTimeMs();
    for (auto& item : items_) {
        if (item.transferId == transferId && !item.isOutgoing) {
            item.speedBps = 0.0;
            item.etaSeconds = -1.0;
            if (hashValid && renameOk) {
                item.transferredBytes = receivedBytes;
                item.status = TransferStatus::Completed;
                item.sha256Hex = computedSha;
                uint64_t totalElapsedMs = nowMs - item.startTimeMs;
                if (totalElapsedMs > 0) {
                    item.avgSpeedBps = (static_cast<double>(item.totalBytes) * 1000.0) / static_cast<double>(totalElapsedMs);
                }
                item.statusText = "Verified (" + formatBytesSize(item.totalBytes) + ") • " + FileTransferItem::formatSpeed(item.avgSpeedBps);
            } else if (!bytesComplete) {
                item.status = TransferStatus::Failed;
                item.statusText = "Failed (Incomplete Transfer)";
            } else if (!hashValid) {
                item.status = TransferStatus::Failed;
                item.statusText = "Failed (SHA-256 Mismatch)";
            } else {
                item.status = TransferStatus::Failed;
                item.statusText = "Failed (Save Error)";
            }
            break;
        }
    }
}

void FileTransferManager::handleFileCancel(uint32_t transferId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = incomingStreams_.begin(); it != incomingStreams_.end(); ++it) {
        if ((*it)->transferId == transferId) {
            (*it)->stream.close();
            std::error_code ec;
            std::filesystem::remove((*it)->partPath, ec);
            incomingStreams_.erase(it);
            break;
        }
    }
    for (auto it = outgoingQueue_.begin(); it != outgoingQueue_.end(); ++it) {
        if ((*it)->transferId == transferId) {
            (*it)->stream.close();
            outgoingQueue_.erase(it);
            break;
        }
    }
    for (auto& item : items_) {
        if (item.transferId == transferId) {
            item.status = TransferStatus::Cancelled;
            item.statusText = "Cancelled";
            item.speedBps = 0.0;
            item.etaSeconds = -1.0;
            break;
        }
    }
}

void FileTransferManager::clearCompleted() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(
        std::remove_if(items_.begin(), items_.end(), [](const FileTransferItem& it) {
            return it.status != TransferStatus::InProgress;
        }),
        items_.end()
    );
}

std::vector<FileTransferItem> FileTransferManager::snapshotTransfers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

bool FileTransferManager::hasActiveTransfers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& it : items_) {
        if (it.status == TransferStatus::InProgress) {
            return true;
        }
    }
    return !outgoingQueue_.empty() || !incomingStreams_.empty();
}

double FileTransferManager::aggregateActiveBandwidthBps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double totalBps = 0.0;
    for (const auto& it : items_) {
        if (it.status == TransferStatus::InProgress && it.speedBps > 0.0) {
            totalBps += it.speedBps;
        }
    }
    return totalBps;
}

// ---------------- ClipboardFileTransferManager ----------------

#if !defined(_WIN32)
namespace {

std::filesystem::path privateStagingRoot() {
    auto verifyOrCreate0700 = [](const std::filesystem::path& p) -> bool {
        struct stat st{};
        if (::lstat(p.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return false;
            if (st.st_uid != geteuid()) return false;
            if ((st.st_mode & 0077) != 0) {
                if (::chmod(p.c_str(), 0700) != 0) return false;
            }
            return true;
        }
        return ::mkdir(p.c_str(), 0700) == 0;
    };

    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
        std::filesystem::path p = std::filesystem::path(xdg) / "cppdesk_clipboard";
        if (verifyOrCreate0700(p)) return p;
    }
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    std::filesystem::path perUser = std::filesystem::path(tmp) / ("cppdesk_clipboard_" + std::to_string(geteuid()));
    if (verifyOrCreate0700(perUser)) return perUser;

    std::string tmpl = std::string(tmp) + "/cppdesk_stgroot_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (char* created = ::mkdtemp(buf.data())) {
        return std::filesystem::path(created);
    }
    return {};
}

} // namespace
#endif

ClipboardFileTransferManager::ClipboardFileTransferManager()
    : state_(std::make_shared<SharedState>()) {
    static std::atomic<uint32_t> instanceCount{0};
    std::error_code ec;
#if defined(_WIN32)
    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    const std::wstring root = std::wstring(tempPath) + L"CppDesk_Clipboard\\";
    const std::wstring ownPrefix = L"stg_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    stagingDir_ = root + ownPrefix + std::to_wstring(++instanceCount) + L"\\";
    lastSequenceNumber_ = GetClipboardSequenceNumber();
    try {
        const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
        for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
            if (e.is_symlink(ec)) continue;
            const std::wstring name = e.path().filename().wstring();
            const bool leftover = (name.rfind(L"stg_", 0) == 0 && name.rfind(ownPrefix, 0) != 0) ||
                                  e.path().extension() == ".part";
            if (!leftover) continue;
            const auto written = e.last_write_time(ec);
            if (!ec && written < cutoff) std::filesystem::remove_all(e.path(), ec);
        }
    } catch (...) {}
    std::filesystem::create_directories(stagingDir_, ec);
#else
    const std::filesystem::path root = privateStagingRoot();
    const std::string ownPrefix = "stg_" + std::to_string(getpid()) + "_";
    if (!root.empty()) {
        try {
            const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
            for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
                if (e.is_symlink(ec)) continue;
                const std::string name = e.path().filename().string();
                const bool leftover = (name.rfind("stg_", 0) == 0 && name.rfind(ownPrefix, 0) != 0) ||
                                      e.path().extension() == ".part";
                if (!leftover) continue;
                const auto written = e.last_write_time(ec);
                if (!ec && written < cutoff) std::filesystem::remove_all(e.path(), ec);
            }
        } catch (...) {}
        std::string tmpl = (root / (ownPrefix + std::to_string(++instanceCount) + "_XXXXXX")).string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (char* created = ::mkdtemp(buf.data())) {
            std::string dirStr = std::string(created) + "/";
            stagingDir_.assign(dirStr.begin(), dirStr.end());
        }
    }
    lastSequenceNumber_ = 1;
#endif
}

ClipboardFileTransferManager::~ClipboardFileTransferManager() {
    clearOwnOffer();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->destroyed = true;
        state_->incoming.cancelled = true;
        state_->incoming.inProgress = false;
        state_->pendingReq.failed = true;
        state_->pendingReq.active = false;
        state_->activeSendFn = nullptr;
        state_->chunkCv.notify_all();
        state_->incoming = {};
        state_->outgoing = {};
    }
    purgeStaging();
    std::error_code ec;
    if (!stagingDir_.empty()) {
        std::filesystem::remove_all(stagingDir_, ec);
    }
}

void ClipboardFileTransferManager::clearOwnOffer() {
    if (!hasActiveOffer_.exchange(false)) return;
#if defined(_WIN32)
    ShellClipboard::clearClipboard();
#else
    ShellClipboard::clearClipboardIfCurrent(offerGeneration_.load());
#endif
}

void ClipboardFileTransferManager::purgeStaging() {
    std::error_code ec;
    if (!stagingDir_.empty() && std::filesystem::exists(stagingDir_, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(stagingDir_, ec)) {
            if (entry.is_symlink(ec)) continue;
            if (entry.path().extension() == ".part") {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }
}

bool ClipboardFileTransferManager::pollLocalClipboardFiles(const SendPacketFn& sendPacket) {
#if defined(_WIN32)
    uint32_t seq = GetClipboardSequenceNumber();
    if (seq == lastSequenceNumber_) return false;
    lastSequenceNumber_ = seq;
#endif
    // Elsewhere there is no change counter: the files are read every poll and the manifest check below drops repeats.

    std::vector<VirtualFileEntry> files;
    std::vector<std::wstring> fullPaths;
    if (!ShellClipboard::getLocalClipboardFiles(files, fullPaths)) {
        return false;
    }

    uint64_t totalBytes = 0;
    std::string manifest;
    for (const auto& f : files) {
        totalBytes += f.fileSize;
        manifest += std::to_string(f.fileSize) + ":" + wideToUtf8(f.fileName) + ";";
    }

    constexpr uint64_t MAX_CAP = 2ULL * 1024ULL * 1024ULL * 1024ULL; // 2 GB
    if (totalBytes > MAX_CAP) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (manifest == state_->outgoing.manifestHash) {
        return false;
    }

    state_->outgoing.transferId = ++nextTransferId_;
    state_->outgoing.files = files;
    state_->outgoing.fullPaths = fullPaths;
    state_->outgoing.totalBytes = totalBytes;
    state_->outgoing.manifestHash = manifest;

    if (sendPacket) {
        std::vector<uint8_t> pkt;
        serializeClipboardFileList(state_->outgoing.transferId, state_->outgoing.files, pkt);
        sendPacket(PacketType::CLIPBOARD_FILE_LIST, pkt);
    }
    return true;
}

void ClipboardFileTransferManager::handleRemoteFileList(uint32_t transferId, const std::vector<VirtualFileEntry>& files, const SendPacketFn& sendPacket) {
    if (files.empty()) return;

    constexpr uint64_t MAX_CAP = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    uint64_t totalBytes = 0;
    for (const auto& f : files) {
        if (f.fileSize > MAX_CAP || totalBytes > MAX_CAP - f.fileSize) {
            if (sendPacket) {
                ClipboardFileCancelHeader cancelHdr{ transferId, 2 }; // 2 = size limit exceeded
                std::vector<uint8_t> pkt(sizeof(cancelHdr));
                std::memcpy(pkt.data(), &cancelHdr, sizeof(cancelHdr));
                sendPacket(PacketType::CLIPBOARD_FILE_CANCEL, pkt);
            }
            return;
        }
        totalBytes += f.fileSize;
    }
    if (totalBytes > MAX_CAP) {
        if (sendPacket) {
            ClipboardFileCancelHeader cancelHdr{ transferId, 2 }; // 2 = size limit exceeded
            std::vector<uint8_t> pkt(sizeof(cancelHdr));
            std::memcpy(pkt.data(), &cancelHdr, sizeof(cancelHdr));
            sendPacket(PacketType::CLIPBOARD_FILE_CANCEL, pkt);
        }
        return;
    }

    std::vector<VirtualFileEntry> sanitizedFiles = files;
    for (auto& f : sanitizedFiles) {
        std::wstring clean = std::filesystem::path(f.fileName).filename().wstring();
        if (clean.empty() || clean == L"." || clean == L"..") {
            clean = L"received_file_" + std::to_wstring(f.fileIndex) + L".bin";
        }
        for (wchar_t& ch : clean) {
            if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
                ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*' ||
                static_cast<uint16_t>(ch) < 32) {
                ch = L'_';
            }
        }
        size_t pos = 0;
        while ((pos = clean.find(L"..", pos)) != std::wstring::npos) {
            clean.replace(pos, 2, L"__");
            pos += 2;
        }
        if (clean.empty()) {
            clean = L"received_file_" + std::to_wstring(f.fileIndex) + L".bin";
        }
        f.fileName = clean;
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->incoming.transferId = transferId;
        state_->incoming.files = sanitizedFiles;
        state_->incoming.totalBytes = totalBytes;
        state_->incoming.transferredBytes = 0;
        state_->incoming.currentFileIndex = 0;
        state_->incoming.currentFileName = wideToUtf8(sanitizedFiles[0].fileName);
        state_->incoming.inProgress = false;
        state_->incoming.cancelled = false;
        state_->incoming.startTimeMs = 0;
        state_->incoming.transferRateMBs = 0.0f;
        state_->activeSendFn = sendPacket;
    }

    std::weak_ptr<SharedState> weakState = state_;
    auto reader = [weakState](uint32_t fIndex, uint64_t off, uint32_t len, std::vector<uint8_t>& out) {
        auto st = weakState.lock();
        if (!st) return false;
        return ClipboardFileTransferManager::fetchChunkFromState(*st, fIndex, off, len, out);
    };
#if defined(_WIN32)
    if (ShellClipboard::setVirtualClipboardFiles(sanitizedFiles, reader)) {
        hasActiveOffer_.store(true);
    }
#else
    uint64_t generation = 0;
    if (ShellClipboard::setVirtualClipboardFiles(sanitizedFiles, reader, &generation)) {
        offerGeneration_.store(generation);
        hasActiveOffer_.store(true);
    }
#endif
}

bool ClipboardFileTransferManager::fetchChunkFromState(SharedState& st, uint32_t fileIndex, uint64_t offset, uint32_t length, std::vector<uint8_t>& outData) {
    std::unique_lock<std::mutex> lock(st.mutex);
    if (st.destroyed || st.incoming.cancelled) return false;

    st.incoming.inProgress = true;
    st.incoming.currentFileIndex = fileIndex;
    if (fileIndex < st.incoming.files.size()) {
        const auto& f = st.incoming.files[fileIndex];
        st.incoming.currentFileName = wideToUtf8(f.fileName);
    }
    if (st.incoming.startTimeMs == 0) {
        st.incoming.startTimeMs = currentTimeMs();
    }

    st.pendingReq.transferId = st.incoming.transferId;
    st.pendingReq.fileIndex = fileIndex;
    st.pendingReq.offset = offset;
    st.pendingReq.length = length;
    st.pendingReq.active = true;
    st.pendingReq.completed = false;
    st.pendingReq.failed = false;
    st.pendingReq.data.clear();

    SendPacketFn sendFn = st.activeSendFn;
    uint32_t tId = st.incoming.transferId;
    lock.unlock();

    if (sendFn) {
        ClipboardFileRequestHeader reqHdr{};
        reqHdr.transferId = tId;
        reqHdr.fileIndex = fileIndex;
        reqHdr.offset = offset;
        reqHdr.length = length;
        std::vector<uint8_t> pkt(sizeof(reqHdr));
        std::memcpy(pkt.data(), &reqHdr, sizeof(reqHdr));
        if (!sendFn(PacketType::CLIPBOARD_FILE_REQUEST, pkt)) {
            lock.lock();
            st.pendingReq.active = false;
            st.pendingReq.failed = true;
            return false;
        }
    } else {
        lock.lock();
        st.pendingReq.active = false;
        return false;
    }

    lock.lock();
    bool ok = st.chunkCv.wait_for(lock, std::chrono::seconds(15), [&st]() {
        return st.destroyed || st.pendingReq.completed || st.pendingReq.failed || st.incoming.cancelled;
    });

    st.pendingReq.active = false;
    if (!ok || st.destroyed || st.pendingReq.failed || st.incoming.cancelled) {
        return false;
    }

    outData = std::move(st.pendingReq.data);
    st.incoming.transferredBytes += outData.size();

    uint64_t nowMs = currentTimeMs();
    uint64_t elapsedMs = nowMs - st.incoming.startTimeMs;
    if (elapsedMs > 200) {
        double sec = static_cast<double>(elapsedMs) / 1000.0;
        double mb = static_cast<double>(st.incoming.transferredBytes) / (1024.0 * 1024.0);
        st.incoming.transferRateMBs = static_cast<float>(mb / sec);
    }

    if (st.incoming.transferredBytes >= st.incoming.totalBytes) {
        st.incoming.inProgress = false;
    }
    return true;
}

bool ClipboardFileTransferManager::fetchChunk(uint32_t fileIndex, uint64_t offset, uint32_t length, std::vector<uint8_t>& outData) {
    return fetchChunkFromState(*state_, fileIndex, offset, length, outData);
}

void ClipboardFileTransferManager::handleFileRequest(uint32_t transferId, uint32_t fileIndex, uint64_t offset, uint32_t length, const SendPacketFn& sendPacket) {
    std::wstring path;
    uint64_t expectedFileSize = 0;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (transferId != state_->outgoing.transferId ||
            fileIndex >= state_->outgoing.fullPaths.size() ||
            fileIndex >= state_->outgoing.files.size()) {
            return;
        }
        path = state_->outgoing.fullPaths[fileIndex];
        expectedFileSize = state_->outgoing.files[fileIndex].fileSize;
    }

    if (offset > expectedFileSize ||
        offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        if (sendPacket) {
            ClipboardFileCancelHeader cancelHdr{ transferId, 3 }; // 3 = IO/range error
            std::vector<uint8_t> pkt(sizeof(cancelHdr));
            std::memcpy(pkt.data(), &cancelHdr, sizeof(cancelHdr));
            sendPacket(PacketType::CLIPBOARD_FILE_CANCEL, pkt);
        }
        return;
    }

    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file.is_open()) {
        if (sendPacket) {
            ClipboardFileCancelHeader cancelHdr{ transferId, 3 }; // 3 = IO error
            std::vector<uint8_t> pkt(sizeof(cancelHdr));
            std::memcpy(pkt.data(), &cancelHdr, sizeof(cancelHdr));
            sendPacket(PacketType::CLIPBOARD_FILE_CANCEL, pkt);
        }
        return;
    }

    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    uint64_t remaining = expectedFileSize - offset;
    uint32_t readSize = static_cast<uint32_t>(std::min<uint64_t>(std::min<uint32_t>(length, 64 * 1024), remaining));
    std::vector<uint8_t> buf(readSize);
    if (readSize > 0) {
        file.read(reinterpret_cast<char*>(buf.data()), readSize);
        std::streamsize bytesRead = file.gcount();
        if (bytesRead < 0) bytesRead = 0;
        buf.resize(static_cast<size_t>(bytesRead));
    }

    auto digest = CryptoUtils::sha256(buf.data(), buf.size());

    ClipboardFileChunkHeader chunkHdr{};
    chunkHdr.transferId = transferId;
    chunkHdr.fileIndex = fileIndex;
    chunkHdr.offset = offset;
    chunkHdr.dataLength = static_cast<uint32_t>(buf.size());
    std::memcpy(chunkHdr.sha256, digest.data(), 32);

    std::vector<uint8_t> pkt(sizeof(chunkHdr) + buf.size());
    std::memcpy(pkt.data(), &chunkHdr, sizeof(chunkHdr));
    if (!buf.empty()) {
        std::memcpy(pkt.data() + sizeof(chunkHdr), buf.data(), buf.size());
    }
    if (sendPacket) {
        sendPacket(PacketType::CLIPBOARD_FILE_CHUNK, pkt);
    }
}

void ClipboardFileTransferManager::handleFileChunk(uint32_t transferId, uint32_t fileIndex, uint64_t offset, const uint8_t* data, size_t len, const uint8_t sha256[32]) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->destroyed || transferId != state_->incoming.transferId || state_->incoming.cancelled) {
        return;
    }

    // Only accept chunks matching the active in-flight request within declared file bounds.
    if (!state_->pendingReq.active ||
        state_->pendingReq.transferId != transferId ||
        state_->pendingReq.fileIndex != fileIndex ||
        state_->pendingReq.offset != offset) {
        return;
    }

    if (fileIndex >= state_->incoming.files.size() ||
        len > state_->pendingReq.length ||
        offset > state_->incoming.files[fileIndex].fileSize ||
        len > state_->incoming.files[fileIndex].fileSize - offset ||
        (len > 0 && !data)) {
        state_->pendingReq.failed = true;
        state_->pendingReq.active = false;
        state_->chunkCv.notify_all();
        return;
    }

    auto digest = CryptoUtils::sha256(data, len);
    if (std::memcmp(digest.data(), sha256, 32) != 0) {
        state_->pendingReq.failed = true;
        state_->pendingReq.active = false;
        state_->chunkCv.notify_all();
        return;
    }

    state_->pendingReq.data.assign(data, data + len);
    state_->pendingReq.completed = true;
    state_->pendingReq.active = false;
    state_->chunkCv.notify_all();
}

void ClipboardFileTransferManager::handleFileCancel(uint32_t transferId, uint32_t /*reasonCode*/) {
    bool shouldClear = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->incoming.transferId == transferId) {
            state_->incoming.cancelled = true;
            state_->incoming.inProgress = false;
            state_->pendingReq.failed = true;
            state_->pendingReq.active = false;
            state_->chunkCv.notify_all();
            purgeStaging();
            shouldClear = true;
        }
    }
    if (shouldClear) {
        clearOwnOffer();
    }
}

void ClipboardFileTransferManager::cancelActiveTransfer(const SendPacketFn& sendPacket) {
    uint32_t tId = 0;
    SendPacketFn sendFn;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->incoming.cancelled = true;
        state_->incoming.inProgress = false;
        state_->pendingReq.failed = true;
        state_->pendingReq.active = false;
        tId = state_->incoming.transferId;
        sendFn = sendPacket ? sendPacket : state_->activeSendFn;
        state_->chunkCv.notify_all();
        purgeStaging();
    }
    clearOwnOffer();

    if (sendFn && tId > 0) {
        ClipboardFileCancelHeader cancelHdr{ tId, 1 }; // 1 = user cancelled
        std::vector<uint8_t> pkt(sizeof(cancelHdr));
        std::memcpy(pkt.data(), &cancelHdr, sizeof(cancelHdr));
        sendFn(PacketType::CLIPBOARD_FILE_CANCEL, pkt);
    }
}

void ClipboardFileTransferManager::reset() {
    clearOwnOffer();
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->incoming.cancelled = true;
    state_->incoming.inProgress = false;
    state_->pendingReq.failed = true;
    state_->pendingReq.active = false;
    state_->chunkCv.notify_all();
    purgeStaging();
    state_->incoming = {};
    state_->outgoing = {};
    state_->activeSendFn = nullptr;
}

bool ClipboardFileTransferManager::isTransferActive() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->incoming.inProgress && !state_->incoming.cancelled;
}

std::string ClipboardFileTransferManager::activeFileName() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->incoming.currentFileName;
}

float ClipboardFileTransferManager::activeProgressFraction() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->incoming.totalBytes == 0) return 0.0f;
    float frac = static_cast<float>(static_cast<double>(state_->incoming.transferredBytes) / static_cast<double>(state_->incoming.totalBytes));
    return std::clamp(frac, 0.0f, 1.0f);
}

float ClipboardFileTransferManager::activeTransferRateMBs() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->incoming.transferRateMBs;
}

uint64_t ClipboardFileTransferManager::activeTransferredBytes() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->incoming.transferredBytes;
}

uint64_t ClipboardFileTransferManager::activeTotalBytes() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->incoming.totalBytes;
}

uint32_t ClipboardFileTransferManager::activeTransferId() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->incoming.transferId;
}

} // namespace cppdesk
