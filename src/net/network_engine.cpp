#include "network_engine.hpp"
#include "../control/windows_service_manager.hpp"
#include "../capture/virtual_display_manager.hpp"
#include "../capture/screen_blank_manager.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dxgi.h>
#include <cpuid.h>

#include <cstring>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <deque>

namespace cppdesk {

namespace {

#ifndef AUDCLNT_STREAMFLAGS_LOOPBACK
#define AUDCLNT_STREAMFLAGS_LOOPBACK 0x00020000
#endif
#ifndef AUDCLNT_BUFFERFLAGS_SILENT
#define AUDCLNT_BUFFERFLAGS_SILENT 0x2
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static const CLSID CLSID_MMDeviceEnumerator_val = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID IID_IMMDeviceEnumerator_val = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID IID_IAudioClient_val = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const IID IID_IAudioCaptureClient_val = { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_val = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

std::once_flag g_wsaInitFlag;

void ensureWinsockInitialized() {
    std::call_once(g_wsaInitFlag, []() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

uint64_t nowTickMs() {
    return static_cast<uint64_t>(GetTickCount64());
}

SOCKET toWinSock(uintptr_t s) {
    return (s == ~uintptr_t(0)) ? INVALID_SOCKET : static_cast<SOCKET>(s);
}

uintptr_t fromWinSock(SOCKET s) {
    return (s == INVALID_SOCKET) ? ~uintptr_t(0) : static_cast<uintptr_t>(s);
}

void closeWinSock(uintptr_t& s) {
    SOCKET ws = toWinSock(s);
    if (ws != INVALID_SOCKET) {
        shutdown(ws, SD_BOTH);
        closesocket(ws);
        s = ~uintptr_t(0);
    }
}

void setTcpNoDelay(SOCKET s) {
    NetworkEngine::setTcpNoDelay(s);
}

void setSocketTimeoutMs(SOCKET s, DWORD timeoutMs) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
}

bool sendAllBytes(SOCKET s, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t sentTotal = 0;
    while (sentTotal < len) {
        int chunk = static_cast<int>(std::min<size_t>(len - sentTotal, 65536));
        int n = send(s, p + sentTotal, chunk, 0);
        if (n <= 0) return false;
        sentTotal += static_cast<size_t>(n);
    }
    return true;
}

struct CurtainWindowConfig {
    std::wstring brandName = L"CppDesk Enterprise Security";
    std::wstring noticeText = L"Screen output hidden and local physical inputs secured for authorized administration.";
    std::wstring deskIdText = L"";
    bool showDeskId = true;
};
static CurtainWindowConfig s_curtainConfig;

static std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (req <= 0) return L"";
    std::wstring out(req, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &out[0], req);
    return out;
}

static LRESULT CALLBACK PrivacyCurtainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = std::max<int>(1, rc.right - rc.left);
            int h = std::max<int>(1, rc.bottom - rc.top);

            // Double buffering memory DC to eliminate flicker
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            // 1. OLED Deep Dark Background (#0B0E14)
            HBRUSH bgBrush = CreateSolidBrush(RGB(11, 14, 20));
            FillRect(memDC, &rc, bgBrush);
            DeleteObject(bgBrush);

            // 2. Central Frosted Security Card Container
            int cardW = std::clamp(w - 60, 360, 680);
            int cardH = std::clamp(h - 60, 300, 420);
            int cardX = (w - cardW) / 2;
            int cardY = (h - cardH) / 2;
            RECT cardRc = { cardX, cardY, cardX + cardW, cardY + cardH };

            HBRUSH cardBrush = CreateSolidBrush(RGB(20, 24, 34));
            HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(36, 45, 61));
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, cardBrush);
            HPEN oldPen = (HPEN)SelectObject(memDC, cardPen);
            RoundRect(memDC, cardRc.left, cardRc.top, cardRc.right, cardRc.bottom, 22, 22);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(cardBrush);
            DeleteObject(cardPen);

            // 3. Central Shield Polygon Icon (Emerald Accent #10B981)
            int shieldCX = cardX + cardW / 2;
            int shieldCY = cardY + 58;
            POINT shieldPts[5] = {
                { shieldCX, shieldCY - 26 },
                { shieldCX + 24, shieldCY - 14 },
                { shieldCX + 16, shieldCY + 18 },
                { shieldCX, shieldCY + 28 },
                { shieldCX - 16, shieldCY + 18 }
            };
            HBRUSH shieldBrush = CreateSolidBrush(RGB(16, 185, 129));
            HPEN shieldPen = CreatePen(PS_SOLID, 2, RGB(5, 150, 105));
            oldBrush = (HBRUSH)SelectObject(memDC, shieldBrush);
            oldPen = (HPEN)SelectObject(memDC, shieldPen);
            Polygon(memDC, shieldPts, 5);

            // Inner lock core
            HBRUSH innerBrush = CreateSolidBrush(RGB(20, 24, 34));
            SelectObject(memDC, innerBrush);
            Ellipse(memDC, shieldCX - 5, shieldCY - 4, shieldCX + 5, shieldCY + 6);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(shieldBrush);
            DeleteObject(shieldPen);
            DeleteObject(innerBrush);

            // 4. Branded Typography & Badges
            SetBkMode(memDC, TRANSPARENT);

            // Brand Title (Segoe UI Bold 24pt, #F8FAFC)
            HFONT hFontBrand = CreateFontW(25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT hOldFont = (HFONT)SelectObject(memDC, hFontBrand);
            SetTextColor(memDC, RGB(248, 250, 252));
            RECT brandRc = { cardX + 24, shieldCY + 36, cardX + cardW - 24, shieldCY + 70 };
            DrawTextW(memDC, s_curtainConfig.brandName.c_str(), -1, &brandRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Status Pill: [ SECURED PRIVACY CURTAIN ACTIVE ]
            HFONT hFontPill = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SelectObject(memDC, hFontPill);
            SetTextColor(memDC, RGB(16, 185, 129));
            RECT pillRc = { cardX + 24, brandRc.bottom + 2, cardX + cardW - 24, brandRc.bottom + 20 };
            DrawTextW(memDC, L"[ SECURED PRIVACY CURTAIN ACTIVE ]", -1, &pillRc, DT_CENTER | DT_SINGLELINE);

            // Custom Security Notice (#CBD5E1, Segoe UI 15pt)
            HFONT hFontNotice = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SelectObject(memDC, hFontNotice);
            SetTextColor(memDC, RGB(203, 213, 225));
            RECT noticeRc = { cardX + 32, pillRc.bottom + 12, cardX + cardW - 32, pillRc.bottom + 76 };
            DrawTextW(memDC, s_curtainConfig.noticeText.c_str(), -1, &noticeRc, DT_CENTER | DT_WORDBREAK);

            // Desk ID Pill Badge
            HFONT hFontId = nullptr;
            if (s_curtainConfig.showDeskId && !s_curtainConfig.deskIdText.empty()) {
                RECT idBox = { cardX + cardW / 2 - 130, noticeRc.bottom + 8, cardX + cardW / 2 + 130, noticeRc.bottom + 34 };
                HBRUSH idBrush = CreateSolidBrush(RGB(28, 33, 46));
                HPEN idPen = CreatePen(PS_SOLID, 1, RGB(45, 55, 75));
                HBRUSH prevBrush = (HBRUSH)SelectObject(memDC, idBrush);
                HPEN prevPen = (HPEN)SelectObject(memDC, idPen);
                RoundRect(memDC, idBox.left, idBox.top, idBox.right, idBox.bottom, 8, 8);
                SelectObject(memDC, prevBrush);
                SelectObject(memDC, prevPen);
                DeleteObject(idBrush);
                DeleteObject(idPen);

                hFontId = CreateFontW(14, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                SelectObject(memDC, hFontId);
                SetTextColor(memDC, RGB(148, 163, 184));
                DrawTextW(memDC, s_curtainConfig.deskIdText.c_str(), -1, &idBox, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            // Host Physical Abort Failsafe
            HFONT hFontFailsafe = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SelectObject(memDC, hFontFailsafe);
            SetTextColor(memDC, RGB(100, 116, 139));
            RECT hintRc = { cardX + 24, cardRc.bottom - 36, cardX + cardW - 24, cardRc.bottom - 12 };
            DrawTextW(memDC, L"Local emergency failsafe: Press Ctrl+Alt+Del on host physical keyboard to unlock workstation.", -1, &hintRc, DT_CENTER | DT_SINGLELINE);

            // Transfer backbuffer to screen
            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

            // Cleanup GDI objects
            SelectObject(memDC, hOldFont);
            DeleteObject(hFontBrand);
            DeleteObject(hFontPill);
            DeleteObject(hFontNotice);
            if (hFontId) DeleteObject(hFontId);
            DeleteObject(hFontFailsafe);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(nullptr);
            return TRUE;
        case WM_CLOSE:
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool recvAllBytes(SOCKET s, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t gotTotal = 0;
    while (gotTotal < len) {
        int chunk = static_cast<int>(std::min<size_t>(len - gotTotal, 65536));
        int n = recv(s, p + gotTotal, chunk, 0);
        if (n <= 0) return false;
        gotTotal += static_cast<size_t>(n);
    }
    return true;
}

SOCKET connectTcpWithTimeout(const std::string& host, uint16_t port, int timeoutMs) {
    ensureWinsockInitialized();

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return INVALID_SOCKET;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }

    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);

    int rc = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);

    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        fd_set wfds{}, efds{};
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);

        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int sel = select(0, nullptr, &wfds, &efds, &tv);
        if (sel <= 0 || FD_ISSET(s, &efds) || !FD_ISSET(s, &wfds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        int soErr = 0;
        int soLen = sizeof(soErr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen);
        if (soErr != 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    nonBlock = 0;
    ioctlsocket(s, FIONBIO, &nonBlock);
    setTcpNoDelay(s);
    return s;
}

bool parseHostPort(const std::string& input, std::string& outHost, uint16_t& outPort, uint16_t defaultPort) {
    if (input.empty()) return false;
    auto colon = input.rfind(':');
    if (colon != std::string::npos) {
        outHost = input.substr(0, colon);
        try {
            int p = std::stoi(input.substr(colon + 1));
            if (p > 0 && p <= 65535) {
                outPort = static_cast<uint16_t>(p);
                return !outHost.empty();
            }
        } catch (...) {
            return false;
        }
    }
    outHost = input;
    outPort = defaultPort;
    return !outHost.empty();
}

std::string detectPrimaryLocalIpv4() {
    ensureWinsockInitialized();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
            sockaddr_in local{};
            int len = sizeof(local);
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                char ipStr[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &local.sin_addr, ipStr, sizeof(ipStr))) {
                    closesocket(s);
                    return std::string(ipStr);
                }
            }
        }
        closesocket(s);
    }
    return "127.0.0.1";
}

void spliceSockets(SOCKET a, SOCKET b) {
    char buf[32768];
    while (true) {
        int n = recv(a, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (!sendAllBytes(b, buf, static_cast<size_t>(n))) break;
    }
    shutdown(a, SD_BOTH);
    shutdown(b, SD_BOTH);
}

void executeRemoteSystemAction(SystemActionType action) {
    InputInjector::executeSystemAction(action);
}

} // namespace

// ---------------- Frame IO ----------------

void NetworkEngine::setTcpNoDelay(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    BOOL flag = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    // Scale TCP socket send and receive buffers to 2 MB (2,097,152 bytes)
    // to prevent TCP window throttling on high-bitrate video and data transfer
    int bufSize = 2 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
}

bool NetworkEngine::sendScatterGather(
    SOCKET s,
    const void* hdrBuf,
    size_t hdrLen,
    const void* payloadBuf,
    size_t payloadLen)
{
    if (s == INVALID_SOCKET || (!hdrBuf && hdrLen > 0)) return false;

    WSABUF bufs[2];
    bufs[0].buf = const_cast<char*>(static_cast<const char*>(hdrBuf));
    bufs[0].len = static_cast<ULONG>(hdrLen);

    DWORD numBufs = 1;
    if (payloadBuf && payloadLen > 0) {
        bufs[1].buf = const_cast<char*>(static_cast<const char*>(payloadBuf));
        bufs[1].len = static_cast<ULONG>(payloadLen);
        numBufs = 2;
    }

    size_t totalBytes = hdrLen + ((payloadBuf && payloadLen > 0) ? payloadLen : 0);
    if (totalBytes == 0) return true;
    size_t totalSent = 0;
    DWORD curBufIdx = 0;

    while (totalSent < totalBytes) {
        DWORD bytesSent = 0;
        int res = WSASend(s, &bufs[curBufIdx], numBufs - curBufIdx, &bytesSent, 0, nullptr, nullptr);
        if (res == SOCKET_ERROR || bytesSent == 0) {
            return false;
        }

        totalSent += bytesSent;
        if (totalSent >= totalBytes) break;

        DWORD remainingSent = bytesSent;
        while (curBufIdx < numBufs && remainingSent > 0) {
            if (remainingSent >= bufs[curBufIdx].len) {
                remainingSent -= bufs[curBufIdx].len;
                bufs[curBufIdx].len = 0;
                curBufIdx++;
            } else {
                bufs[curBufIdx].buf += remainingSent;
                bufs[curBufIdx].len -= remainingSent;
                remainingSent = 0;
            }
        }
    }
    return true;
}

bool NetworkEngine::sendFrame(
    uintptr_t sock,
    PacketType type,
    uint8_t flags,
    const void* payload,
    size_t payloadLen,
    std::mutex& sendMutex,
    AesGcmSessionCipher* cipher,
    uint64_t* sendSeq)
{
    SOCKET s = toWinSock(sock);
    if (s == INVALID_SOCKET || payloadLen > MAX_PACKET_PAYLOAD_SIZE) return false;

    std::lock_guard<std::mutex> lock(sendMutex);

    FrameHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(type);
    hdr.flags = flags;

    if (cipher && cipher->isInitialized() && sendSeq) {
        hdr.flags |= FLAG_ENCRYPTED;
        uint64_t seq = (*sendSeq)++;

        // Output layout: [12-byte Nonce][Ciphertext][16-byte Tag]
        hdr.payloadSize = static_cast<uint32_t>(12 + payloadLen + 16);

        thread_local std::vector<uint8_t> tl_encryptedPayload;
        // Authenticate with AAD = hdr
        if (!cipher->encrypt(payload, payloadLen, seq, &hdr, sizeof(hdr), tl_encryptedPayload)) {
            return false;
        }

        return sendScatterGather(s, &hdr, sizeof(hdr), tl_encryptedPayload.data(), tl_encryptedPayload.size());
    } else {
        hdr.payloadSize = static_cast<uint32_t>(payloadLen);
        if (sendSeq) (*sendSeq)++;
        return sendScatterGather(s, &hdr, sizeof(hdr), payload, payloadLen);
    }
}

bool NetworkEngine::recvFrame(
    uintptr_t sock,
    FrameHeader& outHeader,
    std::vector<uint8_t>& outPayload,
    AesGcmSessionCipher* cipher,
    uint64_t* recvSeq)
{
    SOCKET s = toWinSock(sock);
    if (s == INVALID_SOCKET) return false;

    if (!recvAllBytes(s, &outHeader, sizeof(outHeader))) {
        return false;
    }
    if (outHeader.magic != PROTOCOL_MAGIC || outHeader.payloadSize > MAX_PACKET_PAYLOAD_SIZE) {
        return false;
    }

    if (cipher && cipher->isInitialized() && recvSeq) {
        // Post-authentication frames must carry FLAG_ENCRYPTED
        if ((outHeader.flags & FLAG_ENCRYPTED) == 0 || outHeader.payloadSize < 28) {
            return false;
        }

        thread_local std::vector<uint8_t> tl_encryptedPayload;
        tl_encryptedPayload.resize(outHeader.payloadSize);
        if (!recvAllBytes(s, tl_encryptedPayload.data(), outHeader.payloadSize)) {
            return false;
        }

        uint64_t pktSeq = 0;
        // Authenticate with AAD = outHeader and decrypt directly into outPayload
        if (!cipher->decrypt(tl_encryptedPayload.data(), tl_encryptedPayload.size(), &outHeader, sizeof(outHeader), outPayload, &pktSeq)) {
            return false; // Authentication tag mismatch or replay detected!
        }
        *recvSeq = pktSeq;
    } else {
        outPayload.resize(outHeader.payloadSize);
        if (outHeader.payloadSize > 0) {
            if (!recvAllBytes(s, outPayload.data(), outHeader.payloadSize)) {
                return false;
            }
        }
    }
    return true;
}

bool NetworkEngine::sendHostEncryptedPacket(PacketType type, uint8_t flags, const void* payload, size_t payloadLen) {
    uintptr_t cs = activeHostClientSock_.load();
    if (cs == ~uintptr_t(0)) return false;
    bool enc = hostEncrypted_.load();
    std::lock_guard<std::mutex> lock(hostCipherMutex_);
    return sendFrame(cs, type, flags, payload, payloadLen, hostSendMutex_,
                     enc ? &hostCipher_ : nullptr,
                     enc ? &hostSendSeq_ : nullptr);
}

bool NetworkEngine::sendViewerEncryptedPacket(PacketType type, uint8_t flags, const void* payload, size_t payloadLen) {
    uintptr_t vs = viewerSock_.load();
    if (vs == ~uintptr_t(0)) return false;
    bool enc = viewerEncrypted_.load();
    std::lock_guard<std::mutex> lock(viewerCipherMutex_);
    return sendFrame(vs, type, flags, payload, payloadLen, viewerSendMutex_,
                     enc ? &viewerCipher_ : nullptr,
                     enc ? &viewerSendSeq_ : nullptr);
}

// ---------------- Brute-Force IP Protection ----------------

bool NetworkEngine::isIpRateLimited(const std::string& ip) {
    std::lock_guard<std::mutex> lock(authRateMutex_);
    auto it = authRateMap_.find(ip);
    if (it == authRateMap_.end()) return false;
    uint64_t now = nowTickMs();
    if (it->second.lockedUntilMs > now) {
        return true;
    }
    if (it->second.lockedUntilMs != 0 && now >= it->second.lockedUntilMs) {
        it->second = BruteForceRecord{};
    }
    return false;
}

void NetworkEngine::recordAuthResultForIp(const std::string& ip, bool success) {
    std::lock_guard<std::mutex> lock(authRateMutex_);
    if (success) {
        authRateMap_.erase(ip);
        return;
    }
    uint64_t now = nowTickMs();
    auto& rec = authRateMap_[ip];
    if (rec.windowStartMs == 0 || (now - rec.windowStartMs > 60000)) {
        rec.windowStartMs = now;
        rec.failedCount = 1;
        rec.lockedUntilMs = 0;
    } else {
        rec.failedCount++;
        if (rec.failedCount >= 5) {
            rec.lockedUntilMs = now + 60000; // 60s lockout after 5 bad password attempts
        }
    }
}

void NetworkEngine::clearRateLimitRecords() {
    std::lock_guard<std::mutex> lock(authRateMutex_);
    authRateMap_.clear();
}

// ---------------- RelayServer ----------------

RelayServer::RelayServer() = default;

RelayServer::~RelayServer() {
    stop();
}

bool RelayServer::start(uint16_t port) {
    ensureWinsockInitialized();
    if (running_.load()) return true;

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(ls, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(ls);
        return false;
    }

    port_ = port;
    listenSock_ = fromWinSock(ls);
    running_.store(true);
    acceptThread_ = std::thread(&RelayServer::acceptLoop, this);
    return true;
}

void RelayServer::stop() {
    if (!running_.exchange(false)) return;
    closeWinSock(listenSock_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& h : hosts_) {
            closeWinSock(h.controlSock);
        }
        hosts_.clear();
        for (auto& b : bridges_) {
            closeWinSock(b.viewerSock);
            closeWinSock(b.hostSock);
        }
        bridges_.clear();
    }
    bridgeCv_.notify_all();
    try {
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
    } catch (...) {}
}

size_t RelayServer::registeredPeerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hosts_.size();
}

void RelayServer::acceptLoop() {
    while (running_.load()) {
        SOCKET ls = toWinSock(listenSock_);
        if (ls == INVALID_SOCKET) break;

        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        SOCKET cs = accept(ls, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (cs == INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }
        setTcpNoDelay(cs);

        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::string peerIp = ipStr;

        std::thread(&RelayServer::handleClientSocket, this, fromWinSock(cs), peerIp).detach();
    }
}

void RelayServer::handleClientSocket(uintptr_t sock, std::string peerIp) {
    FrameHeader hdr{};
    std::vector<uint8_t> payload;

    setSocketTimeoutMs(toWinSock(sock), 8000);

    if (!recvAllBytes(toWinSock(sock), &hdr, sizeof(hdr)) ||
        hdr.magic != PROTOCOL_MAGIC || hdr.payloadSize > 65536) {
        closeWinSock(sock);
        return;
    }
    payload.resize(hdr.payloadSize);
    if (hdr.payloadSize > 0 && !recvAllBytes(toWinSock(sock), payload.data(), hdr.payloadSize)) {
        closeWinSock(sock);
        return;
    }

    PacketType pt = static_cast<PacketType>(hdr.type);

    if (pt == PacketType::RELAY_REGISTER) {
        try {
            ByteReader r(payload);
            uint64_t deskId = r.readU64();
            std::string hostname = r.readString();
            std::string reportedIp = r.readString();
            uint16_t tcpPort = r.readU16();

            std::string effectiveIp = (peerIp == "127.0.0.1" && !reportedIp.empty()) ? reportedIp : peerIp;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                hosts_.erase(
                    std::remove_if(hosts_.begin(), hosts_.end(), [&](RegisteredHost& h) {
                        if (h.deskId == deskId) {
                            closeWinSock(h.controlSock);
                            return true;
                        }
                        return false;
                    }),
                    hosts_.end()
                );
                RegisteredHost rh;
                rh.deskId = deskId;
                rh.hostname = hostname;
                rh.ip = effectiveIp;
                rh.tcpPort = tcpPort;
                rh.controlSock = sock;
                rh.lastSeenMs = nowTickMs();
                hosts_.push_back(rh);
            }

            ByteWriter w;
            w.writeU8(1);
            FrameHeader ackHdr{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_REGISTER_ACK), 0, static_cast<uint32_t>(w.buffer().size()) };
            sendAllBytes(toWinSock(sock), &ackHdr, sizeof(ackHdr));
            sendAllBytes(toWinSock(sock), w.buffer().data(), w.buffer().size());

            setSocketTimeoutMs(toWinSock(sock), 30000);
            while (running_.load()) {
                FrameHeader chdr{};
                if (!recvAllBytes(toWinSock(sock), &chdr, sizeof(chdr)) || chdr.magic != PROTOCOL_MAGIC) {
                    break;
                }
                std::vector<uint8_t> cpay(chdr.payloadSize);
                if (chdr.payloadSize > 0 && !recvAllBytes(toWinSock(sock), cpay.data(), chdr.payloadSize)) {
                    break;
                }
                if (static_cast<PacketType>(chdr.type) == PacketType::PING) {
                    FrameHeader pong{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::PONG), 0, 0 };
                    sendAllBytes(toWinSock(sock), &pong, sizeof(pong));
                }
            }

            std::lock_guard<std::mutex> lock(mutex_);
            hosts_.erase(
                std::remove_if(hosts_.begin(), hosts_.end(), [&](const RegisteredHost& h) {
                    return h.controlSock == sock;
                }),
                hosts_.end()
            );
        } catch (...) {}
        closeWinSock(sock);
        return;
    }

    if (pt == PacketType::RELAY_LOOKUP) {
        try {
            ByteReader r(payload);
            uint64_t targetId = r.readU64();

            bool found = false;
            std::string hostIp, hostName;
            uint16_t hostPort = 0;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& h : hosts_) {
                    if (h.deskId == targetId) {
                        found = true;
                        hostIp = h.ip;
                        hostPort = h.tcpPort;
                        hostName = h.hostname;
                        break;
                    }
                }
            }

            ByteWriter w;
            w.writeU8(found ? 1 : 0);
            w.writeString(hostIp);
            w.writeU16(hostPort);
            w.writeString(hostName);

            FrameHeader respHdr{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_LOOKUP_RESP), 0, static_cast<uint32_t>(w.buffer().size()) };
            sendAllBytes(toWinSock(sock), &respHdr, sizeof(respHdr));
            sendAllBytes(toWinSock(sock), w.buffer().data(), w.buffer().size());
        } catch (...) {}
        closeWinSock(sock);
        return;
    }

    if (pt == PacketType::RELAY_CONNECT_REQ) {
        try {
            ByteReader r(payload);
            uint64_t targetId = r.readU64();

            uintptr_t hostCtrl = ~uintptr_t(0);
            uint64_t token = 0;
            CryptoUtils::randomBytes(&token, sizeof(token));

            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& h : hosts_) {
                    if (h.deskId == targetId) {
                        hostCtrl = h.controlSock;
                        break;
                    }
                }
                if (hostCtrl != ~uintptr_t(0)) {
                    PendingBridge pb;
                    pb.token = token;
                    pb.viewerSock = sock;
                    pb.ready = false;
                    bridges_.push_back(pb);
                }
            }

            if (hostCtrl == ~uintptr_t(0)) {
                ByteWriter w;
                w.writeU8(0);
                FrameHeader rh{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_BRIDGE_READY), 0, static_cast<uint32_t>(w.buffer().size()) };
                sendAllBytes(toWinSock(sock), &rh, sizeof(rh));
                sendAllBytes(toWinSock(sock), w.buffer().data(), w.buffer().size());
                closeWinSock(sock);
                return;
            }

            ByteWriter reqW;
            reqW.writeU64(token);
            FrameHeader reqH{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_INCOMING_REQ), 0, static_cast<uint32_t>(reqW.buffer().size()) };
            sendAllBytes(toWinSock(hostCtrl), &reqH, sizeof(reqH));
            sendAllBytes(toWinSock(hostCtrl), reqW.buffer().data(), reqW.buffer().size());

            uintptr_t matchedHostSock = ~uintptr_t(0);
            {
                std::unique_lock<std::mutex> lock(mutex_);
                bridgeCv_.wait_for(lock, std::chrono::milliseconds(3500), [&]() {
                    for (const auto& b : bridges_) {
                        if (b.token == token && b.ready) return true;
                    }
                    return !running_.load();
                });
                for (auto it = bridges_.begin(); it != bridges_.end(); ++it) {
                    if (it->token == token) {
                        if (it->ready) matchedHostSock = it->hostSock;
                        bridges_.erase(it);
                        break;
                    }
                }
            }

            if (matchedHostSock == ~uintptr_t(0)) {
                ByteWriter w;
                w.writeU8(0);
                FrameHeader rh{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_BRIDGE_READY), 0, static_cast<uint32_t>(w.buffer().size()) };
                sendAllBytes(toWinSock(sock), &rh, sizeof(rh));
                sendAllBytes(toWinSock(sock), w.buffer().data(), w.buffer().size());
                closeWinSock(sock);
                return;
            }

            setSocketTimeoutMs(toWinSock(sock), 0);
            setSocketTimeoutMs(toWinSock(matchedHostSock), 0);

            ByteWriter w;
            w.writeU8(1);
            FrameHeader rh{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_BRIDGE_READY), 0, static_cast<uint32_t>(w.buffer().size()) };
            sendAllBytes(toWinSock(sock), &rh, sizeof(rh));
            sendAllBytes(toWinSock(sock), w.buffer().data(), w.buffer().size());

            std::thread t(spliceSockets, toWinSock(sock), toWinSock(matchedHostSock));
            spliceSockets(toWinSock(matchedHostSock), toWinSock(sock));
            if (t.joinable()) t.join();

            closeWinSock(matchedHostSock);
            closeWinSock(sock);
            return;
        } catch (...) {
            closeWinSock(sock);
            return;
        }
    }

    if (pt == PacketType::RELAY_BRIDGE_ACCEPT) {
        try {
            ByteReader r(payload);
            uint64_t token = r.readU64();
            bool matched = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto& b : bridges_) {
                    if (b.token == token && !b.ready) {
                        b.hostSock = sock;
                        b.ready = true;
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) {
                bridgeCv_.notify_all();
                return;
            }
        } catch (...) {}
        closeWinSock(sock);
        return;
    }

    closeWinSock(sock);
}

// ---------------- NetworkEngine ----------------

NetworkEngine::NetworkEngine(IdentityManager& identity)
    : identity_(identity)
{
    ensureWinsockInitialized();
    localIp_ = detectPrimaryLocalIpv4();
    autoAcceptIncoming_.store(identity_.settings().autoAcceptIncoming);
    autoAcceptPerms_.store(identity_.settings().defaultPermissions);
}

NetworkEngine::~NetworkEngine() {
    stop();
}

bool NetworkEngine::start() {
    if (running_.load()) return true;

    uint16_t basePort = identity_.listenPort();
    SOCKET hs = INVALID_SOCKET;
    uint16_t boundPort = basePort;

    for (uint16_t offset = 0; offset < 12; ++offset) {
        uint16_t candidate = static_cast<uint16_t>(basePort + offset);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(candidate);

        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
            listen(s, SOMAXCONN) == 0) {
            hs = s;
            boundPort = candidate;
            break;
        }
        closesocket(s);
    }

    if (hs == INVALID_SOCKET) {
        return false;
    }

    identity_.setListenPort(boundPort);
    hostListenSock_ = fromWinSock(hs);

    SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (us != INVALID_SOCKET) {
        BOOL broadcast = TRUE;
        BOOL reuse = TRUE;
        setsockopt(us, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
        setsockopt(us, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        int slotOffset = std::clamp(static_cast<int>(boundPort) - static_cast<int>(DEFAULT_HOST_PORT), 0, 5);
        uint16_t udpPort = static_cast<uint16_t>(DEFAULT_DISCOVERY_PORT - slotOffset);

        sockaddr_in uaddr{};
        uaddr.sin_family = AF_INET;
        uaddr.sin_addr.s_addr = INADDR_ANY;
        uaddr.sin_port = htons(udpPort);
        if (bind(us, reinterpret_cast<sockaddr*>(&uaddr), sizeof(uaddr)) != 0) {
            uaddr.sin_port = 0;
            bind(us, reinterpret_cast<sockaddr*>(&uaddr), sizeof(uaddr));
        }
        udpSock_ = fromWinSock(us);
    }

    if (identity_.instanceId() == 1) {
        localRelay_.start(DEFAULT_RELAY_PORT);
    }

    running_.store(true);
    discoveryThread_ = std::thread(&NetworkEngine::discoveryLoop, this);
    relayRegThread_ = std::thread(&NetworkEngine::relayRegistrationLoop, this);
    hostAcceptThread_ = std::thread(&NetworkEngine::hostAcceptLoop, this);

    return true;
}

void NetworkEngine::stop() {
    if (!running_.exchange(false)) return;

    stopHostAudioCapture();
    stopHostTerminal();
    stopHostTunnelProxy();
    destroyPrivacyCurtainWindow();
    ScreenBlankManager::instance().setEmergencyWakeCallback(nullptr);
    ScreenBlankManager::instance().disengageBlanking();
    ScreenBlankManager::wakeDisplays();
    shutdownViewerAudioPlayback();
    stopViewerTunnelMultiplexer();

    disconnectViewer();
    disconnectHostClient();
    respondToIncomingRequest(false, 0);

    closeWinSock(hostListenSock_);
    closeWinSock(udpSock_);
    closeWinSock(relayControlSock_);
    localRelay_.stop();

    try { if (discoveryThread_.joinable()) discoveryThread_.join(); } catch (...) {}
    try { if (relayRegThread_.joinable()) relayRegThread_.join(); } catch (...) {}
    try { if (hostAcceptThread_.joinable()) hostAcceptThread_.join(); } catch (...) {}
    {
        std::lock_guard<std::mutex> lk(hostSessionThreadMutex_);
        try { if (hostSessionThread_.joinable()) hostSessionThread_.join(); } catch (...) {}
    }
    {
        std::lock_guard<std::mutex> lk(viewerThreadMutex_);
        try { if (viewerThread_.joinable()) viewerThread_.join(); } catch (...) {}
    }
    {
        std::lock_guard<std::mutex> lk(netDiagMutex_);
        try { if (netDiagThread_.joinable()) netDiagThread_.join(); } catch (...) {}
    }
}

std::vector<DiscoveredPeer> NetworkEngine::discoveredPeers() const {
    std::lock_guard<std::mutex> lock(peersMutex_);
    uint64_t now = nowTickMs();
    std::vector<DiscoveredPeer> active;
    for (const auto& p : peers_) {
        if (now - p.lastSeenTickMs <= 12000) {
            active.push_back(p);
        }
    }
    return active;
}

void NetworkEngine::sendDiscoveryQuery(uint64_t targetDeskId) {
    SOCKET us = toWinSock(udpSock_);
    if (us == INVALID_SOCKET) return;

    DiscoveryBeaconPacket pkt{};
    pkt.magic = PROTOCOL_MAGIC;
    pkt.packetKind = (targetDeskId == 0) ? 1 : 2;
    pkt.deskId = identity_.deskId();
    pkt.queryId = targetDeskId;
    pkt.tcpPort = identity_.listenPort();
    std::strncpy(pkt.hostname, identity_.hostname().c_str(), sizeof(pkt.hostname) - 1);

    for (int slot = 0; slot < 6; ++slot) {
        uint16_t port = static_cast<uint16_t>(DEFAULT_DISCOVERY_PORT - slot);

        sockaddr_in loopAddr{};
        loopAddr.sin_family = AF_INET;
        loopAddr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &loopAddr.sin_addr);
        sendto(us, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
               reinterpret_cast<sockaddr*>(&loopAddr), sizeof(loopAddr));

        sockaddr_in bcastAddr{};
        bcastAddr.sin_family = AF_INET;
        bcastAddr.sin_port = htons(port);
        bcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(us, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
               reinterpret_cast<sockaddr*>(&bcastAddr), sizeof(bcastAddr));
    }
}

void NetworkEngine::discoveryLoop() {
    uint64_t lastBroadcast = 0;

    while (running_.load()) {
        SOCKET us = toWinSock(udpSock_);
        if (us == INVALID_SOCKET) break;

        uint64_t now = nowTickMs();
        if (now - lastBroadcast >= 1500) {
            sendDiscoveryQuery(0);
            lastBroadcast = now;
        }

        fd_set rfds{};
        FD_ZERO(&rfds);
        FD_SET(us, &rfds);
        timeval tv{ 0, 150000 };

        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(us, &rfds)) {
            sockaddr_in fromAddr{};
            int fromLen = sizeof(fromAddr);
            DiscoveryBeaconPacket pkt{};
            int n = recvfrom(us, reinterpret_cast<char*>(&pkt), sizeof(pkt), 0,
                             reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
            if (n == sizeof(DiscoveryBeaconPacket) && pkt.magic == PROTOCOL_MAGIC) {
                if (pkt.deskId != identity_.deskId() && pkt.deskId >= 100000000ULL) {
                    char ipStr[INET_ADDRSTRLEN] = {};
                    inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));
                    std::string senderIp = ipStr;

                    pkt.hostname[sizeof(pkt.hostname) - 1] = '\0';

                    {
                        std::lock_guard<std::mutex> lock(peersMutex_);
                        bool updated = false;
                        for (auto& p : peers_) {
                            if (p.deskId == pkt.deskId) {
                                p.hostname = pkt.hostname;
                                p.ip = senderIp;
                                p.port = pkt.tcpPort;
                                p.lastSeenTickMs = nowTickMs();
                                p.viaRelay = false;
                                updated = true;
                                break;
                            }
                        }
                        if (!updated) {
                            DiscoveredPeer np;
                            np.deskId = pkt.deskId;
                            np.hostname = pkt.hostname;
                            np.ip = senderIp;
                            np.port = pkt.tcpPort;
                            np.lastSeenTickMs = nowTickMs();
                            np.viaRelay = false;
                            peers_.push_back(np);
                        }
                    }

                    if (pkt.packetKind == 2 && (pkt.queryId == 0 || pkt.queryId == identity_.deskId())) {
                        DiscoveryBeaconPacket reply{};
                        reply.magic = PROTOCOL_MAGIC;
                        reply.packetKind = 1;
                        reply.deskId = identity_.deskId();
                        reply.queryId = 0;
                        reply.tcpPort = identity_.listenPort();
                        std::strncpy(reply.hostname, identity_.hostname().c_str(), sizeof(reply.hostname) - 1);
                        sendto(us, reinterpret_cast<const char*>(&reply), sizeof(reply), 0,
                               reinterpret_cast<sockaddr*>(&fromAddr), sizeof(fromAddr));
                    }
                }
            }
        }
    }
}

void NetworkEngine::relayRegistrationLoop() {
    while (running_.load()) {
        std::string relayAddr = identity_.relayServerAddress();
        std::string rHost;
        uint16_t rPort = DEFAULT_RELAY_PORT;
        if (!parseHostPort(relayAddr, rHost, rPort, DEFAULT_RELAY_PORT)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            continue;
        }

        SOCKET rs = connectTcpWithTimeout(rHost, rPort, 1200);
        if (rs == INVALID_SOCKET) {
            for (int i = 0; i < 15 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            continue;
        }

        relayControlSock_ = fromWinSock(rs);

        ByteWriter w;
        w.writeU64(identity_.deskId());
        w.writeString(identity_.hostname());
        w.writeString(localIp_);
        w.writeU16(identity_.listenPort());

        std::mutex regMutex;
        if (!sendFrame(relayControlSock_, PacketType::RELAY_REGISTER, 0, w.buffer().data(), w.buffer().size(), regMutex)) {
            closeWinSock(relayControlSock_);
            continue;
        }

        while (running_.load()) {
            SOCKET s = toWinSock(relayControlSock_);
            if (s == INVALID_SOCKET) break;

            fd_set rfds{};
            FD_ZERO(&rfds);
            FD_SET(s, &rfds);
            timeval tv{ 2, 0 };
            int sel = select(0, &rfds, nullptr, nullptr, &tv);
            if (sel == 0) {
                if (!sendFrame(relayControlSock_, PacketType::PING, 0, nullptr, 0, regMutex)) {
                    break;
                }
                continue;
            }
            if (sel < 0) break;

            FrameHeader hdr{};
            std::vector<uint8_t> pay;
            if (!recvFrame(relayControlSock_, hdr, pay)) {
                break;
            }

            PacketType pt = static_cast<PacketType>(hdr.type);
            if (pt == PacketType::RELAY_INCOMING_REQ) {
                try {
                    ByteReader r(pay);
                    uint64_t bridgeToken = r.readU64();

                    SOCKET bs = connectTcpWithTimeout(rHost, rPort, 1500);
                    if (bs != INVALID_SOCKET) {
                        ByteWriter bw;
                        bw.writeU64(bridgeToken);
                        std::mutex bm;
                        uintptr_t bSock = fromWinSock(bs);
                        if (sendFrame(bSock, PacketType::RELAY_BRIDGE_ACCEPT, 0, bw.buffer().data(), bw.buffer().size(), bm)) {
                            if (activeHostClientSock_.load() == ~uintptr_t(0)) {
                                std::lock_guard<std::mutex> slk(hostSessionThreadMutex_);
                                try {
                                    if (hostSessionThread_.joinable()) hostSessionThread_.join();
                                } catch (...) {}
                                hostSessionThread_ = std::thread(&NetworkEngine::runHostSession, this, bSock, "Relay-Bridge");
                            } else {
                                closeWinSock(bSock);
                            }
                        } else {
                            closeWinSock(bSock);
                        }
                    }
                } catch (...) {}
            }
        }

        closeWinSock(relayControlSock_);
        for (int i = 0; i < 10 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

bool NetworkEngine::startLocalRelayServer(uint16_t port) {
    return localRelay_.start(port);
}

void NetworkEngine::stopLocalRelayServer() {
    localRelay_.stop();
}

// ---------------- Host Session Server ----------------

PendingIncomingRequest NetworkEngine::pendingIncomingRequest() const {
    std::lock_guard<std::mutex> lock(approvalMutex_);
    return pendingReq_;
}

void NetworkEngine::respondToIncomingRequest(bool accept, uint8_t permissions) {
    {
        std::lock_guard<std::mutex> lock(approvalMutex_);
        if (!pendingReq_.active) return;
        approvalDecided_ = true;
        approvalAccepted_ = accept;
        approvalPermissions_ = permissions;
        pendingReq_.active = false;
    }
    approvalCv_.notify_all();
}

void NetworkEngine::setAutoAcceptIncoming(bool autoAccept, uint8_t defaultPerms) {
    autoAcceptIncoming_.store(autoAccept);
    autoAcceptPerms_.store(defaultPerms);
}

HostSessionStatus NetworkEngine::hostSessionStatus() const {
    std::lock_guard<std::mutex> lock(hostStatusMutex_);
    return hostStatus_;
}

void NetworkEngine::updateHostSessionPermissions(uint8_t newPermissions) {
    hostLivePermissions_.store(newPermissions);
    {
        std::lock_guard<std::mutex> lock(hostStatusMutex_);
        hostStatus_.permissions = newPermissions;
    }
    ByteWriter w;
    w.writeU8(newPermissions);
    sendHostEncryptedPacket(PacketType::PERMISSION_UPDATE, 0, w.buffer().data(), w.buffer().size());
}

void NetworkEngine::disconnectHostClient() {
    stopVoiceIntercom();
    displayManager_.restoreResolution();
    clipFileMgr_.reset();
    uintptr_t cs = activeHostClientSock_.exchange(~uintptr_t(0));
    if (cs != ~uintptr_t(0)) {
        bool enc = hostEncrypted_.exchange(false);
        {
            std::lock_guard<std::mutex> lock(hostCipherMutex_);
            sendFrame(cs, PacketType::DISCONNECT, 0, nullptr, 0, hostSendMutex_,
                      enc ? &hostCipher_ : nullptr,
                      enc ? &hostSendSeq_ : nullptr);
            hostCipher_.reset();
        }
        closeWinSock(cs);
    }
}

void NetworkEngine::hostAcceptLoop() {
    while (running_.load()) {
        SOCKET ls = toWinSock(hostListenSock_);
        if (ls == INVALID_SOCKET) break;

        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        SOCKET cs = accept(ls, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (cs == INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }
        setTcpNoDelay(cs);

        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::string clientIp = ipStr;

        uintptr_t clientSock = fromWinSock(cs);

        // Enforce per-IP brute-force lockout before spawning session thread
        if (isIpRateLimited(clientIp)) {
            ByteWriter w;
            w.writeU8(static_cast<uint8_t>(AuthResultCode::RateLimited));
            w.writeU8(0);
            w.writeString("Too many failed password attempts. Please wait 60 seconds.");
            std::mutex m;
            sendFrame(clientSock, PacketType::AUTH_RESULT, 0, w.buffer().data(), w.buffer().size(), m);
            shutdown(toWinSock(clientSock), SD_SEND);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            closeWinSock(clientSock);
            continue;
        }

        if (activeHostClientSock_.load() != ~uintptr_t(0)) {
            ByteWriter w;
            w.writeU8(static_cast<uint8_t>(AuthResultCode::HostBusy));
            w.writeU8(0);
            w.writeString("Host is already in an active remote session.");
            std::mutex m;
            sendFrame(clientSock, PacketType::AUTH_RESULT, 0, w.buffer().data(), w.buffer().size(), m);
            shutdown(toWinSock(clientSock), SD_SEND);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            closeWinSock(clientSock);
            continue;
        }

        {
            std::lock_guard<std::mutex> slk(hostSessionThreadMutex_);
            try {
                if (hostSessionThread_.joinable()) {
                    hostSessionThread_.join();
                }
            } catch (...) {}
            hostSessionThread_ = std::thread(&NetworkEngine::runHostSession, this, clientSock, clientIp);
        }
    }
}

void NetworkEngine::runHostSession(uintptr_t clientSock, std::string clientIp) {
    activeHostClientSock_.store(clientSock);
    hostEncrypted_.store(false);

    auto cleanup = [&]() {
        stopHostAudioCapture();
        destroyPrivacyCurtainWindow();
        ScreenBlankManager::instance().setEmergencyWakeCallback(nullptr);
        ScreenBlankManager::instance().disengageBlanking();
        ScreenBlankManager::wakeDisplays();
        stopHostTerminal();
        stopHostTunnelProxy();
        whiteboardMgr_.hideHostOverlay();
        whiteboardMgr_.clearAllStrokes();
        displayManager_.restoreResolution();
        InputInjector::releaseAllModifiers();
        fileManager_.abortActiveTransfers();
        hostEncrypted_.store(false);
        {
            std::lock_guard<std::mutex> lock(hostCipherMutex_);
            hostCipher_.reset();
            CryptoUtils::secureZero(hostSessionKey_.data(), hostSessionKey_.size());
        }
        uintptr_t s = activeHostClientSock_.exchange(~uintptr_t(0));
        if (s != ~uintptr_t(0)) {
            closeWinSock(s);
        } else {
            closeWinSock(clientSock);
        }
        {
            std::lock_guard<std::mutex> lock(approvalMutex_);
            pendingReq_.active = false;
        }
        {
            std::lock_guard<std::mutex> lock(hostStatusMutex_);
            hostStatus_ = HostSessionStatus{};
        }
        {
            std::lock_guard<std::mutex> lk(peerVersionMutex_);
            remotePeerAppVersion_.clear();
        }
        localIsOutdated_.store(false);
        remoteIsOutdated_.store(false);
        VirtualDisplayManager::instance().destroyAllSessionDisplays();
    };

    try {
        // Apply 8-second handshake socket timeout to prevent half-open stalls
        setSocketTimeoutMs(toWinSock(clientSock), 8000);

        // Initialize Ephemeral ECDH (NIST P-256) for forward secrecy
        EcdhKeyExchange hostEcdh;
        hostEcdh.initialize();

        // 1. Receive HELLO
        FrameHeader hdr{};
        std::vector<uint8_t> payload;
        if (!recvFrame(clientSock, hdr, payload) || static_cast<PacketType>(hdr.type) != PacketType::HELLO) {
            cleanup();
            return;
        }

        ByteReader helloReader(payload);
        uint16_t protoVer = helloReader.readU16();
        uint64_t viewerId = helloReader.readU64();
        std::string viewerHostname = helloReader.readString();
        std::vector<uint8_t> viewerPubBlob;
        if (helloReader.hasRemaining(2)) {
            uint16_t pubLen = helloReader.readU16();
            if (helloReader.hasRemaining(pubLen)) {
                viewerPubBlob = helloReader.readBytesVector(pubLen);
            }
        }
        std::string viewerAppVersion;
        if (helloReader.hasRemaining(2)) {
            viewerAppVersion = helloReader.readString();
        }
        {
            std::lock_guard<std::mutex> lk(peerVersionMutex_);
            remotePeerAppVersion_ = viewerAppVersion;
        }
        if (protoVer != PROTOCOL_VERSION) {
            cleanup();
            return;
        }

        // 2. Send AUTH_CHALLENGE (including Host Ephemeral ECDH Public Key)
        std::array<uint8_t, 32> nonce{};
        CryptoUtils::randomBytes(nonce.data(), nonce.size());

        {
            ByteWriter w;
            w.writeU64(identity_.deskId());
            w.writeString(identity_.hostname());
            w.writeBytes(nonce.data(), nonce.size());
            w.writeU8(identity_.unattendedEnabled() ? 1 : 0);
            w.writeU16(static_cast<uint16_t>(hostEcdh.localPublicKey().size()));
            if (!hostEcdh.localPublicKey().empty()) {
                w.writeBytes(hostEcdh.localPublicKey().data(), hostEcdh.localPublicKey().size());
            }
            w.writeString(CPP_DESK_VERSION);
            if (!sendFrame(clientSock, PacketType::AUTH_CHALLENGE, 0, w.buffer().data(), w.buffer().size(), hostSendMutex_)) {
                cleanup();
                return;
            }
        }

        // 3. Receive AUTH_RESPONSE
        if (!recvFrame(clientSock, hdr, payload) || static_cast<PacketType>(hdr.type) != PacketType::AUTH_RESPONSE) {
            cleanup();
            return;
        }

        ByteReader authReader(payload);
        uint8_t hasPassword = authReader.readU8();
        std::array<uint8_t, 32> clientDigest{};
        authReader.readBytes(clientDigest.data(), clientDigest.size());

        bool accepted = false;
        uint8_t grantedPerms = PERM_ALL;
        AuthResultCode resultCode = AuthResultCode::RejectedByUser;
        std::string resultMsg;

        if (isIpRateLimited(clientIp)) {
            accepted = false;
            resultCode = AuthResultCode::RateLimited;
            resultMsg = "Too many failed password attempts. Locked for 60s.";
        } else if (hasPassword == 2) {
            std::string tokenHex = CryptoUtils::toHex(clientDigest.data(), clientDigest.size());
            if (WindowsServiceManager::validateAndConsumeRebootToken(tokenHex, viewerId)) {
                recordAuthResultForIp(clientIp, true);
                accepted = true;
                grantedPerms = PERM_ALL;
                resultCode = AuthResultCode::Accepted;
                resultMsg = "Authenticated via Reboot Resume Token";
            } else {
                recordAuthResultForIp(clientIp, false);
                accepted = false;
                resultCode = AuthResultCode::InvalidPassword;
                resultMsg = "Invalid or expired reboot resume token";
            }
        } else if (hasPassword != 0) {
            if (identity_.verifyChallengeResponse(viewerId, nonce, clientDigest)) {
                // Unattended password verified successfully!
                if (identity_.settings().totpEnabled && identity_.totpManager().isConfigured()) {
                    // Host requires Two-Factor Authentication!
                    bool totpVerified = false;
                    int totpAttempts = 0;
                    const int maxTotpAttempts = 3;

                    // Send initial AUTH_RESULT with TotpRequired
                    {
                        ByteWriter w;
                        w.writeU8(static_cast<uint8_t>(AuthResultCode::TotpRequired));
                        w.writeU8(0);
                        w.writeString("Two-Factor Authentication Required");
                        if (!sendFrame(clientSock, PacketType::AUTH_RESULT, 0, w.buffer().data(), w.buffer().size(), hostSendMutex_)) {
                            cleanup();
                            return;
                        }
                    }

                    // Set 45-second socket timeout for TOTP verification
                    setSocketTimeoutMs(toWinSock(clientSock), 45000);

                    while (totpAttempts < maxTotpAttempts && running_.load()) {
                        if (!recvFrame(clientSock, hdr, payload)) {
                            cleanup();
                            return;
                        }
                        if (static_cast<PacketType>(hdr.type) != PacketType::TOTP_VERIFY) {
                            cleanup();
                            return;
                        }

                        ByteReader totpReader(payload);
                        std::string codeStr = totpReader.readString();
                        uint64_t currentUnixTime = static_cast<uint64_t>(std::time(nullptr));

                        if (identity_.totpManager().verifyCode(codeStr, currentUnixTime, 1)) {
                            totpVerified = true;
                            break;
                        } else {
                            totpAttempts++;
                            if (totpAttempts < maxTotpAttempts) {
                                ByteWriter w;
                                w.writeU8(static_cast<uint8_t>(AuthResultCode::TotpInvalid));
                                w.writeU8(0);
                                w.writeString("Invalid Two-Factor Authentication Code. Please try again.");
                                if (!sendFrame(clientSock, PacketType::AUTH_RESULT, 0, w.buffer().data(), w.buffer().size(), hostSendMutex_)) {
                                    cleanup();
                                    return;
                                }
                            }
                        }
                    }

                    if (totpVerified) {
                        recordAuthResultForIp(clientIp, true);
                        accepted = true;
                        grantedPerms = identity_.settings().defaultPermissions;
                        resultCode = AuthResultCode::Accepted;
                        resultMsg = "Authenticated via Unattended Password and TOTP 2FA";
                    } else {
                        recordAuthResultForIp(clientIp, false);
                        accepted = false;
                        resultCode = AuthResultCode::InvalidPassword;
                        resultMsg = "Two-Factor Authentication failed (exceeded attempts or invalid code)";
                    }
                } else {
                    // Standard unattended access without 2FA
                    recordAuthResultForIp(clientIp, true);
                    accepted = true;
                    grantedPerms = identity_.settings().defaultPermissions;
                    resultCode = AuthResultCode::Accepted;
                    resultMsg = "Authenticated via Encrypted Challenge-Response";
                }
            } else {
                recordAuthResultForIp(clientIp, false);
                accepted = false;
                resultCode = isIpRateLimited(clientIp) ? AuthResultCode::RateLimited : AuthResultCode::InvalidPassword;
                resultMsg = (resultCode == AuthResultCode::RateLimited)
                    ? "Too many failed password attempts. Locked for 60s."
                    : "Invalid Unattended or Session Password";
            }
        } else if (autoAcceptIncoming_.load() || identity_.settings().autoAcceptIncoming) {
            accepted = true;
            grantedPerms = identity_.settings().defaultPermissions;
            resultCode = AuthResultCode::Accepted;
            resultMsg = "Accepted automatically";
        } else {
            // Interactive approval via UI popup modal!
            setSocketTimeoutMs(toWinSock(clientSock), 0);
            sendFrame(clientSock, PacketType::AUTH_WAITING, 0, nullptr, 0, hostSendMutex_);

            {
                std::lock_guard<std::mutex> lock(approvalMutex_);
                pendingReq_.active = true;
                pendingReq_.callerDeskId = viewerId;
                pendingReq_.callerHostname = viewerHostname;
                pendingReq_.callerIp = clientIp;
                pendingReq_.proposedPermissions = identity_.settings().defaultPermissions;
                approvalDecided_ = false;
                approvalAccepted_ = false;
                approvalPermissions_ = identity_.settings().defaultPermissions;
            }

            {
                std::unique_lock<std::mutex> lock(approvalMutex_);
                approvalCv_.wait_for(lock, std::chrono::seconds(45), [&]() {
                    return approvalDecided_ || !running_.load() || activeHostClientSock_.load() == ~uintptr_t(0);
                });
                pendingReq_.active = false;
                if (approvalDecided_ && approvalAccepted_) {
                    accepted = true;
                    grantedPerms = approvalPermissions_;
                    resultCode = AuthResultCode::Accepted;
                    resultMsg = "Accepted by Host user";
                } else {
                    accepted = false;
                    resultCode = AuthResultCode::RejectedByUser;
                    resultMsg = "Connection declined or timed out";
                }
            }
        }

        // 4. Send AUTH_RESULT (plaintext before activating stream cipher)
        {
            ByteWriter w;
            w.writeU8(static_cast<uint8_t>(resultCode));
            w.writeU8(grantedPerms);
            w.writeString(resultMsg);
            if (!sendFrame(clientSock, PacketType::AUTH_RESULT, 0, w.buffer().data(), w.buffer().size(), hostSendMutex_)) {
                cleanup();
                return;
            }
        }

        if (!accepted) {
            shutdown(toWinSock(clientSock), SD_SEND);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            cleanup();
            return;
        }

        // Clear handshake timeout & activate mandatory end-to-end AES-256-GCM AEAD encryption!
        setSocketTimeoutMs(toWinSock(clientSock), 0);
        if (!viewerPubBlob.empty()) {
            hostEcdh.computeSharedSessionKey(
                viewerPubBlob.data(), viewerPubBlob.size(),
                identity_.deskId(), viewerId, nonce, hostSessionKey_);
        } else {
            hostSessionKey_ = CryptoUtils::deriveSessionKey(identity_.deskId(), viewerId, nonce);
        }

        {
            std::lock_guard<std::mutex> lock(hostCipherMutex_);
            hostCipher_.initialize(hostSessionKey_, /*isHost=*/true);
            hostSendSeq_ = 0;
            hostEncrypted_.store(true);
        }
        uint64_t hostRecvSeq = 0;
        std::string sasFingerprint = CryptoUtils::sessionFingerprintHex(hostSessionKey_);

        hostLivePermissions_.store(grantedPerms);
        {
            std::lock_guard<std::mutex> lock(hostStatusMutex_);
            hostStatus_.active = true;
            hostStatus_.viewerDeskId = viewerId;
            hostStatus_.viewerHostname = viewerHostname;
            hostStatus_.viewerIp = clientIp;
            hostStatus_.permissions = grantedPerms;
            hostStatus_.connectedSinceTickMs = nowTickMs();
            hostStatus_.securityFingerprint = sasFingerprint;
            hostStatus_.viewerAppVersion = viewerAppVersion;
        }

        // Auto-provision virtual display if host is headless
        if (identity_.settings().autoVirtualDisplay && VirtualDisplayManager::isHostHeadless()) {
            VirtualDisplayManager::instance().ensureHeadlessDisplay(1920, 1080, 60);
        }

        // Register emergency wake callback for local physical interruptions
        ScreenBlankManager::instance().setEmergencyWakeCallback([this]() {
            hostPrivacyModeActive_.store(false);
            PrivacyModeConfigPayload ack{};
            ack.enable = 0;
            ack.acknowledge = 1;
            ack.blankMode = static_cast<uint8_t>(ScreenBlankManager::instance().currentBlankMode());
            ByteWriter w;
            w.writeBytes(&ack, sizeof(ack));
            sendHostEncryptedPacket(PacketType::PRIVACY_MODE_TOGGLE, 0, w.buffer().data(), w.buffer().size());
        });

        // 5. Initialize ScreenCapturer & send encrypted VIDEO_CONFIG
        ScreenCapturer capturer;
        capturer.setAutoHeadless(identity_.settings().autoVirtualDisplay);
        auto monitors = capturer.enumerateMonitors();
        capturer.selectMonitor(0);

        auto sendVideoConfig = [&]() -> bool {
            MonitorDesc cur = capturer.currentMonitor();
            ByteWriter w;
            w.writeI32(cur.index);
            w.writeI32(cur.width);
            w.writeI32(cur.height);
            w.writeU16(static_cast<uint16_t>(monitors.size()));
            for (const auto& m : monitors) {
                w.writeI32(m.index);
                w.writeI32(m.x);
                w.writeI32(m.y);
                w.writeI32(m.width);
                w.writeI32(m.height);
                w.writeU8(m.isPrimary ? 1 : 0);
                w.writeString(m.name);
                w.writeU8(m.isVirtual ? 1 : 0);
                w.writeU32(m.virtualId);
            }
            return sendHostEncryptedPacket(PacketType::VIDEO_CONFIG, 0, w.buffer().data(), w.buffer().size());
        };

        auto broadcastMonitorList = [&]() -> bool {
            monitors = capturer.enumerateMonitors();
            std::vector<uint8_t> monPayload;
            serializeMonitorList(monitors, monPayload);
            return sendHostEncryptedPacket(PacketType::MONITOR_LIST, 0, monPayload.data(), monPayload.size());
        };

        if (!sendVideoConfig()) {
            cleanup();
            return;
        }

        startHostAudioCapture();
        if (grantedPerms & PERM_INPUT) {
            startHostTerminal(false);
            startHostTunnelProxy();
        }

        std::atomic<bool> sessionAlive{true};
        std::atomic<uint8_t> requestedQuality{static_cast<uint8_t>(identity_.settings().defaultQuality)};
        std::atomic<uint8_t> requestedTargetFps{clampTargetFps(identity_.settings().targetFps)};
        std::atomic<bool> adaptiveFpsEnabled{identity_.settings().adaptiveFps};
        std::atomic<uint32_t> hostMeasuredRttMs{0};
        std::atomic<int> requestedMonitor{0};
        std::atomic<bool> forceKeyframeFlag{true};
        std::atomic<float> avgSendMs{4.0f};
        MonitorDesc currentMonDesc = capturer.currentMonitor();
        std::mutex monDescMutex;

        struct HostOutboxItem {
            PacketType type;
            uint8_t flags;
            std::vector<uint8_t> payload;
            bool isVideoTile = false;
        };

        std::mutex outboxMutex;
        std::condition_variable outboxCv;
        std::deque<HostOutboxItem> outbox;
        std::vector<std::vector<uint8_t>> outboxBufferPool; // Recycled payload buffers
        uint64_t totalBytesSent = 0;

        auto enqueueHostPacketMove = [&](PacketType pt, uint8_t flags, std::vector<uint8_t>&& payload, bool isVideo) {
            std::lock_guard<std::mutex> lk(outboxMutex);
            if (isVideo) {
                for (auto it = outbox.begin(); it != outbox.end(); ++it) {
                    if (it->isVideoTile) {
                        outbox.erase(it);
                        break;
                    }
                }
            }
            outbox.push_back({ pt, flags, std::move(payload), isVideo });
            outboxCv.notify_one();
        };

        auto enqueueHostPacketRaw = [&](PacketType pt, uint8_t flags, const void* data, size_t size, bool isVideo) {
            std::vector<uint8_t> payload;
            {
                std::lock_guard<std::mutex> lk(outboxMutex);
                if (!outboxBufferPool.empty()) {
                    payload = std::move(outboxBufferPool.back());
                    outboxBufferPool.pop_back();
                }
            }
            payload.resize(size);
            if (size > 0 && data) {
                std::memcpy(payload.data(), data, size);
            }
            enqueueHostPacketMove(pt, flags, std::move(payload), isVideo);
        };

        auto enqueueHostPacket = [&](PacketType pt, uint8_t flags, const std::vector<uint8_t>& payload, bool isVideo) {
            enqueueHostPacketRaw(pt, flags, payload.data(), payload.size(), isVideo);
        };

        auto sendPacketHelper = [&](PacketType pt, const std::vector<uint8_t>& buf) -> bool {
            enqueueHostPacket(pt, 0, buf, false);
            return true;
        };

        // Stage 3: Dedicated Network Send Worker Thread (Option 3A)
        std::thread sendWorkerThread([&]() {
            while (sessionAlive.load() && running_.load()) {
                HostOutboxItem item;
                {
                    std::unique_lock<std::mutex> lock(outboxMutex);
                    outboxCv.wait(lock, [&]() {
                        return !sessionAlive.load() || !running_.load() || !outbox.empty();
                    });
                    if (!sessionAlive.load() || !running_.load()) break;
                    item = std::move(outbox.front());
                    outbox.pop_front();
                }

                uint64_t sendT0 = nowTickMs();
                if (!sendHostEncryptedPacket(item.type, item.flags, item.payload.data(), item.payload.size())) {
                    sessionAlive.store(false);
                    break;
                }

                totalBytesSent += item.payload.size();
                // Periodic rekeying ratchet every 1 GB (Option 4A)
                if (totalBytesSent >= 1024ULL * 1024ULL * 1024ULL) {
                    totalBytesSent = 0;
                    std::lock_guard<std::mutex> lock(hostCipherMutex_);
                    hostCipher_.ratchetKey();
                }

                if (item.isVideoTile) {
                    float sendDur = static_cast<float>(nowTickMs() - sendT0);
                    float cur = avgSendMs.load();
                    avgSendMs.store(cur * 0.78f + sendDur * 0.22f);
                }

                // Recycle payload buffer back to outboxBufferPool to avoid heap allocator thrashing
                if (item.payload.capacity() >= 256) {
                    item.payload.clear();
                    std::lock_guard<std::mutex> lock(outboxMutex);
                    if (outboxBufferPool.size() < 4) {
                        outboxBufferPool.push_back(std::move(item.payload));
                    }
                }
            }
        });

        // Spawn reader thread for low-latency encrypted input/control handling on Host
        std::thread readerThread([&]() {
            FrameHeader rhdr{};
            std::vector<uint8_t> rpay;
            while (sessionAlive.load() && running_.load()) {
                if (!recvFrame(clientSock, rhdr, rpay, &hostCipher_, &hostRecvSeq)) {
                    sessionAlive.store(false);
                    break;
                }

                PacketType rpt = static_cast<PacketType>(rhdr.type);
                uint8_t perms = hostLivePermissions_.load();

                try {
                    ByteReader r(rpay);
                    switch (rpt) {
                        case PacketType::PING: {
                            uint64_t ts = r.readU64();
                            if (r.hasRemaining(4)) {
                                hostMeasuredRttMs.store(r.readU32());
                            }
                            sendHostEncryptedPacket(PacketType::PONG, 0, &ts, sizeof(ts));
                            break;
                        }
                        case PacketType::INPUT_MOUSE_MOVE: {
                            if (perms & PERM_INPUT) {
                                float nx = r.readF32();
                                float ny = r.readF32();
                                MonitorDesc md;
                                {
                                    std::lock_guard<std::mutex> ml(monDescMutex);
                                    md = currentMonDesc;
                                }
                                InputInjector::injectMouseMove(nx, ny, md);
                            }
                            break;
                        }
                        case PacketType::INPUT_MOUSE_BUTTON: {
                            if (perms & PERM_INPUT) {
                                uint8_t btn = r.readU8();
                                uint8_t isDown = r.readU8();
                                float nx = r.readF32();
                                float ny = r.readF32();
                                MonitorDesc md;
                                {
                                    std::lock_guard<std::mutex> ml(monDescMutex);
                                    md = currentMonDesc;
                                }
                                InputInjector::injectMouseButton(static_cast<MouseButtonId>(btn), isDown != 0, nx, ny, md);
                            }
                            break;
                        }
                        case PacketType::INPUT_MOUSE_WHEEL: {
                            if (perms & PERM_INPUT) {
                                int32_t dy = r.readI32();
                                int32_t dx = r.readI32();
                                InputInjector::injectMouseWheel(dy, dx);
                            }
                            break;
                        }
                        case PacketType::INPUT_KEY_EVENT: {
                            if (perms & PERM_INPUT) {
                                uint16_t vk = r.readU16();
                                uint16_t sc = r.readU16();
                                uint8_t isDown = r.readU8();
                                uint8_t isExt = r.readU8();
                                InputInjector::injectKeyEvent(vk, sc, isDown != 0, isExt != 0);
                            }
                            break;
                        }
                        case PacketType::INPUT_RELEASE_ALL: {
                            InputInjector::releaseAllModifiers();
                            break;
                        }
                        case PacketType::SYSTEM_ACTION: {
                            if (perms & PERM_INPUT) {
                                uint8_t act = r.readU8();
                                executeRemoteSystemAction(static_cast<SystemActionType>(act));
                            }
                            break;
                        }
                        case PacketType::REMOTE_REBOOT_REQUEST: {
                            RemoteRebootRequestPayload req{};
                            if ((perms & PERM_INPUT) && deserializeRemoteRebootRequest(r.currentPtr(), r.remaining(), req)) {
                                std::string token = WindowsServiceManager::generateRebootTokenHex();
                                WindowsServiceManager::saveRebootToken(token, viewerId, 600);

                                RemoteRebootConfirmPayload confirm{};
                                confirm.accepted = true;
                                confirm.countdownSeconds = req.countdownSeconds;
                                confirm.resumeTokenHex = token;
                                confirm.message = "Host reboot scheduled";

                                std::vector<uint8_t> cBuf;
                                serializeRemoteRebootConfirm(confirm, cBuf);
                                sendHostEncryptedPacket(PacketType::REMOTE_REBOOT_CONFIRM, 0, cBuf.data(), cBuf.size());

                                WindowsServiceManager::initiateHostReboot(req.countdownSeconds, req.rebootMode != 0);
                            } else {
                                RemoteRebootConfirmPayload failConfirm{};
                                failConfirm.accepted = false;
                                failConfirm.message = "Permission denied or invalid payload";
                                std::vector<uint8_t> cBuf;
                                serializeRemoteRebootConfirm(failConfirm, cBuf);
                                sendHostEncryptedPacket(PacketType::REMOTE_REBOOT_CONFIRM, 0, cBuf.data(), cBuf.size());
                            }
                            break;
                        }
                        case PacketType::VIRTUAL_DISPLAY_CMD: {
                            VirtualDisplayCmdPayload cmd{};
                            if (deserializeVirtualDisplayCmd(payload.data(), payload.size(), cmd)) {
                                VirtualDisplayStatusPayload status{};
                                status.displayId = cmd.displayId;
                                status.width = cmd.width;
                                status.height = cmd.height;
                                status.refreshRate = cmd.refreshRate;
                                status.isVirtual = 1;
                                status.isHeadlessFallback = 0;
                                status.driverBackend = 0;

                                if (cmd.cmd == static_cast<uint8_t>(VirtualDisplayCmdType::Create)) {
                                    uint32_t newId = 0;
                                    bool ok = VirtualDisplayManager::instance().createVirtualDisplay(
                                        cmd.width, cmd.height, cmd.refreshRate,
                                        (cmd.flags & 0x01) != 0,
                                        &newId
                                    );
                                    if (ok) {
                                        status.statusCode = static_cast<uint8_t>(VirtualDisplayStatusCode::Success);
                                        status.displayId = newId;
                                        VirtualDisplayInfo vinfo;
                                        if (VirtualDisplayManager::instance().getDisplayInfo(newId, vinfo)) {
                                            status.driverBackend = static_cast<uint8_t>(vinfo.backend);
                                            status.width = vinfo.width;
                                            status.height = vinfo.height;
                                            status.refreshRate = vinfo.refreshRate;
                                        }
                                        status.message = "Virtual display created successfully";
                                    } else {
                                        status.statusCode = static_cast<uint8_t>(VirtualDisplayStatusCode::FailedGeneric);
                                        status.message = "Failed to create virtual display surface";
                                    }
                                } else if (cmd.cmd == static_cast<uint8_t>(VirtualDisplayCmdType::Destroy)) {
                                    bool ok = VirtualDisplayManager::instance().destroyVirtualDisplay(cmd.displayId);
                                    if (ok) {
                                        status.statusCode = static_cast<uint8_t>(VirtualDisplayStatusCode::Success);
                                        status.message = "Virtual display destroyed";
                                        if (capturer.currentMonitor().virtualId == cmd.displayId) {
                                            requestedMonitor.store(0);
                                            forceKeyframeFlag.store(true);
                                        }
                                    } else {
                                        status.statusCode = static_cast<uint8_t>(VirtualDisplayStatusCode::NotFound);
                                        status.message = "Virtual display not found";
                                    }
                                } else if (cmd.cmd == static_cast<uint8_t>(VirtualDisplayCmdType::SetMode)) {
                                    bool ok = VirtualDisplayManager::instance().setVirtualDisplayMode(
                                        cmd.displayId, cmd.width, cmd.height, cmd.refreshRate
                                    );
                                    if (ok) {
                                        status.statusCode = static_cast<uint8_t>(VirtualDisplayStatusCode::Success);
                                        status.message = "Virtual display mode updated";
                                        if (capturer.currentMonitor().virtualId == cmd.displayId) {
                                            forceKeyframeFlag.store(true);
                                        }
                                    } else {
                                        status.statusCode = static_cast<uint8_t>(VirtualDisplayStatusCode::NotFound);
                                        status.message = "Virtual display not found";
                                    }
                                }

                                std::vector<uint8_t> stBytes;
                                serializeVirtualDisplayStatus(status, stBytes);
                                sendHostEncryptedPacket(PacketType::VIRTUAL_DISPLAY_STATUS, 0, stBytes.data(), stBytes.size());
                                broadcastMonitorList();
                            }
                            break;
                        }
                        case PacketType::MONITOR_SELECT: {
                            int32_t monIdx = r.readI32();
                            if (monIdx >= -1) {
                                requestedMonitor.store(monIdx);
                                forceKeyframeFlag.store(true);
                            }
                            break;
                        }
                        case PacketType::QUALITY_UPDATE: {
                            uint8_t q = r.readU8();
                            uint8_t fps = r.readU8();
                            uint8_t adap = r.readU8();
                            requestedQuality.store(q);
                            if (fps > 0) requestedTargetFps.store(clampTargetFps(fps));
                            adaptiveFpsEnabled.store(adap != 0);
                            forceKeyframeFlag.store(true);
                            break;
                        }
                        case PacketType::VIDEO_CONTROL_REQ: {
                            uint8_t q = r.readU8();
                            int32_t monIdx = r.readI32();
                            uint8_t reqKf = r.readU8();
                            requestedQuality.store(q);
                            if (monIdx >= -1) requestedMonitor.store(monIdx);
                            if (reqKf != 0) forceKeyframeFlag.store(true);
                            if (r.hasRemaining(1)) {
                                uint8_t fpsReq = r.readU8();
                                if (fpsReq > 0) requestedTargetFps.store(clampTargetFps(fpsReq));
                            }
                            if (r.hasRemaining(1)) {
                                uint8_t adapReq = r.readU8();
                                adaptiveFpsEnabled.store(adapReq != 0);
                            }
                            break;
                        }
                        case PacketType::CLIPBOARD_TEXT: {
                            if (perms & PERM_CLIPBOARD) {
                                std::string txt = r.readString();
                                if (clipboardSyncEnabled_.load()) {
                                    clipboardManager_.applyRemoteClipboard(txt);
                                }
                            }
                            break;
                        }
                        case PacketType::CLIPBOARD_FILE_LIST: {
                            if (perms & PERM_CLIPBOARD) {
                                uint32_t tid = 0;
                                std::vector<VirtualFileEntry> files;
                                if (deserializeClipboardFileList(r.currentPtr(), r.remaining(), tid, files)) {
                                    clipFileMgr_.handleRemoteFileList(tid, files, [this, &enqueueHostPacket](PacketType t, const std::vector<uint8_t>& p) {
                                        enqueueHostPacket(t, 0, p, false);
                                        return true;
                                    });
                                }
                            }
                            break;
                        }
                        case PacketType::CLIPBOARD_FILE_REQUEST: {
                            if (perms & PERM_CLIPBOARD) {
                                if (r.hasRemaining(sizeof(ClipboardFileRequestHeader))) {
                                    ClipboardFileRequestHeader reqHdr{};
                                    r.readBytes(&reqHdr, sizeof(reqHdr));
                                    clipFileMgr_.handleFileRequest(reqHdr.transferId, reqHdr.fileIndex, reqHdr.offset, reqHdr.length,
                                        [this, &enqueueHostPacket](PacketType t, const std::vector<uint8_t>& p) {
                                            enqueueHostPacket(t, 0, p, false);
                                            return true;
                                        });
                                }
                            }
                            break;
                        }
                        case PacketType::CLIPBOARD_FILE_CHUNK: {
                            if (perms & PERM_CLIPBOARD) {
                                if (r.hasRemaining(sizeof(ClipboardFileChunkHeader))) {
                                    ClipboardFileChunkHeader chkHdr{};
                                    r.readBytes(&chkHdr, sizeof(chkHdr));
                                    if (r.hasRemaining(chkHdr.dataLength)) {
                                        clipFileMgr_.handleFileChunk(chkHdr.transferId, chkHdr.fileIndex, chkHdr.offset,
                                            r.currentPtr(), chkHdr.dataLength, chkHdr.sha256);
                                    }
                                }
                            }
                            break;
                        }
                        case PacketType::CLIPBOARD_FILE_CANCEL: {
                            if (r.hasRemaining(sizeof(ClipboardFileCancelHeader))) {
                                ClipboardFileCancelHeader cnlHdr{};
                                r.readBytes(&cnlHdr, sizeof(cnlHdr));
                                clipFileMgr_.handleFileCancel(cnlHdr.transferId, cnlHdr.reasonCode);
                            }
                            break;
                        }
                        case PacketType::FILE_OFFER: {
                            if (perms & PERM_FILE_TRANSFER) {
                                uint32_t tid = r.readU32();
                                uint64_t fsz = r.readU64();
                                std::string fname = r.readString();
                                FileOfferTarget targetHint = FileOfferTarget::DefaultDownloads;
                                float dropNx = 0.0f, dropNy = 0.0f;
                                if (r.hasRemaining(1)) {
                                    targetHint = static_cast<FileOfferTarget>(r.readU8());
                                }
                                if (r.hasRemaining(8)) {
                                    dropNx = r.readF32();
                                    dropNy = r.readF32();
                                }
                                fileManager_.handleFileOffer(tid, fsz, fname, targetHint, dropNx, dropNy);
                            }
                            break;
                        }
                        case PacketType::FILE_CHUNK: {
                            if (perms & PERM_FILE_TRANSFER) {
                                uint32_t tid = r.readU32();
                                uint64_t off = r.readU64();
                                uint32_t clen = r.readU32();
                                if (r.hasRemaining(clen)) {
                                    fileManager_.handleFileChunk(tid, off, r.currentPtr(), clen);
                                }
                            }
                            break;
                        }
                        case PacketType::FILE_COMPLETE: {
                            if (perms & PERM_FILE_TRANSFER) {
                                uint32_t tid = r.readU32();
                                std::string sha = r.readString();
                                fileManager_.handleFileComplete(tid, sha);
                            }
                            break;
                        }
                        case PacketType::FILE_CANCEL: {
                            uint32_t tid = r.readU32();
                            fileManager_.handleFileCancel(tid);
                            break;
                        }
                        case PacketType::CHAT_MESSAGE: {
                            std::string sender = r.readString();
                            std::string msg = r.readString();
                            if (!msg.empty()) {
                                {
                                    std::lock_guard<std::mutex> cl(chatMutex_);
                                    chatHistory_.push_back({ sender, msg, false, nowTickMs() });
                                    if (chatHistory_.size() > 100) chatHistory_.erase(chatHistory_.begin());
                                }
                                unreadChatCount_.fetch_add(1);
                            }
                            break;
                        }
                        case PacketType::CHAT_MEDIA_MESSAGE: {
                            ChatMediaPayload p{};
                            if (deserializeChatMedia(payload.data(), payload.size(), p)) {
                                {
                                    std::lock_guard<std::mutex> cl(chatMutex_);
                                    chatHistory_.push_back({ p.senderName, p.captionText, false, p.timestampMs, true, std::move(p.jpegData), p.imgWidth, p.imgHeight });
                                    if (chatHistory_.size() > 100) chatHistory_.erase(chatHistory_.begin());
                                }
                                unreadChatCount_.fetch_add(1);
                            }
                            break;
                        }
                        case PacketType::PRIVACY_MODE_TOGGLE: {
                            if (perms & PERM_INPUT) {
                                if (r.hasRemaining(sizeof(PrivacyModeConfigPayload))) {
                                    PrivacyModeConfigPayload cp{};
                                    r.readBytes(&cp, sizeof(cp));
                                    cp.customNotice[sizeof(cp.customNotice) - 1] = '\0';
                                    cp.brandName[sizeof(cp.brandName) - 1] = '\0';
                                    ScreenBlankMode bm = static_cast<ScreenBlankMode>(cp.blankMode);
                                    setHostPrivacyMode(cp.enable != 0, cp.customNotice, cp.brandName, cp.showDeskId != 0, bm);

                                    PrivacyModeConfigPayload ack{};
                                    ack.enable = isHostPrivacyModeActive() ? 1 : 0;
                                    ack.acknowledge = 1;
                                    ack.showDeskId = cp.showDeskId;
                                    ack.blankMode = static_cast<uint8_t>(ScreenBlankManager::instance().currentBlankMode());
                                    std::snprintf(ack.customNotice, sizeof(ack.customNotice), "%s", cp.customNotice);
                                    std::snprintf(ack.brandName, sizeof(ack.brandName), "%s", cp.brandName);
                                    ByteWriter w;
                                    w.writeBytes(&ack, sizeof(ack));
                                    sendHostEncryptedPacket(PacketType::PRIVACY_MODE_TOGGLE, 0, w.buffer().data(), w.buffer().size());
                                } else if (r.hasRemaining(sizeof(PrivacyModePayload))) {
                                    PrivacyModePayload p{};
                                    r.readBytes(&p, sizeof(p));
                                    setHostPrivacyMode(p.enable != 0);

                                    PrivacyModePayload ack{};
                                    ack.enable = isHostPrivacyModeActive() ? 1 : 0;
                                    ack.acknowledge = 1;
                                    ByteWriter w;
                                    w.writeBytes(&ack, sizeof(ack));
                                    sendHostEncryptedPacket(PacketType::PRIVACY_MODE_TOGGLE, 0, w.buffer().data(), w.buffer().size());
                                }
                            }
                            break;
                        }
                        case PacketType::AUDIO_CONTROL: {
                            if (r.hasRemaining(sizeof(AudioControlPayload))) {
                                AudioControlPayload p{};
                                r.readBytes(&p, sizeof(p));
                                hostAudioSuspended_.store(p.isMuted != 0);
                            }
                            break;
                        }
                        case PacketType::VOICE_INTERCOM_CHUNK: {
                            handleIncomingVoiceChunk(payload.data(), payload.size());
                            break;
                        }
                        case PacketType::TUNNEL_OPEN: {
                            if (r.hasRemaining(sizeof(TunnelOpenHeader))) {
                                TunnelOpenHeader toh{};
                                r.readBytes(&toh, sizeof(toh));

                                bool permitted = (perms & PERM_INPUT) != 0;
                                SOCKET ts = INVALID_SOCKET;
                                bool ok = false;
                                if (permitted) {
                                    ts = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                                    if (ts != INVALID_SOCKET) {
                                        sockaddr_in targetAddr{};
                                        targetAddr.sin_family = AF_INET;
                                        targetAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                                        targetAddr.sin_port = htons(toh.targetPort);
                                        if (connect(ts, reinterpret_cast<sockaddr*>(&targetAddr), sizeof(targetAddr)) == 0) {
                                            u_long nonblock = 1;
                                            ioctlsocket(ts, FIONBIO, &nonblock);
                                            ok = true;
                                            std::lock_guard<std::mutex> lk(tunnelMutex_);
                                            hostTunnels_[toh.tunnelId] = { toh.tunnelId, 0, ts, toh.targetPort };
                                        } else {
                                            closesocket(ts);
                                        }
                                    }
                                }
                                if (!ok) {
                                    TunnelCloseHeader tch{};
                                    tch.tunnelId = toh.tunnelId;
                                    tch.reasonCode = permitted ? 1 : 2; // Refused / Forbidden
                                    ByteWriter w;
                                    w.writeBytes(&tch, sizeof(tch));
                                    sendHostEncryptedPacket(PacketType::TUNNEL_CLOSE, 0, w.buffer().data(), w.buffer().size());
                                }
                            }
                            break;
                        }
                        case PacketType::TUNNEL_DATA: {
                            if (perms & PERM_INPUT) {
                                if (r.hasRemaining(sizeof(TunnelDataHeader))) {
                                    TunnelDataHeader tdh{};
                                    r.readBytes(&tdh, sizeof(tdh));
                                    if (r.hasRemaining(tdh.dataLen)) {
                                        SOCKET ts = INVALID_SOCKET;
                                        {
                                            std::lock_guard<std::mutex> lk(tunnelMutex_);
                                            auto it = hostTunnels_.find(tdh.tunnelId);
                                            if (it != hostTunnels_.end()) {
                                                ts = it->second.sock;
                                            }
                                        }
                                        if (ts != INVALID_SOCKET) {
                                            sendAllBytes(ts, r.currentPtr(), tdh.dataLen);
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case PacketType::TUNNEL_CLOSE: {
                            if (r.hasRemaining(sizeof(TunnelCloseHeader))) {
                                TunnelCloseHeader tch{};
                                r.readBytes(&tch, sizeof(tch));
                                std::lock_guard<std::mutex> lk(tunnelMutex_);
                                auto it = hostTunnels_.find(tch.tunnelId);
                                if (it != hostTunnels_.end()) {
                                    closesocket(it->second.sock);
                                    hostTunnels_.erase(it);
                                }
                            }
                            break;
                        }
                        case PacketType::TERMINAL_DATA: {
                            if (perms & PERM_INPUT) {
                                if (r.hasRemaining(sizeof(TerminalDataHeader))) {
                                    TerminalDataHeader tdh{};
                                    r.readBytes(&tdh, sizeof(tdh));
                                    if (tdh.streamKind == static_cast<uint8_t>(TerminalStreamKind::StdinInput)) {
                                        if (r.hasRemaining(tdh.textLen)) {
                                            std::string input(reinterpret_cast<const char*>(r.currentPtr()), tdh.textLen);
                                            injectHostTerminalStdin(input);
                                        }
                                    } else if (tdh.streamKind == static_cast<uint8_t>(TerminalStreamKind::ResetShell)) {
                                        startHostTerminal(false);
                                    } else if (tdh.streamKind == static_cast<uint8_t>(TerminalStreamKind::SwitchShell)) {
                                        startHostTerminal(true);
                                    }
                                }
                            }
                            break;
                        }
                        case PacketType::WHITEBOARD_PACKET: {
                            AnnotationStroke stroke{};
                            if (WhiteboardManager::deserializeStroke(r.currentPtr(), r.remaining(), stroke)) {
                                whiteboardMgr_.showHostOverlay();
                                whiteboardMgr_.applyRemoteStroke(stroke);
                            }
                            break;
                        }
                        case PacketType::DIAGNOSTICS_REQ: {
                            uint8_t act = r.readU8();
                            hostDiagnosticsStreamActive_.store(act != 0);
                            if (act != 0) {
                                auto diag = sampleHostDiagnostics();
                                std::vector<uint8_t> payload;
                                serializeSystemDiagnostics(diag, payload);
                                sendHostEncryptedPacket(PacketType::SYSTEM_DIAGNOSTICS, FLAG_ENCRYPTED, payload.data(), payload.size());
                            }
                            break;
                        }
                        case PacketType::PROCESS_KILL: {
                            uint32_t pid = r.readU32();
                            if (executeProcessKill(pid, perms)) {
                                auto diag = sampleHostDiagnostics();
                                std::vector<uint8_t> payload;
                                serializeSystemDiagnostics(diag, payload);
                                sendHostEncryptedPacket(PacketType::SYSTEM_DIAGNOSTICS, FLAG_ENCRYPTED, payload.data(), payload.size());
                            }
                            break;
                        }
                        case PacketType::RESOLUTION_CHANGE_REQ: {
                            handleIncomingResolutionChangeReq(r.currentPtr(), r.remaining(), perms);
                            break;
                        }
                        case PacketType::SYNC_SCAN_REQ: {
                            if (perms & PERM_FILE_TRANSFER) {
                                SyncScanReqPayload req;
                                if (deserializeSyncScanReq(payload.data(), payload.size(), req)) {
                                    SyncScanRespPayload resp;
                                    fileSyncMgr_.handleScanReqHost(req, resp);
                                    std::vector<uint8_t> respBuf;
                                    serializeSyncScanResp(resp, respBuf);
                                    sendPacketHelper(PacketType::SYNC_SCAN_RESP, respBuf);
                                }
                            }
                            break;
                        }
                        case PacketType::SYNC_HASH_REQ: {
                            if (perms & PERM_FILE_TRANSFER) {
                                SyncHashReqPayload req;
                                if (deserializeSyncHashReq(payload.data(), payload.size(), req)) {
                                    SyncHashRespPayload resp;
                                    fileSyncMgr_.handleHashReqHost(req, resp);
                                    std::vector<uint8_t> respBuf;
                                    serializeSyncHashResp(resp, respBuf);
                                    sendPacketHelper(PacketType::SYNC_HASH_RESP, respBuf);
                                }
                            }
                            break;
                        }
                        case PacketType::SYNC_DELTA_BLOCK: {
                            if (perms & PERM_FILE_TRANSFER) {
                                SyncDeltaBlockPayload block;
                                if (deserializeSyncDeltaBlock(payload.data(), payload.size(), block)) {
                                    fileSyncMgr_.handleDeltaBlock(block);
                                }
                            }
                            break;
                        }
                        case PacketType::SYNC_ACTION_REQ: {
                            if (perms & PERM_FILE_TRANSFER) {
                                SyncActionReqPayload req;
                                if (deserializeSyncActionReq(payload.data(), payload.size(), req)) {
                                    SyncActionRespPayload resp;
                                    fileSyncMgr_.handleActionReq(req, resp);
                                    std::vector<uint8_t> respBuf;
                                    serializeSyncActionResp(resp, respBuf);
                                    sendPacketHelper(PacketType::SYNC_ACTION_RESP, respBuf);
                                }
                            }
                            break;
                        }
                        case PacketType::SYNC_STATUS_UPDATE: {
                            SyncStatusUpdatePayload status;
                            if (deserializeSyncStatusUpdate(payload.data(), payload.size(), status)) {
                                fileSyncMgr_.handleStatusUpdate(status);
                            }
                            break;
                        }
                        case PacketType::VERSION_UPGRADE_NOTICE: {
                            VersionUpgradeNoticePayload notice;
                            if (deserializeVersionUpgradeNotice(payload.data(), payload.size(), notice)) {
                                std::function<void(const VersionUpgradeNoticePayload&)> cb;
                                {
                                    std::lock_guard<std::mutex> lk(peerVersionMutex_);
                                    cb = onVersionUpgradeNotice_;
                                }
                                if (cb) {
                                    cb(notice);
                                }
                            }
                            break;
                        }
                        case PacketType::SCRATCHPAD_UPDATE: {
                            ScratchpadUpdatePayload pad{};
                            if (deserializeScratchpadUpdate(payload.data(), payload.size(), pad)) {
                                if (scratchpadMgr_.handleRemoteUpdate(pad)) {
                                    std::function<void(const ScratchpadUpdatePayload&)> cb;
                                    {
                                        std::lock_guard<std::mutex> lk(scratchpadCbMutex_);
                                        cb = onScratchpadUpdate_;
                                    }
                                    if (cb) cb(pad);
                                }
                            }
                            break;
                        }
                        case PacketType::COMMAND_PALETTE_EXEC: {
                            if (perms & PERM_INPUT) {
                                CommandPaletteExecPayload execReq{};
                                if (deserializeCommandPaletteExec(payload.data(), payload.size(), execReq)) {
                                    uint32_t cmdId = execReq.commandId;
                                    std::string runCmd = execReq.customCommand.empty() ? execReq.commandKey : execReq.customCommand;
                                    commandPaletteMgr_.executeAsync(runCmd, [this, cmdId](int32_t exitCode, const std::string& output) {
                                        CommandPaletteOutputPayload outPayload{};
                                        outPayload.commandId = cmdId;
                                        outPayload.exitCode = exitCode;
                                        outPayload.isCompleted = true;
                                        outPayload.stdoutData = output;
                                        std::vector<uint8_t> outBuf;
                                        serializeCommandPaletteOutput(outPayload, outBuf);
                                        sendHostEncryptedPacket(PacketType::COMMAND_PALETTE_OUTPUT, FLAG_ENCRYPTED, outBuf.data(), outBuf.size());
                                    });
                                }
                            }
                            break;
                        }
                        case PacketType::DISCONNECT:
                            sessionAlive.store(false);
                            break;
                        default:
                            break;
                    }
                } catch (...) {}
            }
        });

        struct ScopedThreadJoiner {
            std::thread& t;
            ~ScopedThreadJoiner() {
                if (t.joinable()) {
                    try { t.join(); } catch (...) {}
                }
            }
        };
        ScopedThreadJoiner joinSend(sendWorkerThread);
        ScopedThreadJoiner joinReader(readerThread);

        // Main capture & streaming loop with 15 / 30 / 60 FPS pacing + Automatic Network Congestion FPS Drop
        uint64_t lastClipboardCheck = 0;
        uint64_t lastDiagnosticsSend = 0;
        uint64_t lastHudSend = 0;
        CursorState prevCursor{};
        std::vector<EncodedTile> dirtyTiles;
        dirtyTiles.reserve(64);

        while (sessionAlive.load() && running_.load() && activeHostClientSock_.load() != ~uintptr_t(0)) {
            uint64_t frameStart = nowTickMs();

            if (hostDiagnosticsStreamActive_.load()) {
                if (frameStart - lastDiagnosticsSend >= 1500) {
                    lastDiagnosticsSend = frameStart;
                    auto diag = sampleHostDiagnostics();
                    std::vector<uint8_t> payload;
                    serializeSystemDiagnostics(diag, payload);
                    sendHostEncryptedPacket(PacketType::SYSTEM_DIAGNOSTICS, FLAG_ENCRYPTED, payload.data(), payload.size());
                }
            }

            int targetMon = requestedMonitor.load();
            if (targetMon != capturer.currentMonitorIndex()) {
                capturer.selectMonitor(targetMon);
                {
                    std::lock_guard<std::mutex> ml(monDescMutex);
                    currentMonDesc = capturer.currentMonitor();
                }
                sendVideoConfig();
                forceKeyframeFlag.store(true);
            }

            uint8_t userFps = requestedTargetFps.load();
            bool adaptiveOn = adaptiveFpsEnabled.load();
            uint8_t effectiveFps = computeAdaptiveFpsCap(userFps, adaptiveOn, hostMeasuredRttMs.load(), avgSendMs.load());

            QualityPreset preset = static_cast<QualityPreset>(requestedQuality.load());
            if (adaptiveOn && effectiveFps == 15 && userFps > 15 && preset == QualityPreset::Ultra) {
                preset = QualityPreset::Balanced;
            }
            bool wantKf = forceKeyframeFlag.exchange(false);

            dirtyTiles.clear();
            bool isKf = false;
            CursorState curState{};

            if (capturer.captureDirtyTiles(wantKf, preset, dirtyTiles, isKf, curState)) {
                // Detect dynamic desktop resolution change mid-session
                if (capturer.frameWidth() != currentMonDesc.width || capturer.frameHeight() != currentMonDesc.height) {
                    {
                        std::lock_guard<std::mutex> ml(monDescMutex);
                        currentMonDesc = capturer.currentMonitor();
                        currentMonDesc.width = capturer.frameWidth();
                        currentMonDesc.height = capturer.frameHeight();
                    }
                    sendVideoConfig();
                }

                if (!dirtyTiles.empty()) {
                    size_t estCap = 6;
                    for (const auto& t : dirtyTiles) {
                        estCap += sizeof(TileHeader) + t.data.size();
                    }
                    std::vector<uint8_t> recycledBuf;
                    {
                        std::lock_guard<std::mutex> lk(outboxMutex);
                        if (!outboxBufferPool.empty()) {
                            recycledBuf = std::move(outboxBufferPool.back());
                            outboxBufferPool.pop_back();
                        }
                    }
                    ByteWriter w(std::move(recycledBuf));
                    w.reserve(estCap);
                    w.writeU16(static_cast<uint16_t>(capturer.frameWidth()));
                    w.writeU16(static_cast<uint16_t>(capturer.frameHeight()));
                    w.writeU16(static_cast<uint16_t>(dirtyTiles.size()));
                    for (const auto& t : dirtyTiles) {
                        TileHeader th{};
                        th.x = t.x;
                        th.y = t.y;
                        th.width = t.width;
                        th.height = t.height;
                        th.encoding = static_cast<uint8_t>(t.encoding);
                        th.dataSize = static_cast<uint32_t>(t.data.size());
                        w.writeBytes(&th, sizeof(th));
                        if (!t.data.empty()) {
                            w.writeBytes(t.data.data(), t.data.size());
                        }
                    }
                    uint8_t flags = isKf ? FLAG_KEYFRAME : FLAG_NONE;
                    enqueueHostPacketMove(PacketType::VIDEO_FRAME_TILES, flags, w.takeBuffer(), true);

                    // Send real-time Performance HUD telemetry (throttled to 250ms)
                    if (frameStart - lastHudSend >= 250) {
                        lastHudSend = frameStart;
                        PerformanceHudPayload hudPayload;
                        hudPayload.captureLatencyMs = capturer.lastCaptureLatencyMs();
                        hudPayload.encodeLatencyMs = capturer.lastEncodeLatencyMs();
                        hudPayload.dirtyTilesCount = static_cast<uint32_t>(std::min<size_t>(dirtyTiles.size(), 0xFFFFFFFFULL));
                        size_t rawBytes = 0;
                        size_t compBytes = 0;
                        for (const auto& t : dirtyTiles) {
                            rawBytes += static_cast<size_t>(t.width) * t.height * 4;
                            compBytes += t.data.size();
                        }
                        hudPayload.compressionRatio = (compBytes > 0) ? (static_cast<float>(rawBytes) / static_cast<float>(compBytes)) : 1.0f;
                        hudPayload.hostFps = static_cast<float>(effectiveFps);
                        std::vector<uint8_t> hudBytes;
                        serializePerformanceHud(hudPayload, hudBytes);
                        enqueueHostPacketMove(PacketType::PERFORMANCE_HUD_METRICS, 0, std::move(hudBytes), false);
                    }
                } else {
                    float cur = avgSendMs.load();
                    avgSendMs.store(cur * 0.92f);
                }

                if (std::abs(curState.normX - prevCursor.normX) > 0.001f ||
                    std::abs(curState.normY - prevCursor.normY) > 0.001f ||
                    curState.visible != prevCursor.visible) {
                    prevCursor = curState;
                    CursorUpdatePacket cp{ curState.normX, curState.normY, static_cast<uint8_t>(curState.visible ? 1 : 0) };
                    enqueueHostPacketRaw(PacketType::CURSOR_UPDATE, 0, &cp, sizeof(cp), false);
                }
            }

            if (hostLivePermissions_.load() & PERM_FILE_TRANSFER) {
                fileManager_.pumpOutgoingChunks(sendPacketHelper, 4);
            }

            if (clipboardSyncEnabled_.load() && (hostLivePermissions_.load() & PERM_CLIPBOARD) && (frameStart - lastClipboardCheck >= 400)) {
                lastClipboardCheck = frameStart;
                std::string newClip;
                if (clipboardManager_.pollLocalChange(newClip)) {
                    ByteWriter clipW;
                    clipW.writeString(newClip);
                    enqueueHostPacketMove(PacketType::CLIPBOARD_TEXT, 0, clipW.takeBuffer(), false);
                }
                clipFileMgr_.pollLocalClipboardFiles([&enqueueHostPacket](PacketType t, const std::vector<uint8_t>& p) {
                    enqueueHostPacket(t, 0, p, false);
                    return true;
                });
            }

            fileSyncMgr_.pumpOutgoingSync(sendPacketHelper, 4);

            uint64_t elapsed = nowTickMs() - frameStart;
            uint64_t targetInterval = (effectiveFps >= 60) ? 16 :
                                      (effectiveFps >= 30) ? 33 : 66;
            if (elapsed < targetInterval) {
                std::this_thread::sleep_for(std::chrono::milliseconds(targetInterval - elapsed));
            }
        }

        sessionAlive.store(false);
        outboxCv.notify_all();
        closeWinSock(clientSock);
        if (sendWorkerThread.joinable()) {
            try { sendWorkerThread.join(); } catch (...) {}
        }
        if (readerThread.joinable()) {
            try { readerThread.join(); } catch (...) {}
        }

        if (identity_.settings().lockWorkstationOnDisconnect) {
            LockWorkStation();
        }
    } catch (...) {}

    cleanup();
}

// ---------------- Viewer Session Client ----------------

bool NetworkEngine::connectToRemote(const std::string& targetIdOrAddr, const std::string& password) {
    disconnectViewer();
    std::lock_guard<std::mutex> vlk(viewerThreadMutex_);
    try {
        if (viewerThread_.joinable()) {
            viewerThread_.join();
        }
    } catch (...) {}

    viewerActive_.store(true);
    viewerEncrypted_.store(false);
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        viewerStats_ = ViewerSessionStats{};
        viewerStats_.state = ViewerConnectionState::ResolvingId;
        viewerStats_.statusMessage = "Resolving address...";
        viewerStats_.qualityPreset = identity_.settings().defaultQuality;
        viewerStats_.targetFps = clampTargetFps(identity_.settings().targetFps);
        viewerStats_.effectiveFpsCap = viewerStats_.targetFps;
        viewerStats_.adaptiveFps = identity_.settings().adaptiveFps;
    }

    viewerThread_ = std::thread(&NetworkEngine::runViewerSession, this, targetIdOrAddr, password);
    return true;
}

void NetworkEngine::disconnectViewer() {
    viewerActive_.store(false);
    shutdownViewerAudioPlayback();
    stopVoiceIntercom();
    stopViewerTunnelMultiplexer();
    viewerPrivacyModeActive_.store(false);
    whiteboardMgr_.clearAllStrokes();
    clipFileMgr_.reset();
    uintptr_t s = viewerSock_.exchange(~uintptr_t(0));
    if (s != ~uintptr_t(0)) {
        bool enc = viewerEncrypted_.exchange(false);
        {
            std::lock_guard<std::mutex> lock(viewerCipherMutex_);
            sendFrame(s, PacketType::DISCONNECT, 0, nullptr, 0, viewerSendMutex_,
                      enc ? &viewerCipher_ : nullptr,
                      enc ? &viewerSendSeq_ : nullptr);
            viewerCipher_.reset();
        }
        closeWinSock(s);
    }
    fileManager_.abortActiveTransfers();
    {
        std::lock_guard<std::mutex> lock(viewerTotpMutex_);
        viewerTotpCancelled_ = true;
        viewerTotpSubmitted_ = false;
        viewerTotpCv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        if (viewerStats_.state != ViewerConnectionState::Error) {
            viewerStats_.state = ViewerConnectionState::Disconnected;
            viewerStats_.statusMessage = "Disconnected";
        }
    }
    {
        std::lock_guard<std::mutex> lock(viewerFrameMutex_);
        viewerFullCanvasDirty_ = true;
        viewerDirtyBounds_ = RECT{0, 0, 0, 0};
    }
}

void NetworkEngine::submitViewerTotpCode(const std::string& code) {
    std::lock_guard<std::mutex> lock(viewerTotpMutex_);
    viewerTotpCode_ = code;
    viewerTotpSubmitted_ = true;
    viewerTotpCancelled_ = false;
    viewerTotpCv_.notify_all();
}

void NetworkEngine::cancelViewerTotp() {
    std::lock_guard<std::mutex> lock(viewerTotpMutex_);
    viewerTotpCancelled_ = true;
    viewerTotpSubmitted_ = false;
    viewerTotpCv_.notify_all();
}

bool NetworkEngine::isWaitingForTotp() const {
    std::lock_guard<std::mutex> lock(viewerStatsMutex_);
    return viewerStats_.state == ViewerConnectionState::WaitingTotp;
}

ViewerSessionStats NetworkEngine::viewerStats() const {
    std::lock_guard<std::mutex> lock(viewerStatsMutex_);
    ViewerSessionStats s = viewerStats_;
    s.privacyModeEngaged = viewerPrivacyModeActive_.load();
    s.audioMuted = audioMuted_.load();
    s.audioVolume = audioVolumePercent_.load();
    return s;
}

bool NetworkEngine::copyLatestViewerFrame(
    uint64_t& inOutSeq,
    std::vector<uint8_t>& outBgra,
    int& outW,
    int& outH,
    CursorState& outCursor,
    RECT* outDirtyBounds) const
{
    std::lock_guard<std::mutex> lock(viewerFrameMutex_);
    outCursor = viewerCursor_;
    if (viewerFrameSeq_ == 0 || viewerCanvasW_ <= 0 || viewerCanvasH_ <= 0) {
        return false;
    }
    if (viewerFrameSeq_ == inOutSeq && outW == viewerCanvasW_ && outH == viewerCanvasH_) {
        return false;
    }

    bool resChanged = (outW != viewerCanvasW_ || outH != viewerCanvasH_ || outBgra.size() != viewerCanvasBgra_.size());
    inOutSeq = viewerFrameSeq_;
    outW = viewerCanvasW_;
    outH = viewerCanvasH_;

    if (resChanged || viewerFullCanvasDirty_) {
        outBgra = viewerCanvasBgra_;
        if (outDirtyBounds) {
            *outDirtyBounds = RECT{0, 0, static_cast<LONG>(outW), static_cast<LONG>(outH)};
        }
        viewerFullCanvasDirty_ = false;
    } else {
        RECT r = viewerDirtyBounds_;
        r.left = std::clamp(r.left, 0L, static_cast<LONG>(outW));
        r.right = std::clamp(r.right, 0L, static_cast<LONG>(outW));
        r.top = std::clamp(r.top, 0L, static_cast<LONG>(outH));
        r.bottom = std::clamp(r.bottom, 0L, static_cast<LONG>(outH));
        if (r.right > r.left && r.bottom > r.top) {
            int stride = outW * 4;
            int copyBytes = (r.right - r.left) * 4;
            for (int y = r.top; y < r.bottom; ++y) {
                const uint8_t* srcRow = viewerCanvasBgra_.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(r.left) * 4;
                uint8_t* dstRow = outBgra.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(r.left) * 4;
                std::memcpy(dstRow, srcRow, static_cast<size_t>(copyBytes));
            }
        }
        if (outDirtyBounds) {
            *outDirtyBounds = r;
        }
    }
    return true;
}

void NetworkEngine::setOnFrameDecodedCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(onFrameDecodedMutex_);
    onFrameDecoded_ = std::move(cb);
}

void NetworkEngine::runViewerSession(std::string targetInput, std::string password) {
    auto setStatus = [&](ViewerConnectionState st, const std::string& msg) {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        viewerStats_.state = st;
        viewerStats_.statusMessage = msg;
    };

    while (!targetInput.empty() && (targetInput.front() == ' ' || targetInput.front() == '\t')) targetInput.erase(targetInput.begin());
    while (!targetInput.empty() && (targetInput.back() == ' ' || targetInput.back() == '\t')) targetInput.pop_back();

    uint64_t targetDeskId = CryptoUtils::parseDeskId(targetInput);
    std::string resolvedIp;
    uint16_t resolvedPort = DEFAULT_HOST_PORT;

    auto doConnectAndAuth = [&](bool isReconnecting) -> bool {
        SOCKET connectedSock = INVALID_SOCKET;

        if (targetDeskId > 0) {
            if (isReconnecting && !resolvedIp.empty() && resolvedIp.rfind("Relay:", 0) != 0) {
                connectedSock = connectTcpWithTimeout(resolvedIp, resolvedPort, 1200);
            }

            if (connectedSock == INVALID_SOCKET) {
                if (targetDeskId == identity_.deskId()) {
                    resolvedIp = "127.0.0.1";
                    resolvedPort = identity_.listenPort();
                    connectedSock = connectTcpWithTimeout(resolvedIp, resolvedPort, 1500);
                }
            }

            if (connectedSock == INVALID_SOCKET) {
                sendDiscoveryQuery(targetDeskId);
                for (int waitStep = 0; waitStep < (isReconnecting ? 4 : 6) && viewerActive_.load(); ++waitStep) {
                    {
                        std::lock_guard<std::mutex> lock(peersMutex_);
                        for (const auto& p : peers_) {
                            if (p.deskId == targetDeskId) {
                                resolvedIp = p.ip;
                                resolvedPort = p.port;
                                break;
                            }
                        }
                    }
                    if (!resolvedIp.empty()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }

                if (!resolvedIp.empty()) {
                    if (!isReconnecting) {
                        setStatus(ViewerConnectionState::ConnectingTcp, "Connecting on LAN (" + resolvedIp + ":" + std::to_string(resolvedPort) + ")...");
                    }
                    connectedSock = connectTcpWithTimeout(resolvedIp, resolvedPort, 1500);
                }
            }

            if (connectedSock == INVALID_SOCKET && viewerActive_.load()) {
                std::string rHost;
                uint16_t rPort = DEFAULT_RELAY_PORT;
                if (parseHostPort(identity_.relayServerAddress(), rHost, rPort, DEFAULT_RELAY_PORT)) {
                    if (!isReconnecting) {
                        setStatus(ViewerConnectionState::ResolvingId, "Querying Relay Server (" + rHost + ":" + std::to_string(rPort) + ")...");
                    }
                    SOCKET ls = connectTcpWithTimeout(rHost, rPort, 1500);
                    if (ls != INVALID_SOCKET) {
                        uintptr_t lSock = fromWinSock(ls);
                        ByteWriter lw;
                        lw.writeU64(targetDeskId);
                        std::mutex lm;
                        if (sendFrame(lSock, PacketType::RELAY_LOOKUP, 0, lw.buffer().data(), lw.buffer().size(), lm)) {
                            FrameHeader lhdr{};
                            std::vector<uint8_t> lpay;
                            if (recvFrame(lSock, lhdr, lpay) && static_cast<PacketType>(lhdr.type) == PacketType::RELAY_LOOKUP_RESP) {
                                try {
                                    ByteReader lr(lpay);
                                    uint8_t found = lr.readU8();
                                    std::string peerIp = lr.readString();
                                    uint16_t peerPort = lr.readU16();
                                    if (found && !peerIp.empty()) {
                                        resolvedIp = peerIp;
                                        resolvedPort = peerPort;
                                    }
                                } catch (...) {}
                            }
                        }
                        closeWinSock(lSock);
                    }

                    if (!resolvedIp.empty()) {
                        if (!isReconnecting) {
                            setStatus(ViewerConnectionState::ConnectingTcp, "Connecting to " + resolvedIp + ":" + std::to_string(resolvedPort) + "...");
                        }
                        connectedSock = connectTcpWithTimeout(resolvedIp, resolvedPort, 1500);
                    }

                    if (connectedSock == INVALID_SOCKET && viewerActive_.load()) {
                        if (!isReconnecting) {
                            setStatus(ViewerConnectionState::ConnectingTcp, "Bridging via Relay Server...");
                        }
                        SOCKET bs = connectTcpWithTimeout(rHost, rPort, 1500);
                        if (bs != INVALID_SOCKET) {
                            uintptr_t bSock = fromWinSock(bs);
                            ByteWriter bw;
                            bw.writeU64(targetDeskId);
                            std::mutex bm;
                            if (sendFrame(bSock, PacketType::RELAY_CONNECT_REQ, 0, bw.buffer().data(), bw.buffer().size(), bm)) {
                                FrameHeader bhdr{};
                                std::vector<uint8_t> bpay;
                                if (recvFrame(bSock, bhdr, bpay) && static_cast<PacketType>(bhdr.type) == PacketType::RELAY_BRIDGE_READY) {
                                    ByteReader br(bpay);
                                    if (br.hasRemaining(1) && br.readU8() == 1) {
                                        connectedSock = bs;
                                        resolvedIp = "Relay:" + rHost;
                                        resolvedPort = rPort;
                                    }
                                }
                            }
                            if (connectedSock == INVALID_SOCKET) {
                                closeWinSock(bSock);
                            }
                        }
                    }
                }
            }
        } else {
            if (!parseHostPort(targetInput, resolvedIp, resolvedPort, DEFAULT_HOST_PORT)) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Invalid Desk ID or IP:Port format.");
                }
                return false;
            }
            if (!isReconnecting) {
                setStatus(ViewerConnectionState::ConnectingTcp, "Connecting to " + resolvedIp + ":" + std::to_string(resolvedPort) + "...");
            }
            connectedSock = connectTcpWithTimeout(resolvedIp, resolvedPort, 2500);
        }

        if (connectedSock == INVALID_SOCKET) {
            if (!isReconnecting) {
                setStatus(ViewerConnectionState::Error, "Could not reach remote desk (" + targetInput + "). Verify the peer is online.");
            }
            return false;
        }

        uintptr_t vSock = fromWinSock(connectedSock);
        viewerSock_.store(vSock);
        viewerEncrypted_.store(false);

        try {
            if (!isReconnecting) {
                setStatus(ViewerConnectionState::Authenticating, "Performing cryptographic handshake...");
            }

            // Initialize Ephemeral ECDH (NIST P-256) for forward secrecy
            EcdhKeyExchange viewerEcdh;
            viewerEcdh.initialize();

            // 1. Send HELLO
            {
                ByteWriter w;
                w.writeU16(PROTOCOL_VERSION);
                w.writeU64(identity_.deskId());
                w.writeString(identity_.hostname());
                w.writeU16(static_cast<uint16_t>(viewerEcdh.localPublicKey().size()));
                if (!viewerEcdh.localPublicKey().empty()) {
                    w.writeBytes(viewerEcdh.localPublicKey().data(), viewerEcdh.localPublicKey().size());
                }
                w.writeString(CPP_DESK_VERSION);
                if (!sendFrame(vSock, PacketType::HELLO, 0, w.buffer().data(), w.buffer().size(), viewerSendMutex_)) {
                    FrameHeader ehdr{};
                    std::vector<uint8_t> epay;
                    if (recvFrame(vSock, ehdr, epay) && static_cast<PacketType>(ehdr.type) == PacketType::AUTH_RESULT) {
                        ByteReader r(epay);
                        r.readU8(); r.readU8();
                        std::string msg = r.readString();
                        if (!isReconnecting) {
                            setStatus(ViewerConnectionState::Error, msg.empty() ? "Host busy or rate-limited." : msg);
                        }
                    } else if (!isReconnecting) {
                        setStatus(ViewerConnectionState::Error, "Connection closed during handshake.");
                    }
                    closeWinSock(vSock);
                    viewerSock_.store(~uintptr_t(0));
                    return false;
                }
            }

            // 2. Receive AUTH_CHALLENGE
            FrameHeader hdr{};
            std::vector<uint8_t> payload;
            if (!recvFrame(vSock, hdr, payload)) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Failed to receive authentication challenge.");
                }
                closeWinSock(vSock);
                viewerSock_.store(~uintptr_t(0));
                return false;
            }

            if (static_cast<PacketType>(hdr.type) == PacketType::AUTH_RESULT) {
                ByteReader r(payload);
                r.readU8(); r.readU8();
                std::string msg = r.readString();
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, msg.empty() ? "Host busy or rate-limited." : msg);
                }
                closeWinSock(vSock);
                viewerSock_.store(~uintptr_t(0));
                return false;
            }

            if (static_cast<PacketType>(hdr.type) != PacketType::AUTH_CHALLENGE) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Unexpected handshake packet.");
                }
                closeWinSock(vSock);
                viewerSock_.store(~uintptr_t(0));
                return false;
            }

            ByteReader chalReader(payload);
            uint64_t remoteId = chalReader.readU64();
            std::string remoteHost = chalReader.readString();
            std::array<uint8_t, 32> nonce{};
            chalReader.readBytes(nonce.data(), nonce.size());
            /*uint8_t unattendedAllowed =*/ chalReader.readU8();
            std::vector<uint8_t> hostPubBlob;
            if (chalReader.hasRemaining(2)) {
                uint16_t pubLen = chalReader.readU16();
                if (chalReader.hasRemaining(pubLen)) {
                    hostPubBlob = chalReader.readBytesVector(pubLen);
                }
            }
            std::string hostAppVersion;
            if (chalReader.hasRemaining(2)) {
                hostAppVersion = chalReader.readString();
            }
            {
                std::lock_guard<std::mutex> lk(peerVersionMutex_);
                remotePeerAppVersion_ = hostAppVersion;
            }

            // 3. Compute & send AUTH_RESPONSE
            {
                ByteWriter w;
                if (autoReconnectingWithToken_.load() && !rebootResumeToken_.empty()) {
                    w.writeU8(2);
                    auto tokenBytes = CryptoUtils::fromHex(rebootResumeToken_);
                    if (tokenBytes.size() == 32) {
                        w.writeBytes(tokenBytes.data(), 32);
                    } else {
                        std::array<uint8_t, 32> tb{};
                        std::memcpy(tb.data(), tokenBytes.data(), std::min<size_t>(32, tokenBytes.size()));
                        w.writeBytes(tb.data(), 32);
                    }
                } else if (!password.empty()) {
                    w.writeU8(1);
                    auto digest = CryptoUtils::computeChallengeResponse(password, remoteId, identity_.deskId(), nonce);
                    w.writeBytes(digest.data(), digest.size());
                } else {
                    w.writeU8(0);
                    std::array<uint8_t, 32> zero{};
                    w.writeBytes(zero.data(), zero.size());
                }
                if (!sendFrame(vSock, PacketType::AUTH_RESPONSE, 0, w.buffer().data(), w.buffer().size(), viewerSendMutex_)) {
                    if (!isReconnecting) {
                        setStatus(ViewerConnectionState::Error, "Failed to send authentication response.");
                    }
                    closeWinSock(vSock);
                    viewerSock_.store(~uintptr_t(0));
                    return false;
                }
            }

            // 4. Receive AUTH_WAITING and/or AUTH_RESULT
            while (viewerActive_.load()) {
                if (!recvFrame(vSock, hdr, payload)) {
                    if (!isReconnecting) {
                        setStatus(ViewerConnectionState::Error, "Connection closed during authorization.");
                    }
                    closeWinSock(vSock);
                    viewerSock_.store(~uintptr_t(0));
                    return false;
                }
                PacketType pt = static_cast<PacketType>(hdr.type);
                if (pt == PacketType::AUTH_WAITING) {
                    setStatus(ViewerConnectionState::WaitingApproval,
                              "Waiting for " + remoteHost + " (" + CryptoUtils::formatDeskId(remoteId) + ") to click Accept...");
                    continue;
                }
                if (pt == PacketType::AUTH_RESULT) {
                    ByteReader r(payload);
                    AuthResultCode code = static_cast<AuthResultCode>(r.readU8());
                    uint8_t perms = r.readU8();
                    std::string msg = r.readString();

                    if (code == AuthResultCode::TotpRequired || code == AuthResultCode::TotpInvalid) {
                        while (viewerActive_.load()) {
                            setStatus(ViewerConnectionState::WaitingTotp,
                                      (code == AuthResultCode::TotpInvalid)
                                          ? "Invalid 2FA code. Please try again."
                                          : "Two-Factor Authentication required. Enter the 6-digit code.");

                            // Wait for user input via modal dialog
                            std::string codeToSend;
                            {
                                std::unique_lock<std::mutex> lock(viewerTotpMutex_);
                                viewerTotpSubmitted_ = false;
                                viewerTotpCancelled_ = false;
                                viewerTotpCode_.clear();
                                bool signaled = viewerTotpCv_.wait_for(lock, std::chrono::seconds(45), [&]() {
                                    return viewerTotpSubmitted_ || viewerTotpCancelled_ || !viewerActive_.load();
                                });
                                if (!signaled || viewerTotpCancelled_ || !viewerActive_.load()) {
                                    setStatus(ViewerConnectionState::Error, "2FA verification cancelled or timed out.");
                                    closeWinSock(vSock);
                                    viewerSock_.store(~uintptr_t(0));
                                    return false;
                                }
                                codeToSend = viewerTotpCode_;
                            }

                            // Send PacketType::TOTP_VERIFY
                            ByteWriter tw;
                            tw.writeString(codeToSend);
                            if (!sendFrame(vSock, PacketType::TOTP_VERIFY, 0, tw.buffer().data(), tw.buffer().size(), viewerSendMutex_)) {
                                setStatus(ViewerConnectionState::Error, "Failed to send 2FA verification code.");
                                closeWinSock(vSock);
                                viewerSock_.store(~uintptr_t(0));
                                return false;
                            }

                            // Receive next result packet
                            if (!recvFrame(vSock, hdr, payload)) {
                                setStatus(ViewerConnectionState::Error, "Connection closed during 2FA verification.");
                                closeWinSock(vSock);
                                viewerSock_.store(~uintptr_t(0));
                                return false;
                            }

                            if (static_cast<PacketType>(hdr.type) == PacketType::AUTH_RESULT) {
                                ByteReader r2(payload);
                                code = static_cast<AuthResultCode>(r2.readU8());
                                perms = r2.readU8();
                                msg = r2.readString();

                                if (code == AuthResultCode::Accepted) {
                                    // Successfully verified! Break out and proceed to session cipher setup
                                    break;
                                } else if (code == AuthResultCode::TotpInvalid) {
                                    // Code was wrong, loop again to prompt user with error caption
                                    continue;
                                } else {
                                    // Locked out, rate-limited, or host error
                                    setStatus(ViewerConnectionState::Error, msg.empty() ? "2FA verification failed." : msg);
                                    closeWinSock(vSock);
                                    viewerSock_.store(~uintptr_t(0));
                                    return false;
                                }
                            }
                        }
                    }

                    if (code != AuthResultCode::Accepted) {
                        if (!isReconnecting) {
                            setStatus(ViewerConnectionState::Error, msg.empty() ? "Authentication rejected." : msg);
                        }
                        closeWinSock(vSock);
                        viewerSock_.store(~uintptr_t(0));
                        return false;
                    }

                    // Activate mandatory end-to-end stream encryption
                    if (!hostPubBlob.empty()) {
                        viewerEcdh.computeSharedSessionKey(
                            hostPubBlob.data(), hostPubBlob.size(),
                            remoteId, identity_.deskId(), nonce, viewerSessionKey_);
                    } else {
                        viewerSessionKey_ = CryptoUtils::deriveSessionKey(remoteId, identity_.deskId(), nonce);
                    }

                    std::string sasFingerprint;
                    {
                        std::lock_guard<std::mutex> lock(viewerCipherMutex_);
                        viewerCipher_.initialize(viewerSessionKey_, /*isHost=*/false);
                        viewerSendSeq_ = 0;
                        viewerEncrypted_.store(true);
                        sasFingerprint = CryptoUtils::sessionFingerprintHex(viewerSessionKey_);
                    }

                    {
                        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                        viewerStats_.state = ViewerConnectionState::Connected;
                        viewerStats_.statusMessage = isReconnecting ? ("Reconnected to " + remoteHost) : ("Connected to " + remoteHost);
                        viewerStats_.reconnectAttempt = 0;
                        viewerStats_.remoteDeskId = remoteId;
                        viewerStats_.remoteHostname = remoteHost;
                        viewerStats_.remoteAddress = resolvedIp + ":" + std::to_string(resolvedPort);
                        viewerStats_.securityFingerprint = sasFingerprint;
                        if (!isReconnecting) {
                            viewerStats_.connectedSinceTickMs = nowTickMs();
                        }
                        viewerStats_.grantedPermissions = perms;
                    }
                    identity_.addOrUpdateRecentSession(remoteId, remoteHost, targetInput);
                    initViewerAudioPlayback();
                    startViewerTunnelMultiplexer();

                    requestVideoSettings(
                        identity_.settings().defaultQuality,
                        viewerStats_.activeMonitorIndex,
                        true,
                        clampTargetFps(identity_.settings().targetFps),
                        identity_.settings().adaptiveFps ? 1 : 0
                    );

                    if (!hostAppVersion.empty()) {
                        auto viewerVer = SemanticVersion::parse(CPP_DESK_VERSION);
                        auto hostVer = SemanticVersion::parse(hostAppVersion);
                        if (viewerVer < hostVer) {
                            localIsOutdated_.store(true);
                            std::function<void(const std::string&, const std::string&)> cb;
                            {
                                std::lock_guard<std::mutex> lk(peerVersionMutex_);
                                cb = onVersionMismatch_;
                            }
                            if (cb) {
                                cb(hostAppVersion, CPP_DESK_VERSION);
                            }
                        } else if (viewerVer > hostVer) {
                            remoteIsOutdated_.store(true);
                            VersionUpgradeNoticePayload notice;
                            notice.senderVersion = CPP_DESK_VERSION;
                            notice.minimumVersion = CPP_DESK_VERSION;
                            notice.downloadUrl = "https://github.com/nmnghia2527/cppdesk/releases/latest";
                            notice.message = "Connected viewer is running CppDesk " + std::string(CPP_DESK_VERSION) + ". Host update recommended.";
                            std::vector<uint8_t> noticeBuf;
                            serializeVersionUpgradeNotice(notice, noticeBuf);
                            sendViewerEncryptedPacket(PacketType::VERSION_UPGRADE_NOTICE, 0, noticeBuf.data(), noticeBuf.size());
                        }
                    }
                    return true;
                }
            }
        } catch (...) {}

        closeWinSock(vSock);
        viewerSock_.store(~uintptr_t(0));
        return false;
    };

    bool isReconnecting = false;

    while (viewerActive_.load() && running_.load()) {
        if (!isReconnecting) {
            if (!doConnectAndAuth(false)) {
                viewerActive_.store(false);
                break;
            }
        } else {
            bool recovered = false;
            uint32_t maxAttempts = autoReconnectingWithToken_.load() ? 40 : 3;
            for (uint32_t attempt = 1; attempt <= maxAttempts && viewerActive_.load() && running_.load(); ++attempt) {
                {
                    std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                    viewerStats_.state = ViewerConnectionState::Reconnecting;
                    viewerStats_.reconnectAttempt = attempt;
                    if (rebootPending_.load() || autoReconnectingWithToken_.load()) {
                        viewerStats_.statusMessage = "Host rebooting... Waiting for reconnect (" + std::to_string(attempt) + "/" + std::to_string(maxAttempts) + ")...";
                    } else {
                        viewerStats_.statusMessage = "Connection lost. Reconnecting (attempt " + std::to_string(attempt) + "/3)...";
                    }
                }
                uint32_t backoffMs = autoReconnectingWithToken_.load() ? 2500 : ((attempt == 1) ? 1000 : (attempt == 2) ? 3000 : 5000);
                for (uint32_t waited = 0; waited < backoffMs && viewerActive_.load() && running_.load(); waited += 100) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!viewerActive_.load() || !running_.load()) break;

                if (doConnectAndAuth(true)) {
                    recovered = true;
                    autoReconnectingWithToken_.store(false);
                    rebootPending_.store(false);
                    break;
                }
            }

            if (!recovered) {
                if (viewerActive_.load()) {
                    setStatus(ViewerConnectionState::Error, "Connection lost. Reconnect failed after 3 attempts.");
                    viewerActive_.store(false);
                }
                break;
            }
        }

        uintptr_t vSock = viewerSock_.load();
        if (vSock == ~uintptr_t(0)) break;

        // 5. Connected! Receive encrypted video tiles, cursor updates, clipboard, files, and chat
        uint64_t viewerRecvSeq = 0;
        uint64_t lastMetricTick = nowTickMs();
        uint64_t lastPingTick = 0;
        uint32_t framesInWindow = 0;
        uint64_t bytesInWindow = 0;
        bool socketDropped = false;

        auto sendPacketHelper = [&](PacketType pt, const std::vector<uint8_t>& buf) -> bool {
            return sendViewerEncryptedPacket(pt, 0, buf.data(), buf.size());
        };

        try {
            while (viewerActive_.load() && running_.load()) {
                uint64_t now = nowTickMs();

                if (now - lastPingTick >= 1000) {
                    lastPingTick = now;
                    uint32_t curRtt = 0;
                    {
                        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                        curRtt = viewerStats_.rttMs;
                    }
                    PingPacket pp{ now, curRtt };
                    sendViewerEncryptedPacket(PacketType::PING, 0, &pp, sizeof(pp));

                    std::string newClip;
                    if (clipboardSyncEnabled_.load() && clipboardManager_.pollLocalChange(newClip)) {
                        ByteWriter cw;
                        cw.writeString(newClip);
                        sendViewerEncryptedPacket(PacketType::CLIPBOARD_TEXT, 0, cw.buffer().data(), cw.buffer().size());
                    }
                    if (clipboardSyncEnabled_.load()) {
                        clipFileMgr_.pollLocalClipboardFiles([this](PacketType t, const std::vector<uint8_t>& p) {
                            return sendViewerEncryptedPacket(t, 0, p.data(), p.size());
                        });
                    }
                }

                fileManager_.pumpOutgoingChunks(sendPacketHelper, 4);
                fileSyncMgr_.pumpOutgoingSync(sendPacketHelper, 4);

                SOCKET ws = toWinSock(vSock);
                if (ws == INVALID_SOCKET) {
                    socketDropped = true;
                    break;
                }

                fd_set rfds{};
                FD_ZERO(&rfds);
                FD_SET(ws, &rfds);
                timeval tv{ 0, 20000 }; // 20 ms

                int sel = select(0, &rfds, nullptr, nullptr, &tv);
                if (sel < 0) {
                    socketDropped = true;
                    break;
                }
                if (sel == 0) continue;

                FrameHeader hdr{};
                std::vector<uint8_t> payload;
                if (!recvFrame(vSock, hdr, payload, &viewerCipher_, &viewerRecvSeq)) {
                    socketDropped = true;
                    break;
                }

                bytesInWindow += sizeof(FrameHeader) + payload.size();
                PacketType pt = static_cast<PacketType>(hdr.type);

                try {
                    ByteReader r(payload);
                    switch (pt) {
                        case PacketType::VIDEO_CONFIG: {
                            int32_t monIdx = r.readI32();
                            int32_t fw = r.readI32();
                            int32_t fh = r.readI32();
                            uint16_t mcount = r.readU16();
                            std::vector<MonitorDesc> mons;
                            for (uint16_t i = 0; i < mcount; ++i) {
                                MonitorDesc m;
                                m.index = r.readI32();
                                m.x = r.readI32();
                                m.y = r.readI32();
                                m.width = r.readI32();
                                m.height = r.readI32();
                                m.isPrimary = (r.readU8() != 0);
                                m.name = r.readString();
                                if (r.hasRemaining(1 + 4)) {
                                    m.isVirtual = (r.readU8() != 0);
                                    m.virtualId = r.readU32();
                                }
                                mons.push_back(m);
                            }
                            {
                                std::lock_guard<std::mutex> lock(viewerFrameMutex_);
                                viewerCanvasW_ = fw;
                                viewerCanvasH_ = fh;
                                viewerCanvasBgra_.assign(static_cast<size_t>(fw) * fh * 4, 0);
                                viewerFrameSeq_++;
                            }
                            {
                                std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                viewerStats_.activeMonitorIndex = monIdx;
                                viewerStats_.monitorCount = std::max<int>(1, static_cast<int>(mons.size()));
                                viewerStats_.monitors = std::move(mons);
                                viewerStats_.frameWidth = fw;
                                viewerStats_.frameHeight = fh;
                            }
                            break;
                        }
                        case PacketType::MONITOR_LIST: {
                            std::vector<MonitorDesc> mons;
                            if (deserializeMonitorList(payload.data(), payload.size(), mons)) {
                                std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                viewerStats_.monitors = std::move(mons);
                                viewerStats_.monitorCount = std::max<int>(1, static_cast<int>(viewerStats_.monitors.size()));
                            }
                            break;
                        }
                        case PacketType::VIRTUAL_DISPLAY_STATUS: {
                            VirtualDisplayStatusPayload status{};
                            if (deserializeVirtualDisplayStatus(payload.data(), payload.size(), status)) {
                                std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                viewerStats_.lastVirtualDisplayStatus = status;
                                if (!status.message.empty()) {
                                    viewerStats_.statusMessage = status.message;
                                }
                            }
                            break;
                        }
                        case PacketType::VIDEO_FRAME_TILES: {
                            uint16_t fw = r.readU16();
                            uint16_t fh = r.readU16();
                            uint16_t tileCount = r.readU16();

                            auto decStart = std::chrono::steady_clock::now();
                            {
                                std::lock_guard<std::mutex> lock(viewerFrameMutex_);
                                bool resChanged = (viewerCanvasW_ != fw || viewerCanvasH_ != fh);
                                if (resChanged) {
                                    viewerCanvasW_ = fw;
                                    viewerCanvasH_ = fh;
                                    viewerCanvasBgra_.assign(static_cast<size_t>(fw) * fh * 4, 0);
                                    viewerFullCanvasDirty_ = true;
                                    viewerDirtyBounds_ = RECT{0, 0, static_cast<LONG>(fw), static_cast<LONG>(fh)};
                                }
                                if (tileCount > 0) {
                                    thread_local std::vector<TileThreadPool::DecodeTask> decodeTasks;
                                    decodeTasks.clear();
                                    if (decodeTasks.capacity() < tileCount) {
                                        decodeTasks.reserve(tileCount);
                                    }
                                    int minX = fw, minY = fh, maxX = 0, maxY = 0;
                                    for (uint16_t i = 0; i < tileCount; ++i) {
                                        if (!r.hasRemaining(sizeof(TileHeader))) break;
                                        TileHeader th{};
                                        r.readBytes(&th, sizeof(th));
                                        if (!r.hasRemaining(th.dataSize)) break;
                                        const uint8_t* tileDataPtr = r.currentPtr();
                                        r.skip(th.dataSize);

                                        TileThreadPool::DecodeTask dt;
                                        dt.rx = th.x;
                                        dt.ry = th.y;
                                        dt.rw = th.width;
                                        dt.rh = th.height;
                                        dt.encoding = static_cast<TileEncoding>(th.encoding);
                                        dt.data = tileDataPtr;
                                        dt.dataSize = th.dataSize;
                                        dt.canvasBgra = viewerCanvasBgra_.data();
                                        dt.canvasW = viewerCanvasW_;
                                        dt.canvasH = viewerCanvasH_;
                                        decodeTasks.push_back(dt);

                                        minX = std::min(minX, static_cast<int>(th.x));
                                        minY = std::min(minY, static_cast<int>(th.y));
                                        maxX = std::max(maxX, static_cast<int>(th.x + th.width));
                                        maxY = std::max(maxY, static_cast<int>(th.y + th.height));
                                    }
                                    TileThreadPool::instance().parallelDecode(decodeTasks);
                                    if (!resChanged) {
                                        if (viewerFullCanvasDirty_) {
                                            viewerDirtyBounds_ = RECT{0, 0, static_cast<LONG>(fw), static_cast<LONG>(fh)};
                                        } else {
                                            if (maxX > minX && maxY > minY) {
                                                viewerDirtyBounds_ = RECT{
                                                    static_cast<LONG>(std::clamp(minX, 0, static_cast<int>(fw))),
                                                    static_cast<LONG>(std::clamp(minY, 0, static_cast<int>(fh))),
                                                    static_cast<LONG>(std::clamp(maxX, 0, static_cast<int>(fw))),
                                                    static_cast<LONG>(std::clamp(maxY, 0, static_cast<int>(fh)))
                                                };
                                            } else {
                                                viewerDirtyBounds_ = RECT{0, 0, 0, 0};
                                            }
                                        }
                                    }
                                }
                                viewerFrameSeq_++;
                            }
                            auto decEnd = std::chrono::steady_clock::now();
                            float decMs = std::chrono::duration<float, std::milli>(decEnd - decStart).count();
                            {
                                std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                viewerStats_.decodeLatencyMs = decMs;
                                viewerStats_.deltaTilesCount = tileCount;
                            }

                            std::function<void()> frameCb;
                            {
                                std::lock_guard<std::mutex> lk(onFrameDecodedMutex_);
                                frameCb = onFrameDecoded_;
                            }
                            if (frameCb) {
                                frameCb();
                            }

                            framesInWindow++;
                            uint64_t tNow = nowTickMs();
                            if (tNow - lastMetricTick >= 500) {
                                double sec = static_cast<double>(tNow - lastMetricTick) / 1000.0;
                                std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                viewerStats_.frameWidth = fw;
                                viewerStats_.frameHeight = fh;
                                viewerStats_.fps = static_cast<float>(framesInWindow / sec);
                                viewerStats_.kbps = static_cast<float>((bytesInWindow / 1024.0) / sec);
                                framesInWindow = 0;
                                bytesInWindow = 0;
                                lastMetricTick = tNow;
                            }
                            break;
                        }
                        case PacketType::PERFORMANCE_HUD_METRICS: {
                            PerformanceHudPayload p{};
                            if (deserializePerformanceHud(payload.data(), payload.size(), p)) {
                                std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                viewerStats_.captureLatencyMs = p.captureLatencyMs;
                                viewerStats_.encodeLatencyMs = p.encodeLatencyMs;
                                viewerStats_.deltaTilesCount = p.dirtyTilesCount;
                                if (p.compressionRatio > 0.0f) {
                                    viewerStats_.compressionRatio = p.compressionRatio;
                                }
                            }
                            break;
                        }
                        case PacketType::REMOTE_REBOOT_CONFIRM: {
                            RemoteRebootConfirmPayload c{};
                            if (deserializeRemoteRebootConfirm(payload.data(), payload.size(), c) && c.accepted) {
                                std::lock_guard<std::mutex> lock(rebootTokenMutex_);
                                rebootResumeToken_ = c.resumeTokenHex;
                                rebootResumeDeskId_ = targetDeskId;
                                rebootResumeTargetInput_ = targetInput;
                                autoReconnectingWithToken_.store(true);
                                rebootPending_.store(true);
                                rebootCountdown_.store(c.countdownSeconds);
                            }
                            break;
                        }
                        case PacketType::CURSOR_UPDATE: {
                            if (r.hasRemaining(sizeof(CursorUpdatePacket))) {
                                CursorUpdatePacket cp{};
                                r.readBytes(&cp, sizeof(cp));
                                float cx = cp.normX;
                                float cy = cp.normY;
                                bool cvis = (cp.visible != 0);
                                {
                                    std::lock_guard<std::mutex> lock(viewerFrameMutex_);
                                    viewerCursor_.normX = cx;
                                    viewerCursor_.normY = cy;
                                    viewerCursor_.visible = cvis;
                                    viewerFrameSeq_++;
                                }
                                {
                                    std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                    viewerStats_.remoteCursor = { cx, cy, cvis };
                                }
                                std::function<void()> frameCb;
                                {
                                    std::lock_guard<std::mutex> lk(onFrameDecodedMutex_);
                                    frameCb = onFrameDecoded_;
                                }
                                if (frameCb) {
                                    frameCb();
                                }
                            }
                            break;
                        }
                        case PacketType::PERMISSION_UPDATE: {
                            uint8_t newPerms = r.readU8();
                            std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                            viewerStats_.grantedPermissions = newPerms;
                            break;
                        }
                        case PacketType::PONG: {
                            uint64_t sentTs = r.readU64();
                            uint64_t rtt = nowTickMs() - sentTs;
                            std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                            viewerStats_.rttMs = static_cast<uint32_t>(rtt);
                            viewerStats_.rttHistory.push_back(static_cast<float>(rtt));
                            if (viewerStats_.rttHistory.size() > 30) {
                                viewerStats_.rttHistory.erase(viewerStats_.rttHistory.begin());
                            }
                            uint8_t effCap = computeAdaptiveFpsCap(
                                viewerStats_.targetFps,
                                viewerStats_.adaptiveFps,
                                viewerStats_.rttMs,
                                0.0f
                            );
                            viewerStats_.effectiveFpsCap = effCap;
                            viewerStats_.networkThrottled = (effCap < viewerStats_.targetFps);
                            break;
                        }
                        case PacketType::CLIPBOARD_TEXT: {
                            std::string txt = r.readString();
                            if (clipboardSyncEnabled_.load()) {
                                clipboardManager_.applyRemoteClipboard(txt);
                            }
                            break;
                        }
                        case PacketType::CLIPBOARD_FILE_LIST: {
                            uint32_t tid = 0;
                            std::vector<VirtualFileEntry> files;
                            if (deserializeClipboardFileList(r.currentPtr(), r.remaining(), tid, files)) {
                                clipFileMgr_.handleRemoteFileList(tid, files, [this](PacketType t, const std::vector<uint8_t>& p) {
                                    return sendViewerEncryptedPacket(t, 0, p.data(), p.size());
                                });
                            }
                            break;
                        }
                        case PacketType::CLIPBOARD_FILE_REQUEST: {
                            if (r.hasRemaining(sizeof(ClipboardFileRequestHeader))) {
                                ClipboardFileRequestHeader reqHdr{};
                                r.readBytes(&reqHdr, sizeof(reqHdr));
                                clipFileMgr_.handleFileRequest(reqHdr.transferId, reqHdr.fileIndex, reqHdr.offset, reqHdr.length,
                                    [this](PacketType t, const std::vector<uint8_t>& p) {
                                        return sendViewerEncryptedPacket(t, 0, p.data(), p.size());
                                    });
                            }
                            break;
                        }
                        case PacketType::CLIPBOARD_FILE_CHUNK: {
                            if (r.hasRemaining(sizeof(ClipboardFileChunkHeader))) {
                                ClipboardFileChunkHeader chkHdr{};
                                r.readBytes(&chkHdr, sizeof(chkHdr));
                                if (r.hasRemaining(chkHdr.dataLength)) {
                                    clipFileMgr_.handleFileChunk(chkHdr.transferId, chkHdr.fileIndex, chkHdr.offset,
                                        r.currentPtr(), chkHdr.dataLength, chkHdr.sha256);
                                }
                            }
                            break;
                        }
                        case PacketType::CLIPBOARD_FILE_CANCEL: {
                            if (r.hasRemaining(sizeof(ClipboardFileCancelHeader))) {
                                ClipboardFileCancelHeader cnlHdr{};
                                r.readBytes(&cnlHdr, sizeof(cnlHdr));
                                clipFileMgr_.handleFileCancel(cnlHdr.transferId, cnlHdr.reasonCode);
                            }
                            break;
                        }
                        case PacketType::FILE_OFFER: {
                            uint32_t tid = r.readU32();
                            uint64_t fsz = r.readU64();
                            std::string fname = r.readString();
                            FileOfferTarget targetHint = FileOfferTarget::DefaultDownloads;
                            float dropNx = 0.0f, dropNy = 0.0f;
                            if (r.hasRemaining(1)) {
                                targetHint = static_cast<FileOfferTarget>(r.readU8());
                            }
                            if (r.hasRemaining(8)) {
                                dropNx = r.readF32();
                                dropNy = r.readF32();
                            }
                            fileManager_.handleFileOffer(tid, fsz, fname, targetHint, dropNx, dropNy);
                            break;
                        }
                        case PacketType::FILE_CHUNK: {
                            uint32_t tid = r.readU32();
                            uint64_t off = r.readU64();
                            uint32_t clen = r.readU32();
                            if (r.hasRemaining(clen)) {
                                fileManager_.handleFileChunk(tid, off, r.currentPtr(), clen);
                            }
                            break;
                        }
                        case PacketType::FILE_COMPLETE: {
                            uint32_t tid = r.readU32();
                            std::string sha = r.readString();
                            fileManager_.handleFileComplete(tid, sha);
                            break;
                        }
                        case PacketType::FILE_CANCEL: {
                            uint32_t tid = r.readU32();
                            fileManager_.handleFileCancel(tid);
                            break;
                        }
                        case PacketType::CHAT_MESSAGE: {
                            std::string sender = r.readString();
                            std::string msg = r.readString();
                            if (!msg.empty()) {
                                {
                                    std::lock_guard<std::mutex> cl(chatMutex_);
                                    chatHistory_.push_back({ sender, msg, false, nowTickMs() });
                                    if (chatHistory_.size() > 100) chatHistory_.erase(chatHistory_.begin());
                                }
                                unreadChatCount_.fetch_add(1);
                            }
                            break;
                        }
                        case PacketType::CHAT_MEDIA_MESSAGE: {
                            ChatMediaPayload p{};
                            if (deserializeChatMedia(payload.data(), payload.size(), p)) {
                                {
                                    std::lock_guard<std::mutex> cl(chatMutex_);
                                    chatHistory_.push_back({ p.senderName, p.captionText, false, p.timestampMs, true, std::move(p.jpegData), p.imgWidth, p.imgHeight });
                                    if (chatHistory_.size() > 100) chatHistory_.erase(chatHistory_.begin());
                                }
                                unreadChatCount_.fetch_add(1);
                            }
                            break;
                        }
                        case PacketType::AUDIO_STREAM_CHUNK: {
                            enqueueViewerAudioChunk(payload.data(), payload.size());
                            break;
                        }
                        case PacketType::VOICE_INTERCOM_CHUNK: {
                            handleIncomingVoiceChunk(payload.data(), payload.size());
                            break;
                        }
                        case PacketType::PRIVACY_MODE_TOGGLE: {
                            if (payload.size() >= sizeof(PrivacyModePayload)) {
                                PrivacyModePayload ack{};
                                std::memcpy(&ack, payload.data(), sizeof(ack));
                                viewerPrivacyModeActive_.store(ack.enable != 0);
                                {
                                    std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                    viewerStats_.privacyModeEngaged = (ack.enable != 0);
                                }
                            }
                            break;
                        }
                        case PacketType::SYSTEM_DIAGNOSTICS: {
                            SystemDiagnosticsPayload diag;
                            if (deserializeSystemDiagnostics(payload.data(), payload.size(), diag)) {
                                std::lock_guard<std::mutex> lk(diagnosticsMutex_);
                                latestDiagnostics_ = std::move(diag);
                            }
                            break;
                        }
                        case PacketType::TUNNEL_DATA: {
                            if (payload.size() >= sizeof(TunnelDataHeader)) {
                                TunnelDataHeader tdh{};
                                std::memcpy(&tdh, payload.data(), sizeof(tdh));
                                const uint8_t* tdata = payload.data() + sizeof(tdh);
                                size_t tlen = payload.size() - sizeof(tdh);
                                if (tlen > 0) {
                                    SOCKET cs = INVALID_SOCKET;
                                    uint32_t rId = 0;
                                    {
                                        std::lock_guard<std::mutex> lk(tunnelMutex_);
                                        auto it = viewerTunnels_.find(tdh.tunnelId);
                                        if (it != viewerTunnels_.end()) {
                                            cs = it->second.sock;
                                            rId = it->second.ruleId;
                                        }
                                    }
                                    if (cs != INVALID_SOCKET) {
                                        sendAllBytes(cs, tdata, tlen);
                                        std::lock_guard<std::mutex> lk(tunnelMutex_);
                                        for (auto& r : tunnelRules_) {
                                            if (r.ruleId == rId) {
                                                r.bytesTransferredIn += tlen;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case PacketType::TUNNEL_CLOSE: {
                            if (payload.size() >= sizeof(TunnelCloseHeader)) {
                                TunnelCloseHeader tch{};
                                std::memcpy(&tch, payload.data(), sizeof(tch));
                                std::lock_guard<std::mutex> lk(tunnelMutex_);
                                auto it = viewerTunnels_.find(tch.tunnelId);
                                if (it != viewerTunnels_.end()) {
                                    closesocket(it->second.sock);
                                    viewerTunnels_.erase(it);
                                }
                            }
                            break;
                        }
                        case PacketType::TERMINAL_DATA: {
                            if (payload.size() >= sizeof(TerminalDataHeader)) {
                                TerminalDataHeader tdh{};
                                std::memcpy(&tdh, payload.data(), sizeof(tdh));
                                const char* ttext = reinterpret_cast<const char*>(payload.data() + sizeof(tdh));
                                size_t tlen = payload.size() - sizeof(tdh);
                                if (tlen > 0) {
                                    std::lock_guard<std::mutex> lk(terminalMutex_);
                                    if (terminalLines_.empty()) {
                                        terminalLines_.push_back("");
                                    }
                                    for (size_t i = 0; i < tlen; ++i) {
                                        char ch = ttext[i];
                                        if (ch == '\r') {
                                            continue;
                                        }
                                        if (ch == '\n') {
                                            terminalLines_.push_back("");
                                            if (terminalLines_.size() > 500) {
                                                terminalLines_.erase(terminalLines_.begin());
                                            }
                                        } else {
                                            terminalLines_.back() += ch;
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case PacketType::WHITEBOARD_PACKET: {
                            AnnotationStroke stroke{};
                            if (WhiteboardManager::deserializeStroke(payload.data(), payload.size(), stroke)) {
                                whiteboardMgr_.applyRemoteStroke(stroke);
                            }
                            break;
                        }
                        case PacketType::SYNC_SCAN_RESP: {
                            SyncScanRespPayload resp;
                            if (deserializeSyncScanResp(payload.data(), payload.size(), resp)) {
                                fileSyncMgr_.handleScanResp(resp);
                                fileSyncMgr_.generateDiffPlan();
                            }
                            break;
                        }
                        case PacketType::SYNC_HASH_RESP: {
                            SyncHashRespPayload resp;
                            if (deserializeSyncHashResp(payload.data(), payload.size(), resp)) {
                                fileSyncMgr_.handleHashResp(resp);
                            }
                            break;
                        }
                        case PacketType::SYNC_DELTA_BLOCK: {
                            SyncDeltaBlockPayload block;
                            if (deserializeSyncDeltaBlock(payload.data(), payload.size(), block)) {
                                fileSyncMgr_.handleDeltaBlock(block);
                            }
                            break;
                        }
                        case PacketType::SYNC_STATUS_UPDATE: {
                            SyncStatusUpdatePayload status;
                            if (deserializeSyncStatusUpdate(payload.data(), payload.size(), status)) {
                                fileSyncMgr_.handleStatusUpdate(status);
                            }
                            break;
                        }
                        case PacketType::SYNC_ACTION_RESP: {
                            SyncActionRespPayload resp;
                            if (deserializeSyncActionResp(payload.data(), payload.size(), resp)) {
                                fileSyncMgr_.handleActionResp(resp);
                            }
                            break;
                        }
                        case PacketType::VERSION_UPGRADE_NOTICE: {
                            VersionUpgradeNoticePayload notice;
                            if (deserializeVersionUpgradeNotice(payload.data(), payload.size(), notice)) {
                                std::function<void(const VersionUpgradeNoticePayload&)> cb;
                                {
                                    std::lock_guard<std::mutex> lk(peerVersionMutex_);
                                    cb = onVersionUpgradeNotice_;
                                }
                                if (cb) {
                                    cb(notice);
                                }
                            }
                            break;
                        }
                        case PacketType::SCRATCHPAD_UPDATE: {
                            ScratchpadUpdatePayload pad{};
                            if (deserializeScratchpadUpdate(payload.data(), payload.size(), pad)) {
                                if (scratchpadMgr_.handleRemoteUpdate(pad)) {
                                    std::function<void(const ScratchpadUpdatePayload&)> cb;
                                    {
                                        std::lock_guard<std::mutex> lk(scratchpadCbMutex_);
                                        cb = onScratchpadUpdate_;
                                    }
                                    if (cb) cb(pad);
                                }
                            }
                            break;
                        }
                        case PacketType::COMMAND_PALETTE_OUTPUT: {
                            CommandPaletteOutputPayload outPayload{};
                            if (deserializeCommandPaletteOutput(payload.data(), payload.size(), outPayload)) {
                                std::function<void(const CommandPaletteOutputPayload&)> cb;
                                {
                                    std::lock_guard<std::mutex> lk(commandPaletteCbMutex_);
                                    cb = onCommandPaletteOutput_;
                                }
                                if (cb) cb(outPayload);
                            }
                            break;
                        }
                        case PacketType::DISCONNECT:
                            viewerActive_.store(false);
                            break;
                        default:
                            break;
                    }
                } catch (...) {}
            }
        } catch (...) {}

        uintptr_t curS = viewerSock_.exchange(~uintptr_t(0));
        closeWinSock(curS);
        viewerEncrypted_.store(false);
        {
            std::lock_guard<std::mutex> lock(viewerCipherMutex_);
            viewerCipher_.reset();
        }
        {
            std::lock_guard<std::mutex> lk(peerVersionMutex_);
            remotePeerAppVersion_.clear();
        }
        localIsOutdated_.store(false);
        remoteIsOutdated_.store(false);

        if (socketDropped && viewerActive_.load() && running_.load()) {
            isReconnecting = true;
            continue;
        } else {
            break;
        }
    }

    uintptr_t s = viewerSock_.exchange(~uintptr_t(0));
    closeWinSock(s);
    shutdownViewerAudioPlayback();
    stopViewerTunnelMultiplexer();
    viewerPrivacyModeActive_.store(false);
    whiteboardMgr_.clearAllStrokes();
    viewerEncrypted_.store(false);
    {
        std::lock_guard<std::mutex> lock(viewerCipherMutex_);
        viewerCipher_.reset();
        CryptoUtils::secureZero(viewerSessionKey_.data(), viewerSessionKey_.size());
    }
    viewerActive_.store(false);
    fileManager_.abortActiveTransfers();
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        if (viewerStats_.state != ViewerConnectionState::Error) {
            viewerStats_.state = ViewerConnectionState::Disconnected;
            viewerStats_.statusMessage = "Session ended.";
        }
    }
}

// ---------------- Viewer Action Senders ----------------

void NetworkEngine::sendMouseMove(float normX, float normY) {
    float coords[2] = { normX, normY };
    sendViewerEncryptedPacket(PacketType::INPUT_MOUSE_MOVE, 0, coords, sizeof(coords));
}

void NetworkEngine::sendMouseButton(MouseButtonId button, bool isDown, float normX, float normY) {
#pragma pack(push, 1)
    struct MouseButtonPayload {
        uint8_t button;
        uint8_t isDown;
        float normX;
        float normY;
    } p{ static_cast<uint8_t>(button), static_cast<uint8_t>(isDown ? 1 : 0), normX, normY };
#pragma pack(pop)
    sendViewerEncryptedPacket(PacketType::INPUT_MOUSE_BUTTON, 0, &p, sizeof(p));
}

void NetworkEngine::sendMouseWheel(int32_t verticalDelta, int32_t horizontalDelta) {
    int32_t deltas[2] = { verticalDelta, horizontalDelta };
    sendViewerEncryptedPacket(PacketType::INPUT_MOUSE_WHEEL, 0, deltas, sizeof(deltas));
}

void NetworkEngine::sendKeyEvent(uint16_t vkCode, uint16_t scanCode, bool isDown, bool isExtended) {
#pragma pack(push, 1)
    struct KeyEventPayload {
        uint16_t vk;
        uint16_t sc;
        uint8_t isDown;
        uint8_t isExt;
    } p{ vkCode, scanCode, static_cast<uint8_t>(isDown ? 1 : 0), static_cast<uint8_t>(isExtended ? 1 : 0) };
#pragma pack(pop)
    sendViewerEncryptedPacket(PacketType::INPUT_KEY_EVENT, 0, &p, sizeof(p));
}

void NetworkEngine::sendReleaseAllModifiers() {
    sendViewerEncryptedPacket(PacketType::INPUT_RELEASE_ALL, 0, nullptr, 0);
}

void NetworkEngine::sendSystemAction(SystemActionType action) {
    ByteWriter w;
    w.writeU8(static_cast<uint8_t>(action));
    sendViewerEncryptedPacket(PacketType::SYSTEM_ACTION, 0, w.buffer().data(), w.buffer().size());
}

bool NetworkEngine::requestRemoteReboot(bool safeMode, uint32_t countdownSeconds) {
    if (!viewerActive_.load()) return false;
    RemoteRebootRequestPayload req{};
    req.rebootMode = safeMode ? 1 : 0;
    req.countdownSeconds = countdownSeconds;
    std::vector<uint8_t> buf;
    serializeRemoteRebootRequest(req, buf);
    return sendViewerEncryptedPacket(PacketType::REMOTE_REBOOT_REQUEST, 0, buf.data(), buf.size());
}

std::string NetworkEngine::rebootResumeToken() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(rebootTokenMutex_));
    return rebootResumeToken_;
}

void NetworkEngine::cancelAutoReconnection() {
    autoReconnectingWithToken_.store(false);
    rebootPending_.store(false);
    std::lock_guard<std::mutex> lock(rebootTokenMutex_);
    rebootResumeToken_.clear();
}

void NetworkEngine::requestVideoSettings(QualityPreset preset, int monitorIndex, bool forceKeyframe, uint8_t targetFps, int adaptiveFps) {
    uint8_t sendFps = 30;
    uint8_t sendAdap = 1;
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        viewerStats_.qualityPreset = preset;
        if (targetFps > 0) {
            viewerStats_.targetFps = clampTargetFps(targetFps);
        }
        if (adaptiveFps >= 0) {
            viewerStats_.adaptiveFps = (adaptiveFps != 0);
        }
        viewerStats_.effectiveFpsCap = computeAdaptiveFpsCap(
            viewerStats_.targetFps,
            viewerStats_.adaptiveFps,
            viewerStats_.rttMs,
            0.0f
        );
        viewerStats_.networkThrottled = (viewerStats_.effectiveFpsCap < viewerStats_.targetFps);
        viewerStats_.connectionProfile = inferConnectionProfile(
            viewerStats_.qualityPreset,
            viewerStats_.targetFps,
            viewerStats_.adaptiveFps
        );
        sendFps = viewerStats_.targetFps;
        sendAdap = viewerStats_.adaptiveFps ? 1 : 0;
    }
    ByteWriter w;
    w.writeU8(static_cast<uint8_t>(preset));
    w.writeI32(monitorIndex);
    w.writeU8(forceKeyframe ? 1 : 0);
    w.writeU8(sendFps);
    w.writeU8(sendAdap);
    sendViewerEncryptedPacket(PacketType::VIDEO_CONTROL_REQ, 0, w.buffer().data(), w.buffer().size());
}

void NetworkEngine::setSessionFpsConfig(uint8_t targetFps, bool adaptiveFps) {
    QualityPreset curPreset = QualityPreset::Balanced;
    int curMon = 0;
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        curPreset = viewerStats_.qualityPreset;
        curMon = viewerStats_.activeMonitorIndex;
    }
    requestVideoSettings(curPreset, curMon, false, clampTargetFps(targetFps), adaptiveFps ? 1 : 0);
}

void NetworkEngine::selectRemoteMonitor(int monitorIndex) {
    if (viewerSock_.load() == ~uintptr_t(0)) return;
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        viewerStats_.activeMonitorIndex = monitorIndex;
    }
    ByteWriter w;
    w.writeI32(monitorIndex);
    sendViewerEncryptedPacket(PacketType::MONITOR_SELECT, 0, w.buffer().data(), w.buffer().size());
}

bool NetworkEngine::requestCreateVirtualDisplay(uint32_t width, uint32_t height, uint32_t refreshRate, bool preferIddCx) {
    if (viewerSock_.load() == ~uintptr_t(0)) return false;
    VirtualDisplayCmdPayload cmd{};
    cmd.cmd = static_cast<uint8_t>(VirtualDisplayCmdType::Create);
    cmd.displayId = 0;
    cmd.width = (width > 0) ? width : 1920;
    cmd.height = (height > 0) ? height : 1080;
    cmd.refreshRate = (refreshRate > 0) ? refreshRate : 60;
    cmd.flags = preferIddCx ? 0x01 : 0x02;

    std::vector<uint8_t> buf;
    serializeVirtualDisplayCmd(cmd, buf);
    return sendViewerEncryptedPacket(PacketType::VIRTUAL_DISPLAY_CMD, 0, buf.data(), buf.size());
}

bool NetworkEngine::requestDestroyVirtualDisplay(uint32_t displayId) {
    if (viewerSock_.load() == ~uintptr_t(0)) return false;
    VirtualDisplayCmdPayload cmd{};
    cmd.cmd = static_cast<uint8_t>(VirtualDisplayCmdType::Destroy);
    cmd.displayId = displayId;
    cmd.width = 0;
    cmd.height = 0;
    cmd.refreshRate = 0;
    cmd.flags = 0;

    std::vector<uint8_t> buf;
    serializeVirtualDisplayCmd(cmd, buf);
    return sendViewerEncryptedPacket(PacketType::VIRTUAL_DISPLAY_CMD, 0, buf.data(), buf.size());
}

bool NetworkEngine::requestSetVirtualDisplayMode(uint32_t displayId, uint32_t width, uint32_t height, uint32_t refreshRate) {
    if (viewerSock_.load() == ~uintptr_t(0)) return false;
    VirtualDisplayCmdPayload cmd{};
    cmd.cmd = static_cast<uint8_t>(VirtualDisplayCmdType::SetMode);
    cmd.displayId = displayId;
    cmd.width = (width > 0) ? width : 1920;
    cmd.height = (height > 0) ? height : 1080;
    cmd.refreshRate = (refreshRate > 0) ? refreshRate : 60;
    cmd.flags = 0;

    std::vector<uint8_t> buf;
    serializeVirtualDisplayCmd(cmd, buf);
    return sendViewerEncryptedPacket(PacketType::VIRTUAL_DISPLAY_CMD, 0, buf.data(), buf.size());
}

void NetworkEngine::updateQualitySettings(QualityPreset preset, uint8_t targetFps, bool adaptiveFps) {
    if (viewerSock_.load() == ~uintptr_t(0)) return;
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        viewerStats_.qualityPreset = preset;
        viewerStats_.targetFps = clampTargetFps(targetFps);
        viewerStats_.adaptiveFps = adaptiveFps;
        viewerStats_.effectiveFpsCap = computeAdaptiveFpsCap(
            viewerStats_.targetFps,
            viewerStats_.adaptiveFps,
            viewerStats_.rttMs,
            0.0f
        );
        viewerStats_.networkThrottled = (viewerStats_.effectiveFpsCap < viewerStats_.targetFps);
        viewerStats_.connectionProfile = inferConnectionProfile(
            viewerStats_.qualityPreset,
            viewerStats_.targetFps,
            viewerStats_.adaptiveFps
        );
    }
    ByteWriter w;
    w.writeU8(static_cast<uint8_t>(preset));
    w.writeU8(targetFps);
    w.writeU8(adaptiveFps ? 1 : 0);
    sendViewerEncryptedPacket(PacketType::QUALITY_UPDATE, 0, w.buffer().data(), w.buffer().size());
}

void NetworkEngine::applyConnectionProfile(ConnectionProfile profile) {
    QualityPreset qp = QualityPreset::Balanced;
    uint8_t fps = 30;
    bool adaptive = true;
    getProfileSettings(profile, qp, fps, adaptive);

    int activeMon = 0;
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        viewerStats_.connectionProfile = profile;
        viewerStats_.qualityPreset = qp;
        viewerStats_.targetFps = fps;
        viewerStats_.adaptiveFps = adaptive;
        viewerStats_.effectiveFpsCap = computeAdaptiveFpsCap(
            viewerStats_.targetFps,
            viewerStats_.adaptiveFps,
            viewerStats_.rttMs,
            0.0f
        );
        viewerStats_.networkThrottled = (viewerStats_.effectiveFpsCap < viewerStats_.targetFps);
        activeMon = viewerStats_.activeMonitorIndex;
    }

    if (viewerSock_.load() != ~uintptr_t(0)) {
        requestVideoSettings(qp, activeMon, true, fps, adaptive ? 1 : 0);
    }
}

uint32_t NetworkEngine::sendFile(const std::string& filePath, FileOfferTarget targetHint, float dropNx, float dropNy) {
    uintptr_t vSock = viewerSock_.load();
    uintptr_t hSock = activeHostClientSock_.load();

    if (vSock != ~uintptr_t(0)) {
        return fileManager_.startOutgoingFile(filePath, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendViewerEncryptedPacket(pt, 0, buf.data(), buf.size());
        }, targetHint, dropNx, dropNy);
    } else if (hSock != ~uintptr_t(0)) {
        return fileManager_.startOutgoingFile(filePath, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendHostEncryptedPacket(pt, 0, buf.data(), buf.size());
        }, targetHint, dropNx, dropNy);
    }
    return 0;
}

int NetworkEngine::sendDropPath(const std::string& path, FileOfferTarget targetHint, float dropNx, float dropNy) {
    uintptr_t vSock = viewerSock_.load();
    uintptr_t hSock = activeHostClientSock_.load();

    if (vSock != ~uintptr_t(0)) {
        return fileManager_.startOutgoingPath(path, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendViewerEncryptedPacket(pt, 0, buf.data(), buf.size());
        }, targetHint, dropNx, dropNy);
    } else if (hSock != ~uintptr_t(0)) {
        return fileManager_.startOutgoingPath(path, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendHostEncryptedPacket(pt, 0, buf.data(), buf.size());
        }, targetHint, dropNx, dropNy);
    }
    return 0;
}

bool NetworkEngine::cancelFileTransfer(uint32_t transferId) {
    uintptr_t vSock = viewerSock_.load();
    uintptr_t hSock = activeHostClientSock_.load();

    if (vSock != ~uintptr_t(0)) {
        return fileManager_.cancelTransfer(transferId, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendViewerEncryptedPacket(pt, 0, buf.data(), buf.size());
        });
    } else if (hSock != ~uintptr_t(0)) {
        return fileManager_.cancelTransfer(transferId, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendHostEncryptedPacket(pt, 0, buf.data(), buf.size());
        });
    }
    return fileManager_.cancelTransfer(transferId, nullptr);
}

void NetworkEngine::pushLocalClipboardNow() {
    std::string txt = ClipboardManager::getClipboardUtf8();
    if (txt.empty()) return;

    ByteWriter w;
    w.writeString(txt);

    if (viewerSock_.load() != ~uintptr_t(0)) {
        sendViewerEncryptedPacket(PacketType::CLIPBOARD_TEXT, 0, w.buffer().data(), w.buffer().size());
    }
    if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        sendHostEncryptedPacket(PacketType::CLIPBOARD_TEXT, 0, w.buffer().data(), w.buffer().size());
    }
}

bool NetworkEngine::requestRemoteScan(const std::string& remotePath) {
    fileSyncMgr_.setRemoteRoot(remotePath);
    SyncScanReqPayload req;
    req.scanId = static_cast<uint32_t>(GetTickCount64());
    req.rootPath = remotePath;
    req.flags = 0x01; // Recursive
    std::vector<uint8_t> buf;
    serializeSyncScanReq(req, buf);
    if (viewerSock_.load() != ~uintptr_t(0)) {
        return sendViewerEncryptedPacket(PacketType::SYNC_SCAN_REQ, 0, buf.data(), buf.size());
    } else if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        return sendHostEncryptedPacket(PacketType::SYNC_SCAN_REQ, 0, buf.data(), buf.size());
    }
    return false;
}

bool NetworkEngine::requestRemoteBlockHashes(const std::string& relativePath) {
    SyncHashReqPayload req;
    req.syncId = 1;
    req.fileId = 1;
    req.relativePath = relativePath;
    std::vector<uint8_t> buf;
    serializeSyncHashReq(req, buf);
    if (viewerSock_.load() != ~uintptr_t(0)) {
        return sendViewerEncryptedPacket(PacketType::SYNC_HASH_REQ, 0, buf.data(), buf.size());
    } else if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        return sendHostEncryptedPacket(PacketType::SYNC_HASH_REQ, 0, buf.data(), buf.size());
    }
    return false;
}

bool NetworkEngine::startFolderSync(const std::string& localPath, const std::string& remotePath, SyncMode mode, bool purge) {
    fileSyncMgr_.setLocalRoot(localPath);
    fileSyncMgr_.setRemoteRoot(remotePath);
    fileSyncMgr_.setSyncMode(mode);
    fileSyncMgr_.setMirrorPurge(purge);
    return fileSyncMgr_.startSync();
}

void NetworkEngine::cancelFolderSync() {
    fileSyncMgr_.cancelSync();
    SyncActionReqPayload req;
    req.actionId = static_cast<uint32_t>(GetTickCount64());
    req.actionType = static_cast<uint8_t>(SyncActionType::AbortSync);
    std::vector<uint8_t> buf;
    serializeSyncActionReq(req, buf);
    if (viewerSock_.load() != ~uintptr_t(0)) {
        sendViewerEncryptedPacket(PacketType::SYNC_ACTION_REQ, 0, buf.data(), buf.size());
    } else if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        sendHostEncryptedPacket(PacketType::SYNC_ACTION_REQ, 0, buf.data(), buf.size());
    }
}

bool NetworkEngine::sendChatMessage(const std::string& text) {
    if (text.empty() || text.size() > 2048) return false;

    ByteWriter w;
    w.writeString(identity_.hostname());
    w.writeString(text);

    bool sent = false;
    if (viewerSock_.load() != ~uintptr_t(0)) {
        if (sendViewerEncryptedPacket(PacketType::CHAT_MESSAGE, 0, w.buffer().data(), w.buffer().size())) {
            sent = true;
        }
    }
    if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        if (sendHostEncryptedPacket(PacketType::CHAT_MESSAGE, 0, w.buffer().data(), w.buffer().size())) {
            sent = true;
        }
    }

    if (sent) {
        std::lock_guard<std::mutex> lock(chatMutex_);
        chatHistory_.push_back({ "You (" + identity_.hostname() + ")", text, true, nowTickMs() });
        if (chatHistory_.size() > 100) chatHistory_.erase(chatHistory_.begin());
    }
    return sent;
}

bool NetworkEngine::sendChatImage(const std::vector<uint8_t>& jpegData, uint32_t w, uint32_t h, const std::string& caption) {
    if (jpegData.empty() || jpegData.size() > 2 * 1024 * 1024) return false;

    ChatMediaPayload p;
    p.senderName = identity_.hostname();
    p.captionText = caption;
    p.imgWidth = w;
    p.imgHeight = h;
    p.timestampMs = nowTickMs();
    p.jpegData = jpegData;

    std::vector<uint8_t> payload;
    serializeChatMedia(p, payload);

    bool sent = false;
    if (viewerSock_.load() != ~uintptr_t(0)) {
        if (sendViewerEncryptedPacket(PacketType::CHAT_MEDIA_MESSAGE, 0, payload.data(), payload.size())) {
            sent = true;
        }
    }
    if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        if (sendHostEncryptedPacket(PacketType::CHAT_MEDIA_MESSAGE, 0, payload.data(), payload.size())) {
            sent = true;
        }
    }

    if (sent) {
        std::lock_guard<std::mutex> lock(chatMutex_);
        chatHistory_.push_back({ "You (" + identity_.hostname() + ")", caption, true, p.timestampMs, true, jpegData, w, h });
        if (chatHistory_.size() > 100) chatHistory_.erase(chatHistory_.begin());
    }
    return sent;
}

std::vector<ChatMessageEntry> NetworkEngine::chatMessages() const {
    std::lock_guard<std::mutex> lock(chatMutex_);
    return chatHistory_;
}

// ---------------- Audio Streaming & Privacy Screen Controls (v2.1.0) ----------------

void NetworkEngine::setAudioVolume(int percent) {
    audioVolumePercent_.store(std::clamp(percent, 0, 100));
    syncAudioControl();
}

int NetworkEngine::audioVolume() const {
    return audioVolumePercent_.load();
}

void NetworkEngine::setAudioMuted(bool muted) {
    audioMuted_.store(muted);
    syncAudioControl();
}

bool NetworkEngine::isAudioMuted() const {
    return audioMuted_.load();
}

void NetworkEngine::syncAudioControl() {
    AudioControlPayload p{};
    p.isMuted = audioMuted_.load() ? 1 : 0;
    p.volumePercent = static_cast<uint8_t>(std::clamp(audioVolumePercent_.load(), 0, 100));
    p.reserved[0] = 0;
    p.reserved[1] = 0;
    sendViewerEncryptedPacket(PacketType::AUDIO_CONTROL, 0, &p, sizeof(p));
}

bool NetworkEngine::isHostAudioSuspended() const {
    return hostAudioSuspended_.load();
}

void NetworkEngine::configurePrivacyCurtain(const std::string& notice, const std::string& brand, bool showId) {
    privacyCurtainNotice_ = notice;
    privacyCurtainBrand_ = brand;
    privacyCurtainShowId_ = showId;
}

void NetworkEngine::requestTogglePrivacyMode(ScreenBlankMode blankMode) {
    bool current = viewerPrivacyModeActive_.load();
    const auto& s = identity_.settings();
    if (!s.hardwareDpmsBlanking && blankMode == ScreenBlankMode::Unified) {
        blankMode = ScreenBlankMode::CurtainOnly;
    }

    std::string notice = !privacyCurtainNotice_.empty() ? privacyCurtainNotice_ : s.privacyCustomNotice;
    std::string brand = !privacyCurtainBrand_.empty() ? privacyCurtainBrand_ : s.privacyBrandName;
    PrivacyModeConfigPayload p{};
    p.enable = current ? 0 : 1;
    p.acknowledge = 0;
    p.showDeskId = privacyCurtainShowId_ ? 1 : 0;
    p.blankMode = static_cast<uint8_t>(blankMode);
    std::snprintf(p.customNotice, sizeof(p.customNotice), "%s", notice.c_str());
    std::snprintf(p.brandName, sizeof(p.brandName), "%s", brand.c_str());
    ByteWriter w;
    w.writeBytes(&p, sizeof(p));
    sendViewerEncryptedPacket(PacketType::PRIVACY_MODE_TOGGLE, 0, w.buffer().data(), w.buffer().size());
}

bool NetworkEngine::isPrivacyModeEngaged() const {
    return viewerPrivacyModeActive_.load();
}

bool NetworkEngine::isHostPrivacyModeActive() const {
    return ScreenBlankManager::instance().isBlankActive() || hostPrivacyModeActive_.load();
}

void NetworkEngine::setHostPrivacyMode(bool enable, const std::string& notice, const std::string& brand, bool showId, ScreenBlankMode blankMode) {
    if (enable) {
        if (!notice.empty()) privacyCurtainNotice_ = notice;
        else if (privacyCurtainNotice_.empty()) privacyCurtainNotice_ = identity_.settings().privacyCustomNotice;

        if (!brand.empty()) privacyCurtainBrand_ = brand;
        else if (privacyCurtainBrand_.empty()) privacyCurtainBrand_ = identity_.settings().privacyBrandName;

        privacyCurtainShowId_ = showId;

        // Auto-provision virtual display if host is headless
        if ((blankMode == ScreenBlankMode::DpmsOnly || blankMode == ScreenBlankMode::Unified) &&
            identity_.settings().autoVirtualDisplay && VirtualDisplayManager::isHostHeadless()) {
            VirtualDisplayManager::instance().ensureHeadlessDisplay(1920, 1080, 60);
        }

        ScreenBlankManager::instance().engageBlanking(blankMode, privacyCurtainNotice_, privacyCurtainBrand_, privacyCurtainShowId_, identity_.deskId());
        hostPrivacyModeActive_.store(true);
    } else {
        ScreenBlankManager::instance().disengageBlanking();
        destroyPrivacyCurtainWindow();
        hostPrivacyModeActive_.store(false);
    }
}

void NetworkEngine::createPrivacyCurtainWindow() {
    if (hwndPrivacyCurtain_) return;

    // Populate global curtain configuration for double-buffered GDI window
    std::string activeBrand = privacyCurtainBrand_.empty() ? identity_.settings().privacyBrandName : privacyCurtainBrand_;
    std::string activeNotice = privacyCurtainNotice_.empty() ? identity_.settings().privacyCustomNotice : privacyCurtainNotice_;
    s_curtainConfig.brandName = utf8ToWide(activeBrand.empty() ? "CppDesk Enterprise Security" : activeBrand);
    s_curtainConfig.noticeText = utf8ToWide(activeNotice.empty() ? "Screen output hidden and local physical inputs secured for authorized administration." : activeNotice);
    s_curtainConfig.showDeskId = privacyCurtainShowId_;

    uint64_t deskId = identity_.deskId();
    if (deskId > 0) {
        std::string rawId = std::to_string(deskId);
        std::string fmtId;
        for (size_t i = 0; i < rawId.size(); ++i) {
            if (i > 0 && (rawId.size() - i) % 3 == 0) fmtId += "-";
            fmtId += rawId[i];
        }
        s_curtainConfig.deskIdText = L"Workstation Desk ID: " + utf8ToWide(fmtId);
    } else {
        s_curtainConfig.deskIdText = L"";
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PrivacyCurtainWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CppDeskPrivacyCurtainClass";
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = std::max<int>(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    int vh = std::max<int>(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    hwndPrivacyCurtain_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"CppDeskPrivacyCurtainClass",
        L"CppDesk Privacy Curtain",
        WS_POPUP,
        vx, vy, vw, vh,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );

    if (hwndPrivacyCurtain_) {
        SetWindowDisplayAffinity(hwndPrivacyCurtain_, WDA_EXCLUDEFROMCAPTURE);
        ShowWindow(hwndPrivacyCurtain_, SW_SHOWMAXIMIZED);
        SetWindowPos(hwndPrivacyCurtain_, HWND_TOPMOST, vx, vy, vw, vh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        UpdateWindow(hwndPrivacyCurtain_);
    }

    BlockInput(TRUE);
    hostPrivacyModeActive_.store(true);
}

void NetworkEngine::destroyPrivacyCurtainWindow() {
    BlockInput(FALSE);
    if (hwndPrivacyCurtain_) {
        DestroyWindow(hwndPrivacyCurtain_);
        hwndPrivacyCurtain_ = nullptr;
    }
    ScreenBlankManager::instance().disengageBlanking();
    hostPrivacyModeActive_.store(false);
}

void NetworkEngine::startHostAudioCapture() {
    stopHostAudioCapture();
    hostAudioActive_.store(true);
    hostAudioThread_ = std::thread(&NetworkEngine::hostAudioCaptureLoop, this);
}

void NetworkEngine::stopHostAudioCapture() {
    hostAudioActive_.store(false);
    try {
        if (hostAudioThread_.joinable()) {
            hostAudioThread_.join();
        }
    } catch (...) {}
}

void NetworkEngine::hostAudioCaptureLoop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coInited = SUCCEEDED(hr);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    WAVEFORMATEX* pwfx = nullptr;

    auto cleanupCom = [&]() {
        if (pwfx) { CoTaskMemFree(pwfx); pwfx = nullptr; }
        if (pCaptureClient) { pCaptureClient->Release(); pCaptureClient = nullptr; }
        if (pAudioClient) { pAudioClient->Stop(); pAudioClient->Release(); pAudioClient = nullptr; }
        if (pDevice) { pDevice->Release(); pDevice = nullptr; }
        if (pEnumerator) { pEnumerator->Release(); pEnumerator = nullptr; }
        if (coInited) { CoUninitialize(); }
    };

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator_val, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator_val, (void**)&pEnumerator);
    if (FAILED(hr)) { cleanupCom(); return; }

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr)) { cleanupCom(); return; }

    hr = pDevice->Activate(IID_IAudioClient_val, CLSCTX_ALL, nullptr, (void**)&pAudioClient);
    if (FAILED(hr)) { cleanupCom(); return; }

    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) { cleanupCom(); return; }

    REFERENCE_TIME hnsBufferDuration = 1000000; // 100ms
    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        hnsBufferDuration,
        0,
        pwfx,
        nullptr
    );
    if (FAILED(hr)) { cleanupCom(); return; }

    hr = pAudioClient->GetService(IID_IAudioCaptureClient_val, (void**)&pCaptureClient);
    if (FAILED(hr)) { cleanupCom(); return; }

    hr = pAudioClient->Start();
    if (FAILED(hr)) { cleanupCom(); return; }

    bool isFloat = false;
    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* pExt = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        if (std::memcmp(&pExt->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_val, sizeof(GUID)) == 0) {
            isFloat = true;
        }
    }

    uint32_t srcSampleRate = pwfx->nSamplesPerSec;
    WORD srcChannels = pwfx->nChannels;

    std::vector<int16_t> convertedPcm;
    std::vector<int16_t> resampledPcm;
    std::vector<uint8_t> audioPacketScratch;
    audioPacketScratch.reserve(sizeof(AudioChunkHeader) + 4800 * sizeof(int16_t));

    while (hostAudioActive_.load() && running_.load() && activeHostClientSock_.load() != ~uintptr_t(0)) {
        UINT32 packetLength = 0;
        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        if (packetLength == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }

        BYTE* pData = nullptr;
        UINT32 numFramesRead = 0;
        DWORD flags = 0;

        hr = pCaptureClient->GetBuffer(&pData, &numFramesRead, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        if (numFramesRead > 0) {
            if (hostAudioSuspended_.load()) {
                pCaptureClient->ReleaseBuffer(numFramesRead);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                AudioChunkHeader hdr{};
                hdr.sampleRate = 48000;
                hdr.channels = 2;
                hdr.bitsPerSample = 16;
                hdr.isSilent = 1;
                hdr.sampleFrames = numFramesRead;

                sendHostEncryptedPacket(PacketType::AUDIO_STREAM_CHUNK, 0, &hdr, sizeof(hdr));
            } else if (pData) {
                convertedPcm.resize(numFramesRead * 2);
                if (isFloat) {
                    const float* fData = reinterpret_cast<const float*>(pData);
                    for (UINT32 f = 0; f < numFramesRead; ++f) {
                        float left = fData[f * srcChannels];
                        float right = (srcChannels > 1) ? fData[f * srcChannels + 1] : left;
                        convertedPcm[f * 2 + 0] = static_cast<int16_t>(std::clamp(left, -1.0f, 1.0f) * 32767.0f);
                        convertedPcm[f * 2 + 1] = static_cast<int16_t>(std::clamp(right, -1.0f, 1.0f) * 32767.0f);
                    }
                } else if (pwfx->wBitsPerSample == 16) {
                    const int16_t* sData = reinterpret_cast<const int16_t*>(pData);
                    for (UINT32 f = 0; f < numFramesRead; ++f) {
                        int16_t left = sData[f * srcChannels];
                        int16_t right = (srcChannels > 1) ? sData[f * srcChannels + 1] : left;
                        convertedPcm[f * 2 + 0] = left;
                        convertedPcm[f * 2 + 1] = right;
                    }
                }

                const int16_t* pcmToSend = convertedPcm.data();
                uint32_t finalFrames = numFramesRead;

                if (srcSampleRate != 48000 && srcSampleRate > 0) {
                    finalFrames = static_cast<uint32_t>((static_cast<uint64_t>(numFramesRead) * 48000) / srcSampleRate);
                    if (finalFrames > 0) {
                        resampledPcm.resize(finalFrames * 2);
                        for (uint32_t i = 0; i < finalFrames; ++i) {
                            double srcPos = static_cast<double>(i) * srcSampleRate / 48000.0;
                            size_t idx0 = static_cast<size_t>(srcPos);
                            size_t idx1 = std::min(idx0 + 1, static_cast<size_t>(numFramesRead - 1));
                            float frac = static_cast<float>(srcPos - idx0);

                            float l0 = convertedPcm[idx0 * 2 + 0];
                            float l1 = convertedPcm[idx1 * 2 + 0];
                            float r0 = convertedPcm[idx0 * 2 + 1];
                            float r1 = convertedPcm[idx1 * 2 + 1];

                            resampledPcm[i * 2 + 0] = static_cast<int16_t>((1.0f - frac) * l0 + frac * l1);
                            resampledPcm[i * 2 + 1] = static_cast<int16_t>((1.0f - frac) * r0 + frac * r1);
                        }
                        pcmToSend = resampledPcm.data();
                    }
                }

                if (finalFrames > 0) {
                    AudioChunkHeader hdr{};
                    hdr.sampleRate = 48000;
                    hdr.channels = 2;
                    hdr.bitsPerSample = 16;
                    hdr.isSilent = 0;
                    hdr.sampleFrames = finalFrames;

                    const size_t pcmBytes = static_cast<size_t>(finalFrames) * 2 * sizeof(int16_t);
                    audioPacketScratch.resize(sizeof(hdr) + pcmBytes);
                    std::memcpy(audioPacketScratch.data(), &hdr, sizeof(hdr));
                    std::memcpy(audioPacketScratch.data() + sizeof(hdr), pcmToSend, pcmBytes);
                    sendHostEncryptedPacket(PacketType::AUDIO_STREAM_CHUNK, 0, audioPacketScratch.data(), audioPacketScratch.size());
                }
            }
        }

        pCaptureClient->ReleaseBuffer(numFramesRead);
    }

    cleanupCom();
}

void NetworkEngine::initViewerAudioPlayback() {
    std::lock_guard<std::mutex> lk(audioPlaybackMutex_);
    if (hWaveOut_) return;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 48000;
    wfx.nAvgBytesPerSec = 48000 * 2 * sizeof(int16_t);
    wfx.nBlockAlign = 2 * sizeof(int16_t);
    wfx.wBitsPerSample = 16;
    wfx.cbSize = 0;

    MMRESULT res = waveOutOpen(&hWaveOut_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (res != MMSYSERR_NOERROR) {
        hWaveOut_ = nullptr;
        return;
    }

    for (size_t i = 0; i < 3; ++i) {
        waveBuffers_[i].assign(8192, 0);
        std::memset(&waveHeaders_[i], 0, sizeof(WAVEHDR));
        waveHeaders_[i].lpData = reinterpret_cast<LPSTR>(waveBuffers_[i].data());
        waveHeaders_[i].dwBufferLength = static_cast<DWORD>(waveBuffers_[i].size());
    }
    currentWaveIdx_ = 0;
}

void NetworkEngine::shutdownViewerAudioPlayback() {
    std::lock_guard<std::mutex> lk(audioPlaybackMutex_);
    if (!hWaveOut_) return;

    waveOutReset(hWaveOut_);
    for (size_t i = 0; i < 3; ++i) {
        if (waveHeaders_[i].dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(hWaveOut_, &waveHeaders_[i], sizeof(WAVEHDR));
        }
        waveHeaders_[i].dwFlags = 0;
    }
    waveOutClose(hWaveOut_);
    hWaveOut_ = nullptr;
}

void NetworkEngine::enqueueViewerAudioChunk(const uint8_t* payload, size_t len) {
    if (!hWaveOut_ || len < sizeof(AudioChunkHeader)) return;
    if (audioMuted_.load()) return;
    int volPercent = audioVolumePercent_.load();
    if (volPercent <= 0) return;

    AudioChunkHeader hdr{};
    std::memcpy(&hdr, payload, sizeof(hdr));
    if (hdr.isSilent || hdr.sampleFrames == 0) return;

    size_t pcmBytes = len - sizeof(AudioChunkHeader);
    if (pcmBytes == 0) return;

    const int16_t* inSamples = reinterpret_cast<const int16_t*>(payload + sizeof(AudioChunkHeader));
    size_t sampleCount = pcmBytes / sizeof(int16_t);

    std::lock_guard<std::mutex> lk(audioPlaybackMutex_);
    if (!hWaveOut_) return;

    WAVEHDR& curHdr = waveHeaders_[currentWaveIdx_];
    if (curHdr.dwFlags & WHDR_PREPARED) {
        if (!(curHdr.dwFlags & WHDR_DONE)) {
            return;
        }
        waveOutUnprepareHeader(hWaveOut_, &curHdr, sizeof(WAVEHDR));
        curHdr.dwFlags = 0;
    }

    if (waveBuffers_[currentWaveIdx_].size() < pcmBytes) {
        waveBuffers_[currentWaveIdx_].resize(pcmBytes);
    }

    int16_t* outSamples = reinterpret_cast<int16_t*>(waveBuffers_[currentWaveIdx_].data());
    if (volPercent == 100) {
        std::memcpy(outSamples, inSamples, pcmBytes);
    } else {
        float volScale = volPercent / 100.0f;
        for (size_t i = 0; i < sampleCount; ++i) {
            float s = static_cast<float>(inSamples[i]) * volScale;
            outSamples[i] = static_cast<int16_t>(std::clamp(s, -32768.0f, 32767.0f));
        }
    }

    curHdr.lpData = reinterpret_cast<LPSTR>(outSamples);
    curHdr.dwBufferLength = static_cast<DWORD>(pcmBytes);
    curHdr.dwFlags = 0;

    if (waveOutPrepareHeader(hWaveOut_, &curHdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
        waveOutWrite(hWaveOut_, &curHdr, sizeof(WAVEHDR));
        currentWaveIdx_ = (currentWaveIdx_ + 1) % 3;
    }
}

// ---------------- Bidirectional Voice Intercom (Feature 4) ----------------

bool NetworkEngine::startVoiceIntercom() {
    voiceIntercom_.startPlayback(48000, 1);

    return voiceIntercom_.startCapture([this](const uint8_t* pcm, size_t bytes, uint32_t sampleRate, uint8_t channels) {
        uint8_t safeCh = std::max<uint8_t>(1, channels);
        VoiceChunkHeader hdr{};
        hdr.sampleRate = sampleRate;
        hdr.channels = safeCh;
        hdr.bitsPerSample = 16;
        hdr.flags = 0x00;
        hdr.sampleFrames = static_cast<uint32_t>(bytes / (safeCh * sizeof(int16_t)));

        thread_local std::vector<uint8_t> tl_voicePacketScratch;
        tl_voicePacketScratch.resize(sizeof(hdr) + bytes);
        std::memcpy(tl_voicePacketScratch.data(), &hdr, sizeof(hdr));
        if (bytes > 0 && pcm) {
            std::memcpy(tl_voicePacketScratch.data() + sizeof(hdr), pcm, bytes);
        }

        if (viewerActive_.load()) {
            sendViewerEncryptedPacket(PacketType::VOICE_INTERCOM_CHUNK, 0, tl_voicePacketScratch.data(), tl_voicePacketScratch.size());
        } else if (activeHostClientSock_.load() != ~uintptr_t(0)) {
            sendHostEncryptedPacket(PacketType::VOICE_INTERCOM_CHUNK, 0, tl_voicePacketScratch.data(), tl_voicePacketScratch.size());
        }
    }, 48000, 1);
}

void NetworkEngine::stopVoiceIntercom() {
    voiceIntercom_.stopCapture();
    voiceIntercom_.stopPlayback();
}

bool NetworkEngine::isVoiceIntercomActive() const {
    return voiceIntercom_.isCapturing();
}

void NetworkEngine::setVoiceIntercomMicMuted(bool muted) {
    voiceIntercom_.setMicMuted(muted);
}

bool NetworkEngine::isVoiceIntercomMicMuted() const {
    return voiceIntercom_.isMicMuted();
}

float NetworkEngine::voiceIntercomInputLevel() const {
    return voiceIntercom_.inputLevel();
}

void NetworkEngine::handleIncomingVoiceChunk(const uint8_t* payload, size_t len) {
    if (!payload || len < sizeof(VoiceChunkHeader)) return;
    const auto* hdr = reinterpret_cast<const VoiceChunkHeader*>(payload);
    const uint8_t* pcm = payload + sizeof(VoiceChunkHeader);
    size_t pcmBytes = len - sizeof(VoiceChunkHeader);
    if (pcmBytes == 0) return;

    uint32_t srate = (hdr->sampleRate >= 8000 && hdr->sampleRate <= 96000) ? hdr->sampleRate : 48000;
    uint8_t ch = (hdr->channels == 1 || hdr->channels == 2) ? hdr->channels : 1;

    if (!voiceIntercom_.isPlaying()) {
        voiceIntercom_.startPlayback(srate, ch);
    }
    voiceIntercom_.enqueuePlaybackChunk(pcm, pcmBytes);
}

// ---------------- Remote Terminal & TCP Port Forwarding (v2.1.0) ----------------

void NetworkEngine::sendTerminalCommand(const std::string& cmd) {
    if (cmd.empty()) return;
    {
        std::lock_guard<std::mutex> lk(terminalMutex_);
        terminalHistory_.push_back(cmd);
        if (terminalHistory_.size() > 50) terminalHistory_.erase(terminalHistory_.begin());
        terminalLines_.push_back("> " + cmd);
        if (terminalLines_.size() > 500) terminalLines_.erase(terminalLines_.begin());
    }

    ByteWriter w;
    w.writeU8(static_cast<uint8_t>(TerminalStreamKind::StdinInput));
    w.writeU32(static_cast<uint32_t>(cmd.size()));
    w.writeBytes(cmd.data(), cmd.size());
    sendViewerEncryptedPacket(PacketType::TERMINAL_DATA, 0, w.buffer().data(), w.buffer().size());
}

void NetworkEngine::resetRemoteTerminal(bool usePowerShell) {
    ByteWriter w;
    w.writeU8(static_cast<uint8_t>(usePowerShell ? TerminalStreamKind::SwitchShell : TerminalStreamKind::ResetShell));
    w.writeU32(0);
    sendViewerEncryptedPacket(PacketType::TERMINAL_DATA, 0, w.buffer().data(), w.buffer().size());
}

std::vector<std::string> NetworkEngine::getTerminalScrollback() const {
    std::lock_guard<std::mutex> lk(terminalMutex_);
    return terminalLines_;
}

void NetworkEngine::clearTerminalScrollback() {
    std::lock_guard<std::mutex> lk(terminalMutex_);
    terminalLines_.clear();
}

void NetworkEngine::sendWhiteboardStroke(const AnnotationStroke& stroke) {
    auto payload = WhiteboardManager::serializeStroke(stroke);
    if (viewerSock_.load() != ~uintptr_t(0)) {
        sendViewerEncryptedPacket(PacketType::WHITEBOARD_PACKET, 0, payload.data(), payload.size());
    } else if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        sendHostEncryptedPacket(PacketType::WHITEBOARD_PACKET, 0, payload.data(), payload.size());
    }
}

void NetworkEngine::sendWhiteboardClear() {
    auto payload = WhiteboardManager::serializeClearPacket();
    if (viewerSock_.load() != ~uintptr_t(0)) {
        sendViewerEncryptedPacket(PacketType::WHITEBOARD_PACKET, 0, payload.data(), payload.size());
    } else if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        sendHostEncryptedPacket(PacketType::WHITEBOARD_PACKET, 0, payload.data(), payload.size());
    }
}

void NetworkEngine::sendWhiteboardLaser(float normX, float normY) {
    AnnotationStroke stroke{};
    stroke.tool = WhiteboardTool::LaserPointer;
    stroke.points.push_back({ normX, normY });
    auto payload = WhiteboardManager::serializeStroke(stroke);
    if (viewerSock_.load() != ~uintptr_t(0)) {
        sendViewerEncryptedPacket(PacketType::WHITEBOARD_PACKET, 0, payload.data(), payload.size());
    } else if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        sendHostEncryptedPacket(PacketType::WHITEBOARD_PACKET, 0, payload.data(), payload.size());
    }
}

void NetworkEngine::startHostTerminal(bool usePowerShell) {
    stopHostTerminal();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hStdinRead = nullptr, hStdinWrite = nullptr;
    HANDLE hStdoutRead = nullptr, hStdoutWrite = nullptr;

    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) return;
    if (!SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hStdinRead); CloseHandle(hStdinWrite);
        return;
    }

    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        CloseHandle(hStdinRead); CloseHandle(hStdinWrite);
        return;
    }
    if (!SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hStdinRead); CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        return;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStdoutWrite;

    PROCESS_INFORMATION pi{};
    wchar_t cmdLine[256];
    if (usePowerShell) {
        wcscpy_s(cmdLine, L"powershell.exe -NoLogo -NoExit");
    } else {
        wcscpy_s(cmdLine, L"cmd.exe /Q /K");
    }

    BOOL created = CreateProcessW(
        nullptr, cmdLine, nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi
    );

    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);

    if (!created) {
        CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead);
        return;
    }

    hChildStdinWrite_ = hStdinWrite;
    hChildStdoutRead_ = hStdoutRead;
    hChildProcess_ = pi.hProcess;
    hChildThread_ = pi.hThread;

    hostTerminalActive_.store(true);
    hostTerminalThread_ = std::thread(&NetworkEngine::hostTerminalReaderLoop, this);
}

void NetworkEngine::stopHostTerminal() {
    hostTerminalActive_.store(false);
    try {
        if (hostTerminalThread_.joinable()) {
            hostTerminalThread_.join();
        }
    } catch (...) {}
    if (hChildProcess_) {
        TerminateProcess(hChildProcess_, 0);
        CloseHandle(hChildProcess_);
        hChildProcess_ = nullptr;
    }
    if (hChildThread_) {
        CloseHandle(hChildThread_);
        hChildThread_ = nullptr;
    }
    if (hChildStdinWrite_) {
        CloseHandle(hChildStdinWrite_);
        hChildStdinWrite_ = nullptr;
    }
    if (hChildStdoutRead_) {
        CloseHandle(hChildStdoutRead_);
        hChildStdoutRead_ = nullptr;
    }
}

void NetworkEngine::injectHostTerminalStdin(const std::string& input) {
    if (!hChildStdinWrite_) return;
    std::string toWrite = input + "\r\n";
    DWORD written = 0;
    WriteFile(hChildStdinWrite_, toWrite.data(), static_cast<DWORD>(toWrite.size()), &written, nullptr);
}

void NetworkEngine::hostTerminalReaderLoop() {
    char buf[4096];
    while (hostTerminalActive_.load() && running_.load() && activeHostClientSock_.load() != ~uintptr_t(0)) {
        if (!hChildStdoutRead_) break;
        DWORD avail = 0;
        if (PeekNamedPipe(hChildStdoutRead_, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD toRead = std::min<DWORD>(avail, sizeof(buf));
            DWORD numRead = 0;
            if (ReadFile(hChildStdoutRead_, buf, toRead, &numRead, nullptr) && numRead > 0) {
                ByteWriter w;
                w.writeU8(static_cast<uint8_t>(TerminalStreamKind::StdoutChunk));
                w.writeU32(numRead);
                w.writeBytes(buf, numRead);
                sendHostEncryptedPacket(PacketType::TERMINAL_DATA, 0, w.buffer().data(), w.buffer().size());
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

uint32_t NetworkEngine::addPortForwardRule(uint16_t localPort, uint16_t targetPort, const std::string& desc, bool startActive) {
    std::lock_guard<std::mutex> lk(tunnelMutex_);
    PortForwardRule r;
    r.ruleId = nextRuleId_++;
    r.localPort = localPort;
    r.targetPort = targetPort;
    r.description = desc;
    r.active = startActive;
    tunnelRules_.push_back(r);

    if (startActive && viewerActive_.load()) {
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls != INVALID_SOCKET) {
            u_long nonblock = 1;
            ioctlsocket(ls, FIONBIO, &nonblock);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(localPort);
            if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                listen(ls, 5) == 0) {
                tunnelListeners_.push_back({ r.ruleId, localPort, targetPort, ls });
            } else {
                closesocket(ls);
            }
        }
    }
    return r.ruleId;
}

void NetworkEngine::removePortForwardRule(uint32_t ruleId) {
    std::lock_guard<std::mutex> lk(tunnelMutex_);
    for (auto it = tunnelListeners_.begin(); it != tunnelListeners_.end(); ++it) {
        if (it->ruleId == ruleId) {
            closesocket(it->listenSock);
            tunnelListeners_.erase(it);
            break;
        }
    }
    for (auto it = tunnelRules_.begin(); it != tunnelRules_.end(); ++it) {
        if (it->ruleId == ruleId) {
            tunnelRules_.erase(it);
            break;
        }
    }
}

void NetworkEngine::setPortForwardRuleActive(uint32_t ruleId, bool active) {
    std::lock_guard<std::mutex> lk(tunnelMutex_);
    for (auto& r : tunnelRules_) {
        if (r.ruleId == ruleId) {
            r.active = active;
            if (!active) {
                for (auto it = tunnelListeners_.begin(); it != tunnelListeners_.end(); ++it) {
                    if (it->ruleId == ruleId) {
                        closesocket(it->listenSock);
                        tunnelListeners_.erase(it);
                        break;
                    }
                }
            } else if (viewerActive_.load()) {
                SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (ls != INVALID_SOCKET) {
                    u_long nonblock = 1;
                    ioctlsocket(ls, FIONBIO, &nonblock);
                    sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    addr.sin_port = htons(r.localPort);
                    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                        listen(ls, 5) == 0) {
                        tunnelListeners_.push_back({ r.ruleId, r.localPort, r.targetPort, ls });
                    } else {
                        closesocket(ls);
                    }
                }
            }
            break;
        }
    }
}

std::vector<PortForwardRule> NetworkEngine::portForwardRules() const {
    std::lock_guard<std::mutex> lk(tunnelMutex_);
    return tunnelRules_;
}

void NetworkEngine::startPortForwarding() {
    startViewerTunnelMultiplexer();
}

void NetworkEngine::stopPortForwarding() {
    stopViewerTunnelMultiplexer();
    stopHostTunnelProxy();
}

void NetworkEngine::startViewerTunnelMultiplexer() {
    stopViewerTunnelMultiplexer();
    viewerTunnelActive_.store(true);
    viewerTunnelThread_ = std::thread(&NetworkEngine::viewerTunnelMultiplexerLoop, this);
}

void NetworkEngine::stopViewerTunnelMultiplexer() {
    viewerTunnelActive_.store(false);
    try {
        if (viewerTunnelThread_.joinable()) {
            viewerTunnelThread_.join();
        }
    } catch (...) {}
    std::lock_guard<std::mutex> lk(tunnelMutex_);
    for (auto& l : tunnelListeners_) {
        closesocket(l.listenSock);
    }
    tunnelListeners_.clear();
    for (auto& kv : viewerTunnels_) {
        closesocket(kv.second.sock);
    }
    viewerTunnels_.clear();
}

void NetworkEngine::viewerTunnelMultiplexerLoop() {
    {
        std::lock_guard<std::mutex> lk(tunnelMutex_);
        for (const auto& r : tunnelRules_) {
            if (r.active) {
                SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (ls != INVALID_SOCKET) {
                    u_long nonblock = 1;
                    ioctlsocket(ls, FIONBIO, &nonblock);
                    sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    addr.sin_port = htons(r.localPort);
                    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                        listen(ls, 5) == 0) {
                        tunnelListeners_.push_back({ r.ruleId, r.localPort, r.targetPort, ls });
                    } else {
                        closesocket(ls);
                    }
                }
            }
        }
    }

    std::vector<uint8_t> readBuf(16384);

    while (viewerTunnelActive_.load() && running_.load() && viewerActive_.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        SOCKET maxSock = 0;

        std::vector<ListenerState> currentListeners;
        std::vector<ActiveTunnel> currentTunnels;
        {
            std::lock_guard<std::mutex> lk(tunnelMutex_);
            currentListeners = tunnelListeners_;
            for (const auto& kv : viewerTunnels_) {
                currentTunnels.push_back(kv.second);
            }
        }

        for (const auto& l : currentListeners) {
            FD_SET(l.listenSock, &readSet);
            if (l.listenSock > maxSock) maxSock = l.listenSock;
        }
        for (const auto& t : currentTunnels) {
            FD_SET(t.sock, &readSet);
            if (t.sock > maxSock) maxSock = t.sock;
        }

        timeval tv{ 0, 20000 };
        int res = select(static_cast<int>(maxSock + 1), &readSet, nullptr, nullptr, &tv);
        if (res <= 0) continue;

        for (const auto& l : currentListeners) {
            if (FD_ISSET(l.listenSock, &readSet)) {
                sockaddr_in clientAddr{};
                int addrLen = sizeof(clientAddr);
                SOCKET cs = accept(l.listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
                if (cs != INVALID_SOCKET) {
                    u_long nonblock = 1;
                    ioctlsocket(cs, FIONBIO, &nonblock);

                    uint32_t tid = 0;
                    {
                        std::lock_guard<std::mutex> lk(tunnelMutex_);
                        tid = nextTunnelId_++;
                        viewerTunnels_[tid] = { tid, l.ruleId, cs, l.targetPort };
                    }

                    TunnelOpenHeader toh{};
                    toh.tunnelId = tid;
                    toh.targetPort = l.targetPort;
                    toh.flags = 0;
                    ByteWriter w;
                    w.writeBytes(&toh, sizeof(toh));
                    sendViewerEncryptedPacket(PacketType::TUNNEL_OPEN, 0, w.buffer().data(), w.buffer().size());
                }
            }
        }

        std::vector<uint32_t> closedTunnels;
        for (const auto& t : currentTunnels) {
            if (FD_ISSET(t.sock, &readSet)) {
                int n = recv(t.sock, reinterpret_cast<char*>(readBuf.data()), static_cast<int>(readBuf.size()), 0);
                if (n > 0) {
                    TunnelDataHeader tdh{};
                    tdh.tunnelId = t.tunnelId;
                    tdh.dataLen = static_cast<uint32_t>(n);
                    ByteWriter w;
                    w.writeBytes(&tdh, sizeof(tdh));
                    w.writeBytes(readBuf.data(), n);
                    sendViewerEncryptedPacket(PacketType::TUNNEL_DATA, 0, w.buffer().data(), w.buffer().size());

                    std::lock_guard<std::mutex> lk(tunnelMutex_);
                    for (auto& r : tunnelRules_) {
                        if (r.ruleId == t.ruleId) {
                            r.bytesTransferredOut += n;
                            break;
                        }
                    }
                } else {
                    closedTunnels.push_back(t.tunnelId);
                }
            }
        }

        for (uint32_t tid : closedTunnels) {
            TunnelCloseHeader tch{};
            tch.tunnelId = tid;
            tch.reasonCode = 0;
            ByteWriter w;
            w.writeBytes(&tch, sizeof(tch));
            sendViewerEncryptedPacket(PacketType::TUNNEL_CLOSE, 0, w.buffer().data(), w.buffer().size());

            std::lock_guard<std::mutex> lk(tunnelMutex_);
            auto it = viewerTunnels_.find(tid);
            if (it != viewerTunnels_.end()) {
                closesocket(it->second.sock);
                viewerTunnels_.erase(it);
            }
        }
    }
}

void NetworkEngine::startHostTunnelProxy() {
    stopHostTunnelProxy();
    hostTunnelActive_.store(true);
    hostTunnelThread_ = std::thread(&NetworkEngine::hostTunnelProxyLoop, this);
}

void NetworkEngine::stopHostTunnelProxy() {
    hostTunnelActive_.store(false);
    try {
        if (hostTunnelThread_.joinable()) {
            hostTunnelThread_.join();
        }
    } catch (...) {}
    std::lock_guard<std::mutex> lk(tunnelMutex_);
    for (auto& kv : hostTunnels_) {
        closesocket(kv.second.sock);
    }
    hostTunnels_.clear();
}

void NetworkEngine::hostTunnelProxyLoop() {
    std::vector<uint8_t> readBuf(16384);

    while (hostTunnelActive_.load() && running_.load() && activeHostClientSock_.load() != ~uintptr_t(0)) {
        fd_set readSet;
        FD_ZERO(&readSet);
        SOCKET maxSock = 0;

        std::vector<ActiveTunnel> currentTunnels;
        {
            std::lock_guard<std::mutex> lk(tunnelMutex_);
            for (const auto& kv : hostTunnels_) {
                currentTunnels.push_back(kv.second);
            }
        }

        if (currentTunnels.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        for (const auto& t : currentTunnels) {
            FD_SET(t.sock, &readSet);
            if (t.sock > maxSock) maxSock = t.sock;
        }

        timeval tv{ 0, 20000 };
        int res = select(static_cast<int>(maxSock + 1), &readSet, nullptr, nullptr, &tv);
        if (res <= 0) continue;

        std::vector<uint32_t> closedTunnels;
        for (const auto& t : currentTunnels) {
            if (FD_ISSET(t.sock, &readSet)) {
                int n = recv(t.sock, reinterpret_cast<char*>(readBuf.data()), static_cast<int>(readBuf.size()), 0);
                if (n > 0) {
                    TunnelDataHeader tdh{};
                    tdh.tunnelId = t.tunnelId;
                    tdh.dataLen = static_cast<uint32_t>(n);
                    ByteWriter w;
                    w.writeBytes(&tdh, sizeof(tdh));
                    w.writeBytes(readBuf.data(), n);
                    sendHostEncryptedPacket(PacketType::TUNNEL_DATA, 0, w.buffer().data(), w.buffer().size());
                } else {
                    closedTunnels.push_back(t.tunnelId);
                }
            }
        }

        for (uint32_t tid : closedTunnels) {
            TunnelCloseHeader tch{};
            tch.tunnelId = tid;
            tch.reasonCode = 0;
            ByteWriter w;
            w.writeBytes(&tch, sizeof(tch));
            sendHostEncryptedPacket(PacketType::TUNNEL_CLOSE, 0, w.buffer().data(), w.buffer().size());

            std::lock_guard<std::mutex> lk(tunnelMutex_);
            auto it = hostTunnels_.find(tid);
            if (it != hostTunnels_.end()) {
                closesocket(it->second.sock);
                hostTunnels_.erase(it);
            }
        }
    }
}

// ---------------- Remote Hardware Diagnostics & Live Process Telemetry ----------------

SystemDiagnosticsPayload NetworkEngine::sampleHostDiagnostics() {
    SystemDiagnosticsPayload diag{};

    // 1. CPU Usage % via GetSystemTimes
    static std::mutex s_cpuTimesMutex;
    static uint64_t s_prevIdleTime = 0;
    static uint64_t s_prevKernelTime = 0;
    static uint64_t s_prevUserTime = 0;

    std::lock_guard<std::mutex> lk(s_cpuTimesMutex);
    FILETIME idleTime{}, kernelTime{}, userTime{};
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        auto ftToU64 = [](const FILETIME& ft) -> uint64_t {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        uint64_t curIdle = ftToU64(idleTime);
        uint64_t curKernel = ftToU64(kernelTime);
        uint64_t curUser = ftToU64(userTime);

        if (s_prevKernelTime > 0 || s_prevUserTime > 0) {
            uint64_t deltaIdle = (curIdle >= s_prevIdleTime) ? (curIdle - s_prevIdleTime) : 0;
            uint64_t deltaKernel = (curKernel >= s_prevKernelTime) ? (curKernel - s_prevKernelTime) : 0;
            uint64_t deltaUser = (curUser >= s_prevUserTime) ? (curUser - s_prevUserTime) : 0;
            uint64_t deltaTotal = deltaKernel + deltaUser;
            if (deltaTotal > 0 && deltaTotal >= deltaIdle) {
                diag.cpuUsagePercent = static_cast<float>((deltaTotal - deltaIdle) * 100.0 / static_cast<double>(deltaTotal));
                diag.cpuUsagePercent = std::clamp(diag.cpuUsagePercent, 0.0f, 100.0f);
            }
        } else {
            // First time sampling: short sleep to establish an initial delta
            Sleep(15);
            FILETIME i2{}, k2{}, u2{};
            if (GetSystemTimes(&i2, &k2, &u2)) {
                uint64_t curIdle2 = ftToU64(i2);
                uint64_t curKernel2 = ftToU64(k2);
                uint64_t curUser2 = ftToU64(u2);
                uint64_t deltaIdle = (curIdle2 >= curIdle) ? (curIdle2 - curIdle) : 0;
                uint64_t deltaKernel = (curKernel2 >= curKernel) ? (curKernel2 - curKernel) : 0;
                uint64_t deltaUser = (curUser2 >= curUser) ? (curUser2 - curUser) : 0;
                uint64_t deltaTotal = deltaKernel + deltaUser;
                if (deltaTotal > 0 && deltaTotal >= deltaIdle) {
                    diag.cpuUsagePercent = static_cast<float>((deltaTotal - deltaIdle) * 100.0 / static_cast<double>(deltaTotal));
                    diag.cpuUsagePercent = std::clamp(diag.cpuUsagePercent, 0.0f, 100.0f);
                }
                curIdle = curIdle2;
                curKernel = curKernel2;
                curUser = curUser2;
            }
        }
        s_prevIdleTime = curIdle;
        s_prevKernelTime = curKernel;
        s_prevUserTime = curUser;
    }

    // 2. Physical RAM Usage via GlobalMemoryStatusEx
    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        diag.ramTotalBytes = memStatus.ullTotalPhys;
        diag.ramUsedBytes = (memStatus.ullTotalPhys >= memStatus.ullAvailPhys)
            ? (memStatus.ullTotalPhys - memStatus.ullAvailPhys)
            : 0;
    }

    // 3. Primary Disk Space via GetDiskFreeSpaceExW
    ULARGE_INTEGER freeBytesAvail{}, totalBytes{}, totalFreeBytes{};
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytesAvail, &totalBytes, &totalFreeBytes)) {
        diag.diskTotalBytes = totalBytes.QuadPart;
        diag.diskUsedBytes = (totalBytes.QuadPart >= totalFreeBytes.QuadPart)
            ? (totalBytes.QuadPart - totalFreeBytes.QuadPart)
            : 0;
    }

    // 4. Top active processes sorted by memory usage
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            std::vector<ProcessTelemetryItem> candidates;
            candidates.reserve(128);
            do {
                if (pe.th32ProcessID == 0) continue; // skip System Idle

                ProcessTelemetryItem item;
                item.pid = pe.th32ProcessID;

                // Convert wchar_t process name to UTF-8
                int reqSize = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, nullptr, 0, nullptr, nullptr);
                if (reqSize > 1) {
                    std::string u8Name(reqSize - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, u8Name.data(), reqSize, nullptr, nullptr);
                    item.name = std::move(u8Name);
                } else {
                    item.name = "Unknown";
                }

                // Query working set memory
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (hProc) {
                    PROCESS_MEMORY_COUNTERS pmc{};
                    pmc.cb = sizeof(pmc);
                    if (K32GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                        item.workingSetBytes = pmc.WorkingSetSize;
                    }
                    CloseHandle(hProc);
                }
                candidates.push_back(std::move(item));
            } while (Process32NextW(hSnap, &pe));

            // Sort descending by memory usage
            std::sort(candidates.begin(), candidates.end(), [](const ProcessTelemetryItem& a, const ProcessTelemetryItem& b) {
                return a.workingSetBytes > b.workingSetBytes;
            });

            // Keep top 15
            size_t maxProcs = std::min<size_t>(candidates.size(), 15);
            diag.processes.assign(candidates.begin(), candidates.begin() + maxProcs);
        }
        CloseHandle(hSnap);
    }

    // 5. Extended Host Hardware Specs & Health Info
    static std::string s_cachedCpuModel;
    static std::string s_cachedGpuModel;
    static std::string s_cachedOsVersion;
    static uint32_t    s_cachedCpuCores = 0;
    static std::mutex  s_hwInfoMutex;

    {
        std::lock_guard<std::mutex> hwLock(s_hwInfoMutex);
        if (s_cachedCpuCores == 0) {
            // CPU brand string
            char cpuBrand[49] = {0};
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            if (__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) && eax >= 0x80000004) {
                __get_cpuid(0x80000002, (unsigned int*)(cpuBrand), (unsigned int*)(cpuBrand + 4), (unsigned int*)(cpuBrand + 8), (unsigned int*)(cpuBrand + 12));
                __get_cpuid(0x80000003, (unsigned int*)(cpuBrand + 16), (unsigned int*)(cpuBrand + 20), (unsigned int*)(cpuBrand + 24), (unsigned int*)(cpuBrand + 28));
                __get_cpuid(0x80000004, (unsigned int*)(cpuBrand + 32), (unsigned int*)(cpuBrand + 36), (unsigned int*)(cpuBrand + 40), (unsigned int*)(cpuBrand + 44));
                char* p = cpuBrand;
                while (*p == ' ') p++;
                s_cachedCpuModel = p;
            }
            if (s_cachedCpuModel.empty()) {
                s_cachedCpuModel = "x86_64 Processor";
            }

            // CPU cores
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            s_cachedCpuCores = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;

            // GPU model via DXGI
            IDXGIFactory1* pFactory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory)) && pFactory) {
                IDXGIAdapter1* pAdapter = nullptr;
                if (SUCCEEDED(pFactory->EnumAdapters1(0, &pAdapter)) && pAdapter) {
                    DXGI_ADAPTER_DESC1 desc{};
                    if (SUCCEEDED(pAdapter->GetDesc1(&desc))) {
                        int req = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
                        if (req > 1) {
                            std::string gpuStr(req - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpuStr.data(), req, nullptr, nullptr);
                            s_cachedGpuModel = std::move(gpuStr);
                        }
                    }
                    pAdapter->Release();
                }
                pFactory->Release();
            }
            if (s_cachedGpuModel.empty()) {
                s_cachedGpuModel = "Primary Display Adapter";
            }

            // OS version via Registry
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                wchar_t prodName[128] = {0};
                DWORD sz = sizeof(prodName);
                if (RegQueryValueExW(hKey, L"ProductName", nullptr, nullptr, (LPBYTE)prodName, &sz) == ERROR_SUCCESS) {
                    wchar_t buildLab[64] = {0};
                    DWORD bsz = sizeof(buildLab);
                    RegQueryValueExW(hKey, L"CurrentBuild", nullptr, nullptr, (LPBYTE)buildLab, &bsz);
                    int req = WideCharToMultiByte(CP_UTF8, 0, prodName, -1, nullptr, 0, nullptr, nullptr);
                    if (req > 1) {
                        std::string osStr(req - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, prodName, -1, osStr.data(), req, nullptr, nullptr);
                        s_cachedOsVersion = std::move(osStr);
                    }
                    if (buildLab[0] != L'\0') {
                        int reqB = WideCharToMultiByte(CP_UTF8, 0, buildLab, -1, nullptr, 0, nullptr, nullptr);
                        if (reqB > 1) {
                            std::string bStr(reqB - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, buildLab, -1, bStr.data(), reqB, nullptr, nullptr);
                            s_cachedOsVersion += " (Build " + bStr + ")";
                        }
                    }
                }
                RegCloseKey(hKey);
            }
            if (s_cachedOsVersion.empty()) {
                s_cachedOsVersion = "Windows Workstation";
            }
        }
        diag.cpuModel = s_cachedCpuModel;
        diag.gpuModel = s_cachedGpuModel;
        diag.osVersion = s_cachedOsVersion;
        diag.cpuCores = s_cachedCpuCores;
    }

    diag.uptimeSeconds = GetTickCount64() / 1000;

    return diag;
}

bool NetworkEngine::executeProcessKill(uint32_t pid, uint8_t callerPermissions) {
    if ((callerPermissions & PERM_INPUT) == 0) {
        return false;
    }
    if (pid == 0 || pid == 4) {
        return false;
    }
    if (pid == GetCurrentProcessId()) {
        return false;
    }

    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) {
        return false;
    }
    BOOL success = TerminateProcess(hProc, 1);
    CloseHandle(hProc);
    return success != FALSE;
}

void NetworkEngine::setDiagnosticsActive(bool active) {
    viewerDiagnosticsActive_.store(active);
    ByteWriter w;
    w.writeU8(active ? 1 : 0);
    sendViewerEncryptedPacket(PacketType::DIAGNOSTICS_REQ, 0, w.buffer().data(), w.buffer().size());
}

void NetworkEngine::sendProcessKill(uint32_t pid) {
    ByteWriter w;
    w.writeU32(pid);
    sendViewerEncryptedPacket(PacketType::PROCESS_KILL, 0, w.buffer().data(), w.buffer().size());
}

SystemDiagnosticsPayload NetworkEngine::latestDiagnostics() const {
    std::lock_guard<std::mutex> lk(diagnosticsMutex_);
    return latestDiagnostics_;
}

// ---------------- Virtual Display Fit & Dynamic Resolution Matching (Feature 5) ----------------

bool NetworkEngine::requestHostResolution(uint32_t width, uint32_t height, uint8_t mode) {
    ResolutionChangePayload payload{};
    payload.targetWidth = width;
    payload.targetHeight = height;
    payload.mode = mode;
    return sendViewerEncryptedPacket(PacketType::RESOLUTION_CHANGE_REQ, 0, &payload, sizeof(payload));
}

bool NetworkEngine::restoreHostResolution() {
    ResolutionChangePayload payload{};
    payload.targetWidth = 0;
    payload.targetHeight = 0;
    payload.mode = 0;
    return sendViewerEncryptedPacket(PacketType::RESOLUTION_CHANGE_REQ, 0, &payload, sizeof(payload));
}

bool NetworkEngine::isHostResolutionChanged() const {
    return displayManager_.isResolutionChanged();
}

void NetworkEngine::handleIncomingResolutionChangeReq(const uint8_t* payload, size_t len, uint8_t callerPermissions) {
    if ((callerPermissions & PERM_INPUT) == 0) {
        return;
    }
    if (!payload || len < sizeof(ResolutionChangePayload)) {
        return;
    }
    ResolutionChangePayload req{};
    std::memcpy(&req, payload, sizeof(req));
    if (req.mode == 0 || (req.targetWidth == 0 && req.targetHeight == 0)) {
        displayManager_.restoreResolution();
    } else if (req.mode == 2) {
        auto modes = DisplayResolutionManager::enumerateDisplayModes();
        auto best = DisplayResolutionManager::findBestResolutionMatch(req.targetWidth, req.targetHeight, modes);
        if (best.width > 0 && best.height > 0) {
            displayManager_.changeResolution(best.width, best.height);
        } else {
            displayManager_.changeResolution(req.targetWidth, req.targetHeight);
        }
    } else {
        displayManager_.changeResolution(req.targetWidth, req.targetHeight);
    }
}

// ---------------- Relay & STUN Network Diagnostics (v3.2.0) ----------------

StunNatResult NetworkEngine::queryStunServer(const std::string& hostPort, uint32_t timeoutMs) {
    ensureWinsockInitialized();
    StunNatResult result;
    result.success = false;
    result.rttMs = -1;

    std::string stunHost;
    uint16_t stunPort = 19302;
    if (!parseHostPort(hostPort, stunHost, stunPort, 19302)) {
        result.natTypeDescription = "Invalid STUN address";
        return result;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(stunPort);
    if (getaddrinfo(stunHost.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        result.natTypeDescription = "DNS resolution failed";
        return result;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        result.natTypeDescription = "Socket creation failed";
        return result;
    }

    #pragma pack(push, 1)
    struct StunHeader {
        uint16_t msgType;
        uint16_t msgLen;
        uint32_t magic;
        uint8_t  transId[12];
    };
    #pragma pack(pop)

    StunHeader req{};
    req.msgType = htons(0x0001); // Binding Request
    req.msgLen  = htons(0x0000); // 0 bytes payload
    req.magic   = htonl(0x2112A442);
    CryptoUtils::randomBytes(req.transId, 12);

    uint64_t t0 = nowTickMs();
    int sent = sendto(s, reinterpret_cast<const char*>(&req), sizeof(req), 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);

    if (sent != sizeof(req)) {
        closesocket(s);
        result.natTypeDescription = "Failed to send STUN request";
        return result;
    }

    fd_set rfds{};
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);

    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int sel = select(0, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0 || !FD_ISSET(s, &rfds)) {
        closesocket(s);
        result.natTypeDescription = "STUN query timed out";
        return result;
    }

    uint8_t recvBuf[1024] = {};
    sockaddr_in fromAddr{};
    int fromLen = sizeof(fromAddr);
    int nRecv = recvfrom(s, reinterpret_cast<char*>(recvBuf), sizeof(recvBuf), 0, reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
    uint64_t t1 = nowTickMs();
    closesocket(s);

    if (nRecv < static_cast<int>(sizeof(StunHeader))) {
        result.natTypeDescription = "STUN response too short";
        return result;
    }

    const auto* respHdr = reinterpret_cast<const StunHeader*>(recvBuf);
    if (ntohl(respHdr->magic) != 0x2112A442) {
        result.natTypeDescription = "Invalid STUN magic cookie";
        return result;
    }
    if (std::memcmp(respHdr->transId, req.transId, 12) != 0) {
        result.natTypeDescription = "Transaction ID mismatch";
        return result;
    }
    if (ntohs(respHdr->msgType) != 0x0101) {
        result.natTypeDescription = "STUN server returned error/non-success";
        return result;
    }

    result.rttMs = std::max<int>(1, static_cast<int>(t1 - t0));

    uint16_t bodyLen = ntohs(respHdr->msgLen);
    size_t offset = sizeof(StunHeader);
    size_t maxOffset = std::min<size_t>(static_cast<size_t>(nRecv), sizeof(StunHeader) + bodyLen);

    while (offset + 4 <= maxOffset) {
        uint16_t attrType = 0;
        uint16_t attrLen = 0;
        std::memcpy(&attrType, recvBuf + offset, sizeof(attrType));
        std::memcpy(&attrLen, recvBuf + offset + 2, sizeof(attrLen));
        attrType = ntohs(attrType);
        attrLen = ntohs(attrLen);
        const uint8_t* val = recvBuf + offset + 4;

        if (offset + 4 + attrLen > maxOffset) break;

        if (attrType == 0x0020 && attrLen >= 8) { // XOR-MAPPED-ADDRESS
            uint8_t family = val[1];
            if (family == 0x01) { // IPv4
                uint16_t xorPort = 0;
                uint32_t xorIp = 0;
                std::memcpy(&xorPort, val + 2, sizeof(xorPort));
                std::memcpy(&xorIp, val + 4, sizeof(xorIp));
                uint16_t port = ntohs(xorPort) ^ 0x2112;
                uint32_t ip = ntohl(xorIp) ^ 0x2112A442;

                in_addr in{};
                in.s_addr = htonl(ip);
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &in, ipStr, sizeof(ipStr));

                result.publicIp = ipStr;
                result.publicPort = port;
                result.success = true;
                result.natTypeDescription = "Reflexive NAT (" + result.publicIp + ":" + std::to_string(result.publicPort) + ")";
                break;
            }
        } else if (attrType == 0x0001 && attrLen >= 8 && !result.success) { // MAPPED-ADDRESS
            uint8_t family = val[1];
            if (family == 0x01) { // IPv4
                uint16_t port = 0;
                uint32_t ip = 0;
                std::memcpy(&port, val + 2, sizeof(port));
                std::memcpy(&ip, val + 4, sizeof(ip));
                port = ntohs(port);
                ip = ntohl(ip);

                in_addr in{};
                in.s_addr = htonl(ip);
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &in, ipStr, sizeof(ipStr));

                result.publicIp = ipStr;
                result.publicPort = port;
                result.success = true;
                result.natTypeDescription = "Mapped NAT (" + result.publicIp + ":" + std::to_string(result.publicPort) + ")";
            }
        }

        offset += 4 + ((attrLen + 3) & ~3);
    }

    if (!result.success) {
        result.natTypeDescription = "No MAPPED-ADDRESS attribute found";
    }
    return result;
}

RelayProbeResult NetworkEngine::probeRelayServer(const std::string& hostPort, uint32_t timeoutMs) {
    ensureWinsockInitialized();
    RelayProbeResult res;
    std::string host;
    uint16_t port = DEFAULT_RELAY_PORT;
    if (!parseHostPort(hostPort, host, port, DEFAULT_RELAY_PORT)) {
        res.reachable = false;
        res.rttMs = -1;
        res.message = "Invalid relay address";
        return res;
    }

    uint64_t t0 = nowTickMs();
    SOCKET s = connectTcpWithTimeout(host, port, timeoutMs);
    if (s == INVALID_SOCKET) {
        res.reachable = false;
        res.rttMs = -1;
        res.message = "Unreachable";
        return res;
    }

    uint64_t t1 = nowTickMs();
    res.reachable = true;
    res.rttMs = std::max<int>(1, static_cast<int>(t1 - t0));
    res.message = "Online (" + std::to_string(res.rttMs) + " ms)";
    closesocket(s);
    return res;
}

void NetworkEngine::setRelayAddressAndReconnect(const std::string& newAddr) {
    identity_.setRelayServerAddress(newAddr);
    AppSettings s = identity_.settings();
    s.relayServer = newAddr;
    identity_.updateSettings(s);

    if (relayControlSock_ != ~uintptr_t(0)) {
        closeWinSock(relayControlSock_);
    }
}

void NetworkEngine::startNetworkDiagnostics(const std::string& relayAddr, const std::string& stunAddr) {
    std::lock_guard<std::mutex> lock(netDiagMutex_);
    if (netDiagResult_.running) {
        return;
    }
    if (netDiagThread_.joinable()) {
        try { netDiagThread_.join(); } catch (...) {}
    }

    netDiagResult_.running = true;
    netDiagResult_.completed = false;
    netDiagResult_.relay = RelayProbeResult{};
    netDiagResult_.stun = StunNatResult{};

    netDiagThread_ = std::thread([this, relayAddr, stunAddr]() {
        RelayProbeResult rProbe = probeRelayServer(relayAddr, 2500);
        StunNatResult sProbe = queryStunServer(stunAddr, 2500);

        std::lock_guard<std::mutex> lk(netDiagMutex_);
        netDiagResult_.relay = rProbe;
        netDiagResult_.stun = sProbe;
        netDiagResult_.running = false;
        netDiagResult_.completed = true;
    });
}

bool NetworkEngine::isNetworkDiagnosticRunning() const {
    std::lock_guard<std::mutex> lock(netDiagMutex_);
    return netDiagResult_.running;
}

bool NetworkEngine::getNetworkDiagnosticResult(RelayProbeResult& outRelay, StunNatResult& outStun) const {
    std::lock_guard<std::mutex> lock(netDiagMutex_);
    if (!netDiagResult_.completed) return false;
    outRelay = netDiagResult_.relay;
    outStun = netDiagResult_.stun;
    return true;
}

std::string NetworkEngine::remotePeerAppVersion() const {
    std::lock_guard<std::mutex> lk(peerVersionMutex_);
    return remotePeerAppVersion_;
}

bool NetworkEngine::isLocalVersionOutdated() const {
    return localIsOutdated_.load();
}

bool NetworkEngine::isRemoteVersionOutdated() const {
    return remoteIsOutdated_.load();
}

void NetworkEngine::setOnVersionUpgradeNoticeCallback(std::function<void(const VersionUpgradeNoticePayload&)> cb) {
    std::lock_guard<std::mutex> lk(peerVersionMutex_);
    onVersionUpgradeNotice_ = std::move(cb);
}

void NetworkEngine::setOnVersionMismatchCallback(std::function<void(const std::string& hostVer, const std::string& viewerVer)> cb) {
    std::lock_guard<std::mutex> lk(peerVersionMutex_);
    onVersionMismatch_ = std::move(cb);
}

void NetworkEngine::setOnScratchpadUpdateCallback(std::function<void(const ScratchpadUpdatePayload&)> cb) {
    std::lock_guard<std::mutex> lk(scratchpadCbMutex_);
    onScratchpadUpdate_ = std::move(cb);
}

bool NetworkEngine::sendScratchpadText(const std::string& text, uint32_t cursorPos) {
    uint32_t myId = static_cast<uint32_t>(identity_.deskId());
    ScratchpadUpdatePayload payload = scratchpadMgr_.updateLocalText(text, myId, cursorPos);
    std::vector<uint8_t> buf;
    serializeScratchpadUpdate(payload, buf);
    if (viewerActive_.load()) {
        return sendViewerEncryptedPacket(PacketType::SCRATCHPAD_UPDATE, FLAG_ENCRYPTED, buf.data(), buf.size());
    } else if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        return sendHostEncryptedPacket(PacketType::SCRATCHPAD_UPDATE, FLAG_ENCRYPTED, buf.data(), buf.size());
    }
    return false;
}

void NetworkEngine::setOnCommandPaletteOutputCallback(std::function<void(const CommandPaletteOutputPayload&)> cb) {
    std::lock_guard<std::mutex> lk(commandPaletteCbMutex_);
    onCommandPaletteOutput_ = std::move(cb);
}

bool NetworkEngine::sendCommandPaletteExec(const std::string& commandLine, uint32_t execId) {
    if (execId == 0) {
        execId = nextExecId_.fetch_add(1);
    }
    CommandPaletteExecPayload payload;
    payload.commandId = execId;
    payload.commandKey = commandLine;
    payload.customCommand = commandLine;
    std::vector<uint8_t> buf;
    serializeCommandPaletteExec(payload, buf);
    if (viewerActive_.load()) {
        return sendViewerEncryptedPacket(PacketType::COMMAND_PALETTE_EXEC, FLAG_ENCRYPTED, buf.data(), buf.size());
    }
    return false;
}

} // namespace cppdesk

