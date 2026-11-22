#include "network_engine.hpp"
#if defined(_WIN32)
#include "../control/windows_service_manager.hpp"
#endif
#include "../capture/virtual_display_manager.hpp"
#include "../capture/screen_blank_manager.hpp"
#include "../capture/avatar_image.hpp"

#if defined(_WIN32)
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
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif
#endif

#include <cstring>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <deque>

#if !defined(_WIN32)
#include <csignal>
#include <sys/statvfs.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <unistd.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#endif

#include "../core/win_compat.hpp"

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

#if defined(_WIN32)
static const CLSID CLSID_MMDeviceEnumerator_val = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID IID_IMMDeviceEnumerator_val = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID IID_IAudioClient_val = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const IID IID_IAudioCaptureClient_val = { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_val = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#endif

void ensureWinsockInitialized() {
    socket_init_network();
}

uint64_t nowTickMs() {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetTickCount64());
#else
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
#endif
}

void setTcpNoDelay(socket_t s) {
    NetworkEngine::setTcpNoDelay(s);
}

#if defined(_WIN32)
using pollfd_t = WSAPOLLFD;
inline int socketPoll(pollfd_t* fds, size_t count, int timeoutMs) {
    if (!fds || count == 0) return 0;
    return WSAPoll(fds, static_cast<ULONG>(count), timeoutMs);
}
#else
using pollfd_t = struct pollfd;
inline int socketPoll(pollfd_t* fds, size_t count, int timeoutMs) {
    if (!fds || count == 0) return 0;
    int rc;
    do {
        rc = poll(fds, static_cast<nfds_t>(count), timeoutMs);
    } while (rc < 0 && errno == EINTR);
    return rc;
}
#endif

void setSocketSendTimeoutMs(socket_t s, int timeoutMs) {
#if defined(_WIN32)
    DWORD dwTimeout = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&dwTimeout), sizeof(dwTimeout));
#else
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

void setSocketTimeoutMs(socket_t s, int timeoutMs) {
#if defined(_WIN32)
    DWORD dwTimeout = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&dwTimeout), sizeof(dwTimeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&dwTimeout), sizeof(dwTimeout));
#else
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool sendAllBytes(socket_t s, const void* buf, size_t len) {
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

#if defined(_WIN32)
static std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (req <= 0) return L"";
    std::wstring out(req, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &out[0], req);
    return out;
}
#endif

bool recvAllBytes(socket_t s, void* buf, size_t len) {
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

socket_t connectTcpWithTimeout(const std::string& host, uint16_t port, int timeoutMs) {
    ensureWinsockInitialized();

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return INVALID_SOCKET_VAL;
    }

    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET_VAL) {
        freeaddrinfo(res);
        return INVALID_SOCKET_VAL;
    }

    socket_set_nonblocking(s, true);
    socket_set_nosigpipe(s);

    int rc = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);

    if (rc == SOCKET_ERROR_VAL) {
        int err = socket_errno();
        if (!socket_is_wouldblock(err)) {
            socket_close(s);
            return INVALID_SOCKET_VAL;
        }

        pollfd_t pfd{};
        pfd.fd = s;
        pfd.events = POLLOUT;

        int sel = socketPoll(&pfd, 1, timeoutMs);
        if (sel <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) || !(pfd.revents & POLLOUT)) {
            socket_close(s);
            return INVALID_SOCKET_VAL;
        }

        int soErr = 0;
#if defined(_WIN32)
        int soLen = sizeof(soErr);
#else
        socklen_t soLen = sizeof(soErr);
#endif
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen);
        if (soErr != 0) {
            socket_close(s);
            return INVALID_SOCKET_VAL;
        }
    }

    socket_set_nonblocking(s, false);
    setTcpNoDelay(s);
    return s;
}

bool parseHostPort(const std::string& input, std::string& outHost, uint16_t& outPort, uint16_t defaultPort) {
    if (input.empty()) return false;
    auto colon = input.rfind(':');
    if (colon != std::string::npos) {
        outHost = input.substr(0, colon);
        std::string portStr = input.substr(colon + 1);
        if (outHost.empty() || portStr.empty()) return false;
        uint32_t portVal = 0;
        for (char c : portStr) {
            if (c < '0' || c > '9') return false;
            portVal = portVal * 10 + static_cast<uint32_t>(c - '0');
            if (portVal > 65535) return false;
        }
        if (portVal == 0) return false;
        outPort = static_cast<uint16_t>(portVal);
        return true;
    }
    outHost = input;
    outPort = defaultPort;
    return !outHost.empty();
}

std::string detectPrimaryLocalIpv4() {
    ensureWinsockInitialized();
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET_VAL) {
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
            sockaddr_in local{};
#if defined(_WIN32)
            int len = sizeof(local);
#else
            socklen_t len = sizeof(local);
#endif
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                char ipStr[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &local.sin_addr, ipStr, sizeof(ipStr))) {
                    socket_close(s);
                    return std::string(ipStr);
                }
            }
        }
        socket_close(s);
    }
    return "127.0.0.1";
}

void spliceSockets(socket_t a, socket_t b) {
    char buf[32768];
    while (true) {
        int n = recv(a, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (!sendAllBytes(b, buf, static_cast<size_t>(n))) break;
    }
    socket_shutdown(a);
    socket_shutdown(b);
}

void executeRemoteSystemAction(SystemActionType action) {
    InputInjector::executeSystemAction(action);
}

std::array<uint8_t, 32> pairPopDigest(
    const std::array<uint8_t, 32>& sessionKey,
    uint64_t viewerDeskId,
    const PairingKey::PublicKey& viewerKey)
{
    ByteWriter w;
    static constexpr char kTag[] = "CppDesk-PairPoP-v1";
    w.writeBytes(kTag, sizeof(kTag) - 1);
    w.writeBytes(sessionKey.data(), sessionKey.size());
    w.writeU64(viewerDeskId);
    w.writeBytes(viewerKey.data(), viewerKey.size());
    return CryptoUtils::sha256(w.buffer().data(), w.buffer().size());
}

} // namespace

// ---------------- Frame IO ----------------

void NetworkEngine::setTcpNoDelay(socket_t s) {
    if (s == INVALID_SOCKET_VAL) return;
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    // Scale TCP socket send and receive buffers to 2 MB (2,097,152 bytes)
    // to prevent TCP window throttling on high-bitrate video and data transfer
    int bufSize = 2 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    socket_set_nosigpipe(s);
}

bool NetworkEngine::sendScatterGather(
    socket_t s,
    const void* hdrBuf,
    size_t hdrLen,
    const void* payloadBuf,
    size_t payloadLen)
{
    if (s == INVALID_SOCKET_VAL || (!hdrBuf && hdrLen > 0)) return false;

#if defined(_WIN32)
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
#else
    struct iovec bufs[2];
    bufs[0].iov_base = const_cast<void*>(hdrBuf);
    bufs[0].iov_len = hdrLen;

    int numBufs = 1;
    if (payloadBuf && payloadLen > 0) {
        bufs[1].iov_base = const_cast<void*>(payloadBuf);
        bufs[1].iov_len = payloadLen;
        numBufs = 2;
    }

    size_t totalBytes = hdrLen + ((payloadBuf && payloadLen > 0) ? payloadLen : 0);
    if (totalBytes == 0) return true;
    size_t totalSent = 0;
    int curBufIdx = 0;

    while (totalSent < totalBytes) {
        ssize_t bytesSent = writev(s, &bufs[curBufIdx], numBufs - curBufIdx);
        if (bytesSent <= 0) {
            if (bytesSent < 0 && (errno == EINTR)) continue;
            return false;
        }

        totalSent += static_cast<size_t>(bytesSent);
        if (totalSent >= totalBytes) break;

        size_t remainingSent = static_cast<size_t>(bytesSent);
        while (curBufIdx < numBufs && remainingSent > 0) {
            if (remainingSent >= bufs[curBufIdx].iov_len) {
                remainingSent -= bufs[curBufIdx].iov_len;
                bufs[curBufIdx].iov_len = 0;
                curBufIdx++;
            } else {
                bufs[curBufIdx].iov_base = static_cast<char*>(bufs[curBufIdx].iov_base) + remainingSent;
                bufs[curBufIdx].iov_len -= remainingSent;
                remainingSent = 0;
            }
        }
    }
    return true;
#endif
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
    const bool expectEncrypted = (cipher && cipher->isInitialized() && recvSeq);
    const size_t maxWirePayload = expectEncrypted ? (MAX_PACKET_PAYLOAD_SIZE + 28) : MAX_PACKET_PAYLOAD_SIZE;
    if (outHeader.magic != PROTOCOL_MAGIC || outHeader.payloadSize > maxWirePayload) {
        return false;
    }

    if (expectEncrypted) {
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
    if (!hostEncrypted_.load() || activeHostClientSock_.load() == ~uintptr_t(0)) return false;
    std::lock_guard<std::mutex> lock(hostCipherMutex_);
    if (!hostEncrypted_.load() || !hostCipher_.isInitialized()) return false;
    uintptr_t cs = activeHostClientSock_.load();
    if (cs == ~uintptr_t(0)) return false;
    return sendFrame(cs, type, flags, payload, payloadLen, hostSendMutex_,
                     &hostCipher_, &hostSendSeq_);
}

bool NetworkEngine::sendViewerEncryptedPacket(PacketType type, uint8_t flags, const void* payload, size_t payloadLen) {
    if (!viewerEncrypted_.load() || viewerSock_.load() == ~uintptr_t(0)) return false;
    std::lock_guard<std::mutex> lock(viewerCipherMutex_);
    if (!viewerEncrypted_.load() || !viewerCipher_.isInitialized()) return false;
    uintptr_t vs = viewerSock_.load();
    if (vs == ~uintptr_t(0)) return false;
    return sendFrame(vs, type, flags, payload, payloadLen, viewerSendMutex_,
                     &viewerCipher_, &viewerSendSeq_);
}

bool NetworkEngine::sendToSessionTarget(SessionTarget target, PacketType type, uint8_t flags, const void* payload, size_t payloadLen) {
    if (target == SessionTarget::Viewer) {
        return sendViewerEncryptedPacket(type, flags, payload, payloadLen);
    }
    if (target == SessionTarget::Host) {
        return sendHostEncryptedPacket(type, flags, payload, payloadLen);
    }
    if (viewerActive_.load() || viewerSock_.load() != ~uintptr_t(0)) {
        return sendViewerEncryptedPacket(type, flags, payload, payloadLen);
    }
    if (activeHostClientSock_.load() != ~uintptr_t(0)) {
        return sendHostEncryptedPacket(type, flags, payload, payloadLen);
    }
    return false;
}

void NetworkEngine::appendChatMessageLocked(ChatMessageEntry&& entry) {
    constexpr size_t kMaxChatEntries = 100;
    constexpr size_t kMaxTotalChatMediaBytes = 20 * 1024 * 1024;
    chatHistory_.push_back(std::move(entry));
    while (chatHistory_.size() > kMaxChatEntries) {
        chatHistory_.erase(chatHistory_.begin());
    }
    size_t totalMediaBytes = 0;
    for (const auto& m : chatHistory_) {
        totalMediaBytes += m.imageJpegData.size();
    }
    for (auto& m : chatHistory_) {
        if (totalMediaBytes <= kMaxTotalChatMediaBytes) break;
        if (!m.imageJpegData.empty()) {
            totalMediaBytes -= m.imageJpegData.size();
            m.imageJpegData.clear();
            m.imageJpegData.shrink_to_fit();
            m.hasImage = false;
        }
    }
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
        authRateMap_.erase(it);
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
    // Bound authRateMap_ growth by evicting expired records when large.
    static constexpr size_t kMaxRateMapEntries = 4096;
    if (authRateMap_.size() >= 1024) {
        for (auto it = authRateMap_.begin(); it != authRateMap_.end(); ) {
            const bool lockExpired = (it->second.lockedUntilMs != 0 && now >= it->second.lockedUntilMs);
            const bool windowExpired = (it->second.lockedUntilMs == 0 && it->second.windowStartMs != 0 && now - it->second.windowStartMs > 60000);
            if (lockExpired || windowExpired) {
                it = authRateMap_.erase(it);
            } else {
                ++it;
            }
        }
        if (authRateMap_.size() >= kMaxRateMapEntries && authRateMap_.find(ip) == authRateMap_.end()) {
            auto oldest = authRateMap_.begin();
            for (auto it = authRateMap_.begin(); it != authRateMap_.end(); ++it) {
                if (it->second.windowStartMs < oldest->second.windowStartMs) {
                    oldest = it;
                }
            }
            if (oldest != authRateMap_.end()) {
                authRateMap_.erase(oldest);
            }
        }
    }
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

namespace {
std::atomic<bool> g_osAssignedPorts{false};

uint16_t boundPortOf(SOCKET s, uint16_t fallback) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0 && a.sin_port != 0) return ntohs(a.sin_port);
    return fallback;
}
} // namespace

void setOsAssignedPortsForTest(bool on) { g_osAssignedPorts.store(on); }

RelayServer::RelayServer() = default;

RelayServer::~RelayServer() {
    stop();
}

bool RelayServer::start(uint16_t port) {
    ensureWinsockInitialized();
    if (running_.load()) return true;

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;

    int reuse = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_osAssignedPorts.load() ? uint16_t(0) : port);

    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(ls, SOMAXCONN) == SOCKET_ERROR) {
        socket_close(ls);
        return false;
    }

    port_ = boundPortOf(ls, port);
    listenSock_.store(fromWinSock(ls));
    running_.store(true);
    acceptThread_ = std::thread(&RelayServer::acceptLoop, this);
    return true;
}

void RelayServer::reapFinishedWorkers() {
    std::vector<std::thread> finished;
    {
        std::lock_guard<std::mutex> lk(workersMutex_);
        for (auto it = workerThreads_.begin(); it != workerThreads_.end(); ) {
            if (it->done && it->done->load()) {
                finished.push_back(std::move(it->thread));
                it = workerThreads_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& t : finished) {
        if (t.joinable()) {
            try { t.join(); } catch (...) {}
        }
    }
}

void RelayServer::stop() {
    if (!running_.exchange(false)) return;
    socket_shutdown(toWinSock(listenSock_.load()));
    try {
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
    } catch (...) {}
    {
        uintptr_t ls = listenSock_.exchange(~uintptr_t(0));
        closeWinSock(ls);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only wake each worker thread: it closes its own socket when it leaves, avoiding double-close races.
        for (auto& h : hosts_) {
            if (h.ctrl) socket_shutdown(toWinSock(h.ctrl->sock));
        }
        hosts_.clear();
        for (auto& b : bridges_) {
            socket_shutdown(toWinSock(b.viewerSock));
            socket_shutdown(toWinSock(b.hostSock));
        }
        for (auto& ab : activeBridges_) {
            socket_shutdown(toWinSock(ab.viewerSock));
            socket_shutdown(toWinSock(ab.hostSock));
        }
    }
    bridgeCv_.notify_all();

    std::vector<std::thread> remaining;
    {
        std::lock_guard<std::mutex> lk(workersMutex_);
        for (auto& w : workerThreads_) {
            remaining.push_back(std::move(w.thread));
        }
        workerThreads_.clear();
    }
    for (auto& t : remaining) {
        if (t.joinable()) {
            try { t.join(); } catch (...) {}
        }
    }
}

size_t RelayServer::registeredPeerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hosts_.size();
}

void RelayServer::setAuthKey(const std::string& authKey) {
    std::lock_guard<std::mutex> lock(mutex_);
    authKey_ = authKey;
}

std::string RelayServer::authKey() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return authKey_;
}

void RelayServer::acceptLoop() {
    while (running_.load()) {
        reapFinishedWorkers();
        SOCKET ls = toWinSock(listenSock_.load());
        if (ls == INVALID_SOCKET) break;

        pollfd_t pfd{};
        pfd.fd = ls;
        pfd.events = POLLIN;
        int pr = socketPoll(&pfd, 1, 150);
        if (pr <= 0) continue;
        if (!running_.load()) break;

        sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        SOCKET cs = accept(ls, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (cs == INVALID_SOCKET) {
            if (!running_.load()) break;
            continue;
        }
        setTcpNoDelay(cs);

        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::string peerIp = ipStr;

        if (!connLimiter_->tryAcquire(peerIp)) {
            uintptr_t rejected = fromWinSock(cs);
            closeWinSock(rejected);
            continue;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lk(workersMutex_);
        if (!running_.load()) {
            connLimiter_->release(peerIp);
            uintptr_t rejected = fromWinSock(cs);
            closeWinSock(rejected);
            break;
        }
        workerThreads_.push_back({
            std::thread([this, limiter = connLimiter_, sock = fromWinSock(cs), peerIp, done]() {
                bool releaseLimiter = true;
                handleClientSocket(sock, peerIp, releaseLimiter);
                if (releaseLimiter) {
                    limiter->release(peerIp);
                }
                done->store(true);
            }),
            done
        });
    }
}

void RelayServer::handleClientSocket(uintptr_t sock, std::string peerIp, bool& releaseLimiter) {
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
    const std::string configuredAuthKey = authKey();
    if (!relayowner::stripAndVerifyAuthTag(configuredAuthKey, hdr.type, hdr.flags, payload)) {
        if (pt == PacketType::RELAY_REGISTER) {
            ByteWriter aw;
            aw.writeU8(0);
            FrameHeader ackHdr{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_REGISTER_ACK), 0, static_cast<uint32_t>(aw.buffer().size()) };
            sendAllBytes(toWinSock(sock), &ackHdr, sizeof(ackHdr));
            sendAllBytes(toWinSock(sock), aw.buffer().data(), aw.buffer().size());
        } else if (pt == PacketType::RELAY_LOOKUP) {
            ByteWriter w;
            w.writeU8(0);
            w.writeString("");
            w.writeU16(0);
            w.writeString("");
            FrameHeader respHdr{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_LOOKUP_RESP), 0, static_cast<uint32_t>(w.buffer().size()) };
            sendAllBytes(toWinSock(sock), &respHdr, sizeof(respHdr));
            sendAllBytes(toWinSock(sock), w.buffer().data(), w.buffer().size());
        } else if (pt == PacketType::RELAY_CONNECT_REQ) {
            ByteWriter w;
            w.writeU8(0);
            FrameHeader rh{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_BRIDGE_READY), 0, static_cast<uint32_t>(w.buffer().size()) };
            sendAllBytes(toWinSock(sock), &rh, sizeof(rh));
            sendAllBytes(toWinSock(sock), w.buffer().data(), w.buffer().size());
        }
        closeWinSock(sock);
        return;
    }

    if (pt == PacketType::RELAY_REGISTER) {
        auto ctrl = std::make_shared<HostControl>();
        ctrl->sock = sock;
        bool registered = false;
        try {
            ByteReader r(payload);
            uint64_t deskId = r.readU64();
            std::string hostname = r.readString();
            std::string reportedIp = r.readString();
            uint16_t tcpPort = r.readU16();

            std::string effectiveIp = (peerIp == "127.0.0.1" && !reportedIp.empty()) ? reportedIp : peerIp;
            // Hosts from before ownership proofs end the frame here.
            const bool ownerCapable = r.remaining() >= 1 && r.readU8() == relayowner::OWNER_CAP;

            auto sendAck = [&](uint8_t accepted) {
                ByteWriter aw;
                aw.writeU8(accepted);
                FrameHeader ackHdr{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_REGISTER_ACK), 0, static_cast<uint32_t>(aw.buffer().size()) };
                std::lock_guard<std::mutex> wlk(ctrl->writeMutex);
                if (!ctrl->closed) {
                    sendAllBytes(toWinSock(ctrl->sock), &ackHdr, sizeof(ackHdr));
                    sendAllBytes(toWinSock(ctrl->sock), aw.buffer().data(), aw.buffer().size());
                }
            };

            // An ID that has an owner is only ever taken over by a host that proves it.
            if (ownerCapable) {
                relayowner::Nonce nonce{};
                CryptoUtils::randomBytes(nonce.data(), nonce.size());
                const bool enrolling = !owners_.hasOwner(deskId, nowTickMs());
                ByteWriter cw;
                cw.writeU8(enrolling ? relayowner::MODE_ENROLL : relayowner::MODE_PROVE);
                cw.writeBytes(nonce.data(), nonce.size());
                FrameHeader chHdr{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_REGISTER_CHALLENGE), 0, static_cast<uint32_t>(cw.buffer().size()) };
                sendAllBytes(toWinSock(sock), &chHdr, sizeof(chHdr));
                sendAllBytes(toWinSock(sock), cw.buffer().data(), cw.buffer().size());

                FrameHeader pHdr{};
                relayowner::Proof proof{};
                bool proven = recvAllBytes(toWinSock(sock), &pHdr, sizeof(pHdr)) &&
                              pHdr.magic == PROTOCOL_MAGIC &&
                              pHdr.type == static_cast<uint8_t>(PacketType::RELAY_REGISTER_PROOF) &&
                              pHdr.payloadSize == proof.size() &&
                              recvAllBytes(toWinSock(sock), proof.data(), proof.size());
                if (proven) {
                    proven = enrolling ? owners_.enroll(deskId, nonce, proof, nowTickMs())
                                       : owners_.verify(deskId, nonce, proof, nowTickMs());
                }
                if (!proven) {
                    sendAck(0);
                    closeWinSock(sock);
                    return;
                }
            } else if (owners_.hasOwner(deskId, nowTickMs())) {
                sendAck(0);
                closeWinSock(sock);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                hosts_.erase(
                    std::remove_if(hosts_.begin(), hosts_.end(), [&](RegisteredHost& h) {
                        if (h.deskId == deskId) {
                            // Wake the old registration's thread but leave the close to it: closing here too would hit
                            // whatever connection has reused that handle number by the time the thread wakes.
                            if (h.ctrl) socket_shutdown(toWinSock(h.ctrl->sock));
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
                rh.ctrl = ctrl;
                rh.lastSeenMs = nowTickMs();
                hosts_.push_back(std::move(rh));
                registered = true;
            }

            sendAck(1);

            setSocketTimeoutMs(toWinSock(sock), 30000);
            while (running_.load()) {
                FrameHeader chdr{};
                if (!recvAllBytes(toWinSock(sock), &chdr, sizeof(chdr)) ||
                    chdr.magic != PROTOCOL_MAGIC ||
                    chdr.payloadSize > 65536) {
                    break;
                }
                std::vector<uint8_t> cpay(chdr.payloadSize);
                if (chdr.payloadSize > 0 && !recvAllBytes(toWinSock(sock), cpay.data(), chdr.payloadSize)) {
                    break;
                }
                if (ownerCapable) owners_.touch(deskId, nowTickMs());
                if (static_cast<PacketType>(chdr.type) == PacketType::PING) {
                    FrameHeader pong{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::PONG), 0, 0 };
                    std::lock_guard<std::mutex> wlk(ctrl->writeMutex);
                    if (!ctrl->closed) {
                        sendAllBytes(toWinSock(ctrl->sock), &pong, sizeof(pong));
                    }
                }
            }

            if (ownerCapable) owners_.touch(deskId, nowTickMs());
        } catch (...) {}

        if (registered) {
            std::lock_guard<std::mutex> lock(mutex_);
            hosts_.erase(
                std::remove_if(hosts_.begin(), hosts_.end(), [&](const RegisteredHost& h) {
                    return h.ctrl == ctrl;
                }),
                hosts_.end()
            );
        }
        socket_shutdown(toWinSock(ctrl->sock));
        {
            std::lock_guard<std::mutex> wlk(ctrl->writeMutex);
            ctrl->closed = true;
            closeWinSock(ctrl->sock);
        }
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
        uintptr_t matchedHostSock = ~uintptr_t(0);
        std::string matchedHostPeerIp;
        uint64_t token = 0;
        bool bridgeInserted = false;
        auto releaseMatchedHost = [&]() {
            if (matchedHostSock != ~uintptr_t(0)) {
                closeWinSock(matchedHostSock);
            }
            if (!matchedHostPeerIp.empty()) {
                connLimiter_->release(matchedHostPeerIp);
                matchedHostPeerIp.clear();
            }
        };
        try {
            ByteReader r(payload);
            uint64_t targetId = r.readU64();

            std::shared_ptr<HostControl> hostCtrl;
            CryptoUtils::randomBytes(&token, sizeof(token));

            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& h : hosts_) {
                    if (h.deskId == targetId && h.ctrl) {
                        hostCtrl = h.ctrl;
                        break;
                    }
                }
                if (hostCtrl) {
                    PendingBridge pb;
                    pb.token = token;
                    pb.viewerSock = sock;
                    pb.ready = false;
                    bridges_.push_back(std::move(pb));
                    bridgeInserted = true;
                }
            }

            if (!hostCtrl) {
                ByteWriter w;
                w.writeU8(0);
                FrameHeader rh{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_BRIDGE_READY), 0, static_cast<uint32_t>(w.buffer().size()) };
                sendAllBytes(toWinSock(sock), &rh, sizeof(rh));
                sendAllBytes(toWinSock(sock), w.buffer().data(), w.buffer().size());
                closeWinSock(sock);
                return;
            }

            {
                ByteWriter reqW;
                reqW.writeU64(token);
                FrameHeader reqH{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_INCOMING_REQ), 0, static_cast<uint32_t>(reqW.buffer().size()) };
                std::lock_guard<std::mutex> wlk(hostCtrl->writeMutex);
                if (!hostCtrl->closed) {
                    sendAllBytes(toWinSock(hostCtrl->sock), &reqH, sizeof(reqH));
                    sendAllBytes(toWinSock(hostCtrl->sock), reqW.buffer().data(), reqW.buffer().size());
                }
            }

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
                        if (it->ready) {
                            matchedHostSock = it->hostSock;
                            matchedHostPeerIp = std::move(it->hostPeerIp);
                        }
                        bridges_.erase(it);
                        bridgeInserted = false;
                        break;
                    }
                }
            }

            if (matchedHostSock == ~uintptr_t(0) || !running_.load()) {
                if (running_.load()) {
                    ByteWriter w;
                    w.writeU8(0);
                    FrameHeader rh{ PROTOCOL_MAGIC, static_cast<uint8_t>(PacketType::RELAY_BRIDGE_READY), 0, static_cast<uint32_t>(w.buffer().size()) };
                    sendAllBytes(toWinSock(sock), &rh, sizeof(rh));
                    sendAllBytes(toWinSock(sock), w.buffer().data(), w.buffer().size());
                }
                releaseMatchedHost();
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

            {
                std::lock_guard<std::mutex> lock(mutex_);
                activeBridges_.push_back({ sock, matchedHostSock });
            }

            std::thread t(spliceSockets, toWinSock(sock), toWinSock(matchedHostSock));
            spliceSockets(toWinSock(matchedHostSock), toWinSock(sock));
            if (t.joinable()) t.join();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                activeBridges_.erase(
                    std::remove_if(activeBridges_.begin(), activeBridges_.end(), [&](const ActiveBridge& ab) {
                        return ab.viewerSock == sock && ab.hostSock == matchedHostSock;
                    }),
                    activeBridges_.end()
                );
            }

            releaseMatchedHost();
            closeWinSock(sock);
            return;
        } catch (...) {
            if (bridgeInserted) {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto it = bridges_.begin(); it != bridges_.end(); ++it) {
                    if (it->token == token) {
                        if (it->ready) {
                            matchedHostSock = it->hostSock;
                            matchedHostPeerIp = std::move(it->hostPeerIp);
                        }
                        bridges_.erase(it);
                        break;
                    }
                }
            }
            releaseMatchedHost();
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
                        b.hostPeerIp = peerIp;
                        b.ready = true;
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) {
                // Lease now belongs to the RELAY_CONNECT_REQ thread for the bridged socket's lifetime.
                releaseLimiter = false;
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
    applyReceiveDirectory();
}

void NetworkEngine::applyReceiveDirectory() {
    const std::string dir = identity_.settings().receiveDir;
    fileManager_.setReceiveDirectory(dir.empty() ? FileTransferManager::defaultReceiveDirectory() : dir);
}

NetworkEngine::~NetworkEngine() {
    stop();
}

bool NetworkEngine::start() {
    if (running_.load()) return true;
    viewerOnly_.store(false);

    uint16_t basePort = g_osAssignedPorts.load() ? uint16_t(0) : identity_.listenPort();
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
            boundPort = boundPortOf(s, candidate);
            break;
        }
        closesocket(s);
    }

    if (hs == INVALID_SOCKET) {
        return false;
    }

    identity_.setListenPort(boundPort);
    hostListenSock_.store(fromWinSock(hs));

    SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (us != INVALID_SOCKET) {
        int broadcast = 1;
        int reuse = 1;
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
        udpSock_.store(fromWinSock(us));
    }

    if (identity_.instanceId() == 1) {
        localRelay_.setAuthKey(identity_.settings().relayAuthKey);
        localRelay_.start(DEFAULT_RELAY_PORT);
    }

    running_.store(true);
    discoveryThread_ = std::thread(&NetworkEngine::discoveryLoop, this);
    relayRegThread_ = std::thread(&NetworkEngine::relayRegistrationLoop, this);
    hostAcceptThread_ = std::thread(&NetworkEngine::hostAcceptLoop, this);

    return true;
}

bool NetworkEngine::startViewerOnly() {
    if (running_.load()) return viewerOnly_.load();
    viewerOnly_.store(true);
    clipboardSyncEnabled_.store(false);
    running_.store(true);
    return true;
}

void NetworkEngine::stop() {
    const bool wasRunning = running_.exchange(false);

    if (wasRunning) {
        disconnectViewer();
        disconnectHostClient();
        respondToIncomingRequest(false, 0);

        relayReconnectRequested_.store(true);
        socket_shutdown(toWinSock(relayControlSock_.load()));
        socket_shutdown(toWinSock(hostListenSock_.load()));
        localRelay_.stop();

        try { if (discoveryThread_.joinable()) discoveryThread_.join(); } catch (...) {}
        try { if (relayRegThread_.joinable()) relayRegThread_.join(); } catch (...) {}
        try { if (hostAcceptThread_.joinable()) hostAcceptThread_.join(); } catch (...) {}
        {
            std::lock_guard<std::mutex> lk(hostSessionThreadMutex_);
            try { if (hostSessionThread_.joinable()) hostSessionThread_.join(); } catch (...) {}
        }

        // Close host listen socket only after hostAcceptThread_ has joined.
        {
            uintptr_t hs = hostListenSock_.exchange(~uintptr_t(0));
            closeWinSock(hs);
        }
        {
            uintptr_t rs = relayControlSock_.exchange(~uintptr_t(0));
            closeWinSock(rs);
        }

        // A viewer-only engine never hosted; the curtain and screen blanking are process-wide and belong to the host engine.
        if (!viewerOnly_.load()) {
            stopHostAudioCapture();
            stopHostTerminal();
            stopHostTunnelProxy();
            hostPrivacyModeActive_.store(false);
            ScreenBlankManager::instance().setEmergencyWakeCallback(nullptr);
            ScreenBlankManager::instance().disengageBlanking();
            ScreenBlankManager::wakeDisplays();
        }
    }

    // Always join viewer and diagnostics threads even if start() was never called or already stopped.
    disconnectViewer();
    {
        std::lock_guard<std::mutex> lk(viewerThreadMutex_);
        try { if (viewerThread_.joinable()) viewerThread_.join(); } catch (...) {}
    }
    // Close UDP socket only after both discoveryThread_ and viewerThread_ have joined.
    {
        uintptr_t us = udpSock_.exchange(~uintptr_t(0));
        closeWinSock(us);
    }
    shutdownViewerAudioPlayback();
    stopViewerTunnelMultiplexer();
    {
        // Join outside the lock: the diagnostics thread takes it to publish its result.
        std::thread diagThread;
        {
            std::lock_guard<std::mutex> lk(netDiagMutex_);
            diagThread = std::move(netDiagThread_);
        }
        try { if (diagThread.joinable()) diagThread.join(); } catch (...) {}
    }
    commandPaletteMgr_.shutdown();
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

void NetworkEngine::injectDiscoveredPeerForTest(const DiscoveredPeer& p) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    std::erase_if(peers_, [&](const DiscoveredPeer& e) { return e.deskId == p.deskId; });
    peers_.push_back(p);
    peers_.back().lastSeenTickMs = nowTickMs();
}

void NetworkEngine::sendDiscoveryQuery(uint64_t targetDeskId) {
    SOCKET us = toWinSock(udpSock_.load());
    if (us == INVALID_SOCKET) return;

    DiscoveryBeaconPacket pkt{};
    pkt.magic = PROTOCOL_MAGIC;
    pkt.packetKind = (targetDeskId == 0) ? 1 : 2;
    pkt.deskId = identity_.deskId();
    pkt.queryId = targetDeskId;
    pkt.tcpPort = identity_.listenPort();
    std::strncpy(pkt.hostname, identity_.wireName(sizeof(pkt.hostname) - 1).c_str(), sizeof(pkt.hostname) - 1);

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
        SOCKET us = toWinSock(udpSock_.load());
        if (us == INVALID_SOCKET) break;

        uint64_t now = nowTickMs();
        if (now - lastBroadcast >= 1500) {
            sendDiscoveryQuery(0);
            lastBroadcast = now;
        }

        pollfd_t pfd{};
        pfd.fd = us;
        pfd.events = POLLIN;
        int sel = socketPoll(&pfd, 1, 150);
        if (sel > 0 && (pfd.revents & POLLIN)) {
            sockaddr_in fromAddr{};
            socklen_t fromLen = sizeof(fromAddr);
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
                        constexpr size_t kMaxDiscoveredPeers = 256;
                        const uint64_t seenAt = nowTickMs();
                        std::lock_guard<std::mutex> lock(peersMutex_);
                        peers_.erase(
                            std::remove_if(peers_.begin(), peers_.end(), [&](const DiscoveredPeer& p) {
                                return seenAt - p.lastSeenTickMs > 12000;
                            }),
                            peers_.end()
                        );
                        bool updated = false;
                        for (auto& p : peers_) {
                            if (p.deskId == pkt.deskId) {
                                p.hostname = pkt.hostname;
                                p.ip = senderIp;
                                p.port = pkt.tcpPort;
                                p.lastSeenTickMs = seenAt;
                                p.viaRelay = false;
                                updated = true;
                                break;
                            }
                        }
                        if (!updated && peers_.size() < kMaxDiscoveredPeers) {
                            DiscoveredPeer np;
                            np.deskId = pkt.deskId;
                            np.hostname = pkt.hostname;
                            np.ip = senderIp;
                            np.port = pkt.tcpPort;
                            np.lastSeenTickMs = seenAt;
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
                        std::strncpy(reply.hostname, identity_.wireName(sizeof(reply.hostname) - 1).c_str(), sizeof(reply.hostname) - 1);
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

        uintptr_t localRelaySock = fromWinSock(rs);
        relayReconnectRequested_.store(false);
        relayControlSock_.store(localRelaySock);

        ByteWriter w;
        w.writeU64(identity_.deskId());
        w.writeString(identity_.wireName());
        w.writeString(localIp_);
        w.writeU16(identity_.listenPort());
        w.writeU8(relayowner::OWNER_CAP);

        const std::string relayAuthKey = identity_.settings().relayAuthKey;

        auto sendRelayFrame = [&](uintptr_t targetSock, PacketType type, const std::vector<uint8_t>& body, std::mutex& mu) {
            if (relayAuthKey.empty()) {
                return sendFrame(targetSock, type, 0, body.data(), body.size(), mu);
            }
            std::vector<uint8_t> tagged = body;
            relayowner::Key tag = relayowner::computeAuthTag(relayAuthKey, static_cast<uint8_t>(type), body.data(), body.size());
            tagged.insert(tagged.end(), tag.begin(), tag.end());
            return sendFrame(targetSock, type, relayowner::FLAG_RELAY_AUTH, tagged.data(), tagged.size(), mu);
        };

        std::mutex regMutex;
        if (!sendRelayFrame(localRelaySock, PacketType::RELAY_REGISTER, w.buffer(), regMutex)) {
            uintptr_t expected = localRelaySock;
            relayControlSock_.compare_exchange_strong(expected, ~uintptr_t(0));
            closeWinSock(localRelaySock);
            continue;
        }

        bool awaitingRegisterAck = true;
        bool challengeAnswered = false;
        while (running_.load() && !relayReconnectRequested_.load()) {
            SOCKET s = toWinSock(localRelaySock);
            if (s == INVALID_SOCKET) break;

            pollfd_t pfd{};
            pfd.fd = s;
            pfd.events = POLLIN;
            int sel = socketPoll(&pfd, 1, 2000);
            if (!running_.load() || relayReconnectRequested_.load()) break;
            if (sel == 0) {
                if (!sendFrame(localRelaySock, PacketType::PING, 0, nullptr, 0, regMutex)) {
                    break;
                }
                continue;
            }
            if (sel < 0) break;

            FrameHeader hdr{};
            std::vector<uint8_t> pay;
            if (!recvFrame(localRelaySock, hdr, pay)) {
                break;
            }

            PacketType pt = static_cast<PacketType>(hdr.type);
            if (pt == PacketType::RELAY_REGISTER_CHALLENGE) {
                if (!awaitingRegisterAck || challengeAnswered || pay.size() != 1 + relayowner::NONCE_LEN) break;
                challengeAnswered = true;
                relayowner::Nonce nonce{};
                std::copy(pay.begin() + 1, pay.end(), nonce.begin());
                // Enrolling or proving, the answer is the same: our public key and a signature over the challenge.
                relayowner::Proof reply{};
                if (!relayowner::makeProof(identity_.pairingKey(), identity_.deskId(), nonce, reply)) break;
                if (!sendFrame(localRelaySock, PacketType::RELAY_REGISTER_PROOF, 0, reply.data(), reply.size(), regMutex)) break;
                continue;
            }
            if (pt == PacketType::RELAY_REGISTER_ACK) {
                if (!awaitingRegisterAck || pay.empty() || pay[0] != 1) break;
                awaitingRegisterAck = false;
                continue;
            }
            if (pt == PacketType::RELAY_INCOMING_REQ) {
                if (awaitingRegisterAck) continue;
                try {
                    ByteReader r(pay);
                    uint64_t bridgeToken = r.readU64();

                    SOCKET bs = connectTcpWithTimeout(rHost, rPort, 1500);
                    if (bs != INVALID_SOCKET) {
                        ByteWriter bw;
                        bw.writeU64(bridgeToken);
                        std::mutex bm;
                        uintptr_t bSock = fromWinSock(bs);
                        if (!sendRelayFrame(bSock, PacketType::RELAY_BRIDGE_ACCEPT, bw.buffer(), bm) ||
                            !tryStartHostSession(bSock, "Relay-Bridge")) {
                            closeWinSock(bSock);
                        }
                    }
                } catch (...) {}
            }
        }

        {
            uintptr_t expected = localRelaySock;
            relayControlSock_.compare_exchange_strong(expected, ~uintptr_t(0));
            closeWinSock(localRelaySock);
        }
        if (relayReconnectRequested_.exchange(false)) {
            continue;
        }
        for (int i = 0; i < 10 && running_.load() && !relayReconnectRequested_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

bool NetworkEngine::startLocalRelayServer(uint16_t port) {
    localRelay_.setAuthKey(identity_.settings().relayAuthKey);
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

    if ((newPermissions & PERM_INPUT) == 0) {
        std::vector<uint32_t> revokedTunnels;
        {
            std::lock_guard<std::mutex> lk(tunnelMutex_);
            for (const auto& kv : hostTunnels_) {
                revokedTunnels.push_back(kv.first);
            }
        }
        stopHostTerminal();
        stopHostTunnelProxy();
        for (uint32_t tid : revokedTunnels) {
            TunnelCloseHeader tch{};
            tch.tunnelId = tid;
            tch.reasonCode = 2; // Forbidden
            ByteWriter cw;
            cw.writeBytes(&tch, sizeof(tch));
            sendHostEncryptedPacket(PacketType::TUNNEL_CLOSE, 0, cw.buffer().data(), cw.buffer().size());
        }
    }
}

void NetworkEngine::disconnectHostClient() {
    stopVoiceIntercom(SessionTarget::Host);
    displayManager_.restoreResolution();
    clipFileMgr_.reset();
    uintptr_t cs = activeHostClientSock_.exchange(~uintptr_t(0));
    if (cs != ~uintptr_t(0)) {
        bool enc = hostEncrypted_.exchange(false);
        if (enc) {
            std::unique_lock<std::mutex> lock(hostCipherMutex_, std::try_to_lock);
            if (lock.owns_lock() && hostCipher_.isInitialized()) {
                sendFrame(cs, PacketType::DISCONNECT, 0, nullptr, 0, hostSendMutex_,
                          &hostCipher_, &hostSendSeq_);
            }
        }
        socket_t sock = toSocket(cs);
        if (sock != INVALID_SOCKET_VAL) {
            linger l{1, 0};
            setsockopt(sock, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
            // The host session thread exclusively owns closing clientSock after its worker threads join.
            socket_shutdown(sock);
        }
    }
    {
        std::lock_guard<std::mutex> lk(hostSessionThreadMutex_);
        if (hostSessionThread_.joinable()) {
            if (std::this_thread::get_id() != hostSessionThread_.get_id()) {
                hostSessionThread_.join();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(hostStatusMutex_);
        hostStatus_ = HostSessionStatus{};
    }
}

bool NetworkEngine::tryStartHostSession(uintptr_t clientSock, std::string clientIp) {
    if (!running_.load()) return false;
    uintptr_t expected = ~uintptr_t(0);
    if (!activeHostClientSock_.compare_exchange_strong(expected, clientSock)) {
        return false;
    }
    std::lock_guard<std::mutex> slk(hostSessionThreadMutex_);
    if (!running_.load()) {
        uintptr_t cur = clientSock;
        activeHostClientSock_.compare_exchange_strong(cur, ~uintptr_t(0));
        return false;
    }
    try {
        if (hostSessionThread_.joinable()) {
            hostSessionThread_.join();
        }
        hostSessionThread_ = std::thread(&NetworkEngine::runHostSession, this, clientSock, std::move(clientIp));
    } catch (...) {
        uintptr_t cur = clientSock;
        activeHostClientSock_.compare_exchange_strong(cur, ~uintptr_t(0));
        return false;
    }
    return true;
}

void NetworkEngine::hostAcceptLoop() {
    while (running_.load()) {
        SOCKET ls = toWinSock(hostListenSock_.load());
        if (ls == INVALID_SOCKET) break;

        pollfd_t pfd{};
        pfd.fd = ls;
        pfd.events = POLLIN;
        int pr = socketPoll(&pfd, 1, 150);
        if (pr <= 0) continue;
        if (!running_.load()) break;

        sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
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

        if (!tryStartHostSession(clientSock, clientIp)) {
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
    }
}

void NetworkEngine::runHostSession(uintptr_t clientSock, std::string clientIp) {
    hostSessionGen_.fetch_add(1);
    hostEncrypted_.store(false);

    auto cleanup = [&]() {
        stopHostAudioCapture();
        voiceOpenFailed_.store(false);
        if (hostMicLive_.exchange(false)) {   // the viewer's voice opened the microphone: it closes with the session
            voiceIntercom_.stopCapture();
            voiceIntercom_.stopPlayback();
        }
        hostPrivacyModeActive_.store(false);
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
            hostRecvCipher_.reset();
            CryptoUtils::secureZero(hostSessionKey_.data(), hostSessionKey_.size());
        }
        {
            uintptr_t expected = clientSock;
            activeHostClientSock_.compare_exchange_strong(expected, ~uintptr_t(0));
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

    if (activeHostClientSock_.load() != clientSock || !running_.load()) {
        cleanup();
        return;
    }

    try {
        // Apply 8-second handshake socket timeout to prevent half-open stalls
        setSocketTimeoutMs(toWinSock(clientSock), 8000);

        // Initialize Ephemeral ECDH (NIST P-256) for forward secrecy
        EcdhKeyExchange hostEcdh;
        if (!hostEcdh.initialize() || hostEcdh.localPublicKey().empty()) {
            cleanup();
            return;
        }

        // 1. Receive HELLO
        FrameHeader hdr{};
        std::vector<uint8_t> handshakePayload;
        if (!recvFrame(clientSock, hdr, handshakePayload) || static_cast<PacketType>(hdr.type) != PacketType::HELLO) {
            cleanup();
            return;
        }

        ByteReader helloReader(handshakePayload);
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
        if (protoVer != PROTOCOL_VERSION || viewerPubBlob.empty()) {
            cleanup();
            return;
        }

        // 2. Send AUTH_CHALLENGE (including Host Ephemeral ECDH Public Key and PBKDF2 params)
        std::array<uint8_t, 32> nonce{};
        if (!CryptoUtils::randomBytes(nonce.data(), nonce.size())) {
            cleanup();
            return;
        }
        std::array<uint8_t, 16> challengeSalt{};
        uint32_t challengeIterations = CryptoUtils::kDefaultPbkdf2Iterations;
        identity_.getAuthKdfParams(challengeSalt, challengeIterations);

        {
            ByteWriter w;
            w.writeU64(identity_.deskId());
            w.writeString(identity_.wireName());
            w.writeBytes(nonce.data(), nonce.size());
            w.writeU8(identity_.unattendedEnabled() ? 1 : 0);
            w.writeU16(static_cast<uint16_t>(hostEcdh.localPublicKey().size()));
            if (!hostEcdh.localPublicKey().empty()) {
                w.writeBytes(hostEcdh.localPublicKey().data(), hostEcdh.localPublicKey().size());
            }
            w.writeString(CPP_DESK_VERSION);
            w.writeU8(AUTH_CAP_BOUND_RESPONSE | AUTH_CAP_PAIRING);
            // Signed over this session (nonce + both ECDH keys) so a pinned viewer knows it reached the real PC.
            const PairingKey& pk = identity_.pairingKey();
            PairingKey::Signature hostSig{};
            pk.sign(CryptoUtils::pairingTranscript('H', identity_.deskId(), viewerId, nonce,
                                                   hostEcdh.localPublicKey(), viewerPubBlob), hostSig);
            w.writeU8(identity_.settings().allowPairing ? PAIR_FLAG_ALLOW_NEW : 0);
            w.writeU16(static_cast<uint16_t>(pk.publicKey().size()));
            w.writeBytes(pk.publicKey().data(), pk.publicKey().size());
            w.writeU16(static_cast<uint16_t>(hostSig.size()));
            w.writeBytes(hostSig.data(), hostSig.size());
            w.writeBytes(challengeSalt.data(), challengeSalt.size());
            w.writeU32(challengeIterations);
            if (!sendFrame(clientSock, PacketType::AUTH_CHALLENGE, 0, w.buffer().data(), w.buffer().size(), hostSendMutex_)) {
                cleanup();
                return;
            }
        }

        // Derive ECDH session key and enable AES-256-GCM before receiving AUTH_RESPONSE, reboot tokens, or TOTP codes.
        if (!hostEcdh.computeSharedSessionKey(
                viewerPubBlob.data(), viewerPubBlob.size(),
                identity_.deskId(), viewerId, nonce, hostSessionKey_) ||
            !CryptoUtils::isUsableKey(hostSessionKey_)) {
            cleanup();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(hostCipherMutex_);
            if (!hostCipher_.initialize(hostSessionKey_, /*isHost=*/true) ||
                !hostRecvCipher_.initialize(hostSessionKey_, /*isHost=*/true)) {
                cleanup();
                return;
            }
            hostSendSeq_ = 0;
            hostEncrypted_.store(true);
        }
        uint64_t hostRecvSeq = 0;

        // 3. Receive AUTH_RESPONSE (encrypted)
        if (!recvFrame(clientSock, hdr, handshakePayload, &hostRecvCipher_, &hostRecvSeq) ||
            static_cast<PacketType>(hdr.type) != PacketType::AUTH_RESPONSE) {
            cleanup();
            return;
        }

        ByteReader authReader(handshakePayload);
        uint8_t hasPassword = authReader.readU8();
        std::array<uint8_t, 32> clientDigest{};
        authReader.readBytes(clientDigest.data(), clientDigest.size());
        PairingKey::Signature viewerSig{};
        const bool sigPresent = hasPassword == AUTH_MODE_PAIRED &&
            authReader.hasRemaining(2 + viewerSig.size()) && authReader.readU16() == viewerSig.size();
        if (sigPresent) authReader.readBytes(viewerSig.data(), viewerSig.size());
        const auto viewerPicture = readWirePicture(authReader);   // after the mode fields; 1.0 viewers send none

        // Relayed sessions share clientIp == "Relay-Bridge", so they share one lockout. viewerId is chosen by the
        // peer, so keying on it would let an attacker dodge the lockout with a fresh id per guess.
        const std::string& rateKey = clientIp;

        bool accepted = false;
        bool pairedLogin = false;
        uint8_t grantedPerms = PERM_ALL;
        AuthResultCode resultCode = AuthResultCode::RejectedByUser;
        std::string resultMsg;

        if (isIpRateLimited(rateKey)) {
            accepted = false;
            resultCode = AuthResultCode::RateLimited;
            resultMsg = "Too many failed password attempts. Locked for 60s.";
        } else if (hasPassword == AUTH_MODE_REBOOT_TOKEN) {
            std::string tokenHex = CryptoUtils::toHex(clientDigest.data(), clientDigest.size());
#if defined(_WIN32)
            if (WindowsServiceManager::validateAndConsumeRebootToken(tokenHex, viewerId)) {
#else
            if (false) {
#endif
                recordAuthResultForIp(rateKey, true);
                accepted = true;
                grantedPerms = PERM_ALL;
                resultCode = AuthResultCode::Accepted;
                resultMsg = "Authenticated via Reboot Resume Token";
            } else {
                recordAuthResultForIp(rateKey, false);
                accepted = false;
                resultCode = AuthResultCode::InvalidPassword;
                resultMsg = "Invalid or expired reboot resume token";
            }
            CryptoUtils::secureZero(tokenHex.data(), tokenHex.size());
        } else if (hasPassword == AUTH_MODE_PAIRED) {
            // Must stay before the password branch: a paired login never falls through to password checks.
            const auto peer = identity_.findPairedViewer(viewerId);
            const bool pairedOk = sigPresent && peer && !viewerPubBlob.empty() &&
                PairingKey::verify(peer->publicKey,
                                   CryptoUtils::pairingTranscript('V', identity_.deskId(), viewerId, nonce,
                                                                  hostEcdh.localPublicKey(), viewerPubBlob),
                                   viewerSig);
            recordAuthResultForIp(rateKey, pairedOk);
            if (pairedOk) {
                accepted = true;
                pairedLogin = true;
                grantedPerms = identity_.settings().defaultPermissions;
                resultCode = AuthResultCode::Accepted;
                resultMsg = "Authenticated via pairing";
            } else {
                accepted = false;
                resultCode = AuthResultCode::InvalidPassword;
                resultMsg = "This PC is not paired with the host.";
            }
        } else if (hasPassword == AUTH_MODE_PASSWORD_BOUND) {
            // Unbound password mode 1 is removed; only ECDH-bound PBKDF2 digests are accepted.
            const bool digestOk = identity_.verifyChallengeResponse(
                viewerId, nonce, clientDigest, hostSessionKey_, challengeSalt, challengeIterations);
            if (digestOk) {
                // Unattended password verified successfully!
                if (identity_.settings().totpEnabled && identity_.totpManager().isConfigured()) {
                    // Host requires Two-Factor Authentication (encrypted over active AES-GCM session)
                    bool totpVerified = false;
                    int totpAttempts = 0;
                    const int maxTotpAttempts = 3;

                    // Send initial AUTH_RESULT with TotpRequired (encrypted)
                    {
                        ByteWriter w;
                        w.writeU8(static_cast<uint8_t>(AuthResultCode::TotpRequired));
                        w.writeU8(0);
                        w.writeString("Two-Factor Authentication Required");
                        if (!sendHostEncryptedPacket(PacketType::AUTH_RESULT, 0, w.buffer().data(), w.buffer().size())) {
                            cleanup();
                            return;
                        }
                    }

                    // Set 45-second socket timeout for TOTP verification
                    setSocketTimeoutMs(toWinSock(clientSock), 45000);

                    while (totpAttempts < maxTotpAttempts && running_.load()) {
                        if (!recvFrame(clientSock, hdr, handshakePayload, &hostRecvCipher_, &hostRecvSeq)) {
                            recordAuthResultForIp(rateKey, false);
                            cleanup();
                            return;
                        }
                        if (static_cast<PacketType>(hdr.type) != PacketType::TOTP_VERIFY) {
                            recordAuthResultForIp(rateKey, false);
                            cleanup();
                            return;
                        }

                        ByteReader totpReader(handshakePayload);
                        std::string codeStr = totpReader.readString();
                        uint64_t currentUnixTime = static_cast<uint64_t>(std::time(nullptr));

                        if (identity_.totpManager().verifyCode(codeStr, currentUnixTime, 1)) {
                            totpVerified = true;
                            break;
                        } else {
                            totpAttempts++;
                            recordAuthResultForIp(rateKey, false);
                            if (isIpRateLimited(rateKey)) {
                                break;
                            }
                            if (totpAttempts < maxTotpAttempts) {
                                ByteWriter w;
                                w.writeU8(static_cast<uint8_t>(AuthResultCode::TotpInvalid));
                                w.writeU8(0);
                                w.writeString("Invalid Two-Factor Authentication Code. Please try again.");
                                if (!sendHostEncryptedPacket(PacketType::AUTH_RESULT, 0, w.buffer().data(), w.buffer().size())) {
                                    cleanup();
                                    return;
                                }
                            }
                        }
                    }

                    if (totpVerified) {
                        recordAuthResultForIp(rateKey, true);
                        accepted = true;
                        grantedPerms = identity_.settings().defaultPermissions;
                        resultCode = AuthResultCode::Accepted;
                        resultMsg = "Authenticated via Unattended Password and TOTP 2FA";
                    } else {
                        accepted = false;
                        resultCode = isIpRateLimited(rateKey) ? AuthResultCode::RateLimited : AuthResultCode::InvalidPassword;
                        resultMsg = (resultCode == AuthResultCode::RateLimited)
                            ? "Too many failed authentication attempts. Locked for 60s."
                            : "Two-Factor Authentication failed (exceeded attempts or invalid code)";
                    }
                } else {
                    // Standard unattended access without 2FA
                    recordAuthResultForIp(rateKey, true);
                    accepted = true;
                    grantedPerms = identity_.settings().defaultPermissions;
                    resultCode = AuthResultCode::Accepted;
                    resultMsg = "Authenticated via Encrypted Challenge-Response";
                }
            } else {
                recordAuthResultForIp(rateKey, false);
                accepted = false;
                resultCode = isIpRateLimited(rateKey) ? AuthResultCode::RateLimited : AuthResultCode::InvalidPassword;
                resultMsg = (resultCode == AuthResultCode::RateLimited)
                    ? "Too many failed password attempts. Locked for 60s."
                    : "Invalid Unattended or Session Password";
            }
        } else if (hasPassword != AUTH_MODE_NONE) {
            recordAuthResultForIp(rateKey, false);
            accepted = false;
            resultCode = AuthResultCode::ProtocolError;
            resultMsg = "Unsupported authentication mode";
        } else if (autoAcceptIncoming_.load() || identity_.settings().autoAcceptIncoming) {
            accepted = true;
            grantedPerms = identity_.settings().defaultPermissions;
            resultCode = AuthResultCode::Accepted;
            resultMsg = "Accepted automatically";
        } else {
            // Interactive approval via UI popup modal!
            setSocketTimeoutMs(toWinSock(clientSock), 0);
            sendHostEncryptedPacket(PacketType::AUTH_WAITING, 0, nullptr, 0);

            {
                std::lock_guard<std::mutex> lock(approvalMutex_);
                pendingReq_.active = true;
                pendingReq_.callerDeskId = viewerId;
                pendingReq_.callerHostname = viewerHostname;
                pendingReq_.callerIp = clientIp;
                pendingReq_.proposedPermissions = identity_.settings().defaultPermissions;
                pendingReq_.callerPicture = viewerPicture.value_or(std::vector<uint8_t>{});
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

        // 4. Send AUTH_RESULT (encrypted)
        {
            ByteWriter w;
            w.writeU8(static_cast<uint8_t>(resultCode));
            w.writeU8(grantedPerms);
            w.writeString(resultMsg);
            if (!sendHostEncryptedPacket(PacketType::AUTH_RESULT, 0, w.buffer().data(), w.buffer().size())) {
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

        // Clear handshake timeout; AES-256-GCM session cipher is already active.
        setSocketTimeoutMs(toWinSock(clientSock), 0);
        setSocketSendTimeoutMs(toWinSock(clientSock), 10000);
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
            hostStatus_.paired = pairedLogin;
        }
        if (viewerPicture) storePeerPicture(viewerId, *viewerPicture);
        {
            ByteWriter w;
            writeWirePicture(w, ownPicture());
            sendHostEncryptedPacket(PacketType::PROFILE_PICTURE, 0, w.buffer().data(), w.buffer().size());
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
        capturer.setRestoreToken(shareHub_.restoreToken());
        uint64_t appliedCloakGen = privacyCloakGen_.load(std::memory_order_acquire);
        applyPrivacyCloakConfig(capturer.privacyCloak());
#if defined(__APPLE__)
        // The screen stream carries the Mac's sound; a source supplied by a test replaces it.
        if (!sourceFactory_) capturer.setSoundSink([this](const int16_t* pcm, size_t frames) { sendHostSoundChunk(pcm, frames); });
#endif
        shareHub_.setNotice({});   // the last session's notice is old news
        struct ShareNoticeReset {   // the notice belongs to this session, except a share that was refused or ended: the host keeps seeing why
            wayland::ShareHub& hub;
            ~ShareNoticeReset() { if (!wayland::isShareEnd(hub.notice().state)) hub.setNotice({}); }
        } shareNoticeReset{shareHub_};
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

        hostAudioSuspended_.store(false);   // a new session starts with sound, whatever the last viewer chose
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
        size_t outboxBytes = 0;
        std::vector<std::vector<uint8_t>> outboxBufferPool; // Recycled payload buffers
        uint64_t totalBytesSent = 0;
        constexpr size_t MAX_OUTBOX_ITEMS = 256;
        constexpr size_t MAX_OUTBOX_BYTES = 16 * 1024 * 1024;

        auto enqueueHostPacketMove = [&](PacketType pt, uint8_t flags, std::vector<uint8_t>&& payload, bool isVideo) {
            std::lock_guard<std::mutex> lk(outboxMutex);
            if (!sessionAlive.load()) return;
            const bool coalescableTelemetry =
                (pt == PacketType::PERFORMANCE_HUD_METRICS || pt == PacketType::CURSOR_UPDATE);
            if (isVideo) {
                for (auto it = outbox.begin(); it != outbox.end(); ++it) {
                    if (it->isVideoTile) {
                        outboxBytes -= it->payload.size();
                        outbox.erase(it);
                        break;
                    }
                }
            } else if (coalescableTelemetry) {
                for (auto it = outbox.begin(); it != outbox.end(); ++it) {
                    if (it->type == pt) {
                        outboxBytes -= it->payload.size();
                        outbox.erase(it);
                        break;
                    }
                }
            }
            if (outbox.size() >= MAX_OUTBOX_ITEMS || outboxBytes + payload.size() > MAX_OUTBOX_BYTES) {
                if (isVideo || coalescableTelemetry) {
                    return;
                }
                // Non-droppable packets cannot queue further; peer is stalled, tear down the session.
                sessionAlive.store(false);
                outboxCv.notify_all();
                return;
            }
            outboxBytes += payload.size();
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
                    outboxBytes -= item.payload.size();
                    outbox.pop_front();
                }

                uint64_t sendT0 = nowTickMs();
                if (!sendHostEncryptedPacket(item.type, item.flags, item.payload.data(), item.payload.size())) {
                    sessionAlive.store(false);
                    break;
                }

                totalBytesSent += item.payload.size();

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
                if (!recvFrame(clientSock, rhdr, rpay, &hostRecvCipher_, &hostRecvSeq)) {
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
                                std::lock_guard<std::mutex> sinkLock(hostKeySinkMutex_);
                                if (hostKeySink_) hostKeySink_(vk, isDown != 0);
                                else InputInjector::injectKeyEvent(vk, sc, isDown != 0, isExt != 0);
                            }
                            break;
                        }
                        case PacketType::INPUT_RELEASE_ALL: {
                            if (perms & PERM_INPUT) {
                                std::lock_guard<std::mutex> sinkLock(hostKeySinkMutex_);
                                if (hostKeySink_) hostKeySink_(0, false);
                                else InputInjector::releaseAllModifiers();
                            }
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
#if defined(_WIN32)
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
#else
                                RemoteRebootConfirmPayload failConfirm{};
                                failConfirm.accepted = false;
                                failConfirm.message = "Remote reboot not supported on this platform";
                                std::vector<uint8_t> cBuf;
                                serializeRemoteRebootConfirm(failConfirm, cBuf);
                                sendHostEncryptedPacket(PacketType::REMOTE_REBOOT_CONFIRM, 0, cBuf.data(), cBuf.size());
#endif
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
                            if (deserializeVirtualDisplayCmd(rpay.data(), rpay.size(), cmd)) {
                                VirtualDisplayStatusPayload status{};
                                status.displayId = cmd.displayId;
                                status.width = cmd.width;
                                status.height = cmd.height;
                                status.refreshRate = cmd.refreshRate;
                                status.isVirtual = 1;
                                status.isHeadlessFallback = 0;
                                status.driverBackend = 0;

                                if ((perms & PERM_INPUT) == 0) {
                                    status.statusCode = static_cast<uint8_t>(VirtualDisplayStatusCode::PermissionDenied);
                                    status.message = "Input permission required for virtual display control";
                                    std::vector<uint8_t> stBytes;
                                    serializeVirtualDisplayStatus(status, stBytes);
                                    sendHostEncryptedPacket(PacketType::VIRTUAL_DISPLAY_STATUS, 0, stBytes.data(), stBytes.size());
                                    break;
                                }

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
                            requestedQuality.store(q > 3 ? 0 : q);
                            if (fps > 0) requestedTargetFps.store(clampTargetFps(fps));
                            adaptiveFpsEnabled.store(adap != 0);
                            forceKeyframeFlag.store(true);
                            break;
                        }
                        case PacketType::VIDEO_CONTROL_REQ: {
                            uint8_t q = r.readU8();
                            int32_t monIdx = r.readI32();
                            uint8_t reqKf = r.readU8();
                            requestedQuality.store(q > 3 ? 0 : q);
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
                            if (perms & PERM_CLIPBOARD) {
                                if (r.hasRemaining(sizeof(ClipboardFileCancelHeader))) {
                                    ClipboardFileCancelHeader cnlHdr{};
                                    r.readBytes(&cnlHdr, sizeof(cnlHdr));
                                    clipFileMgr_.handleFileCancel(cnlHdr.transferId, cnlHdr.reasonCode);
                                }
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
                            if (perms & PERM_FILE_TRANSFER) {
                                uint32_t tid = r.readU32();
                                fileManager_.handleFileCancel(tid);
                            }
                            break;
                        }
                        case PacketType::CHAT_MESSAGE: {
                            std::string sender = r.readString();
                            std::string msg = r.readString();
                            if (!msg.empty()) {
                                {
                                    std::lock_guard<std::mutex> cl(chatMutex_);
                                    appendChatMessageLocked({ sender, msg, false, nowTickMs() });
                                }
                                unreadChatCount_.fetch_add(1);
                            }
                            break;
                        }
                        case PacketType::CHAT_MEDIA_MESSAGE: {
                            ChatMediaPayload p{};
                            if (deserializeChatMedia(rpay.data(), rpay.size(), p) &&
                                p.jpegData.size() <= 2 * 1024 * 1024) {
                                {
                                    std::lock_guard<std::mutex> cl(chatMutex_);
                                    appendChatMessageLocked({ p.senderName, p.captionText, false, p.timestampMs, true, std::move(p.jpegData), p.imgWidth, p.imgHeight });
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
                            // Only decides whether this viewer is sent sound, so it needs no permission.
                            if (r.hasRemaining(sizeof(AudioControlPayload))) {
                                AudioControlPayload p{};
                                r.readBytes(&p, sizeof(p));
                                hostAudioSuspended_.store(p.isMuted != 0);
                            }
                            break;
                        }
                        case PacketType::VOICE_INTERCOM_CHUNK: {
                            if (perms & PERM_INPUT) {
                                handleIncomingVoiceChunk(rpay.data(), rpay.size(), SessionTarget::Host);
                            }
                            break;
                        }
                        case PacketType::TUNNEL_OPEN: {
                            if (r.hasRemaining(sizeof(TunnelOpenHeader))) {
                                TunnelOpenHeader toh{};
                                r.readBytes(&toh, sizeof(toh));

                                bool permitted = (perms & PERM_INPUT) != 0;
                                bool duplicateId = false;
                                bool ok = false;
                                if (permitted) {
                                    bool atLimit = false;
                                    {
                                        std::lock_guard<std::mutex> lk(tunnelMutex_);
                                        auto it = hostTunnels_.find(toh.tunnelId);
                                        if (it != hostTunnels_.end() && !it->second.closeRequested) {
                                            duplicateId = true;
                                        } else if (hostTunnels_.size() >= MAX_ACTIVE_TUNNELS) {
                                            atLimit = true;
                                        }
                                    }
                                    if (!duplicateId && !atLimit) {
                                        if (!hostTunnelActive_.load()) {
                                            startHostTunnelProxy();
                                        }
                                        SOCKET ts = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                                        if (ts != INVALID_SOCKET) {
                                            socket_set_nonblocking(ts, true);
                                            socket_set_nosigpipe(ts);
                                            sockaddr_in targetAddr{};
                                            targetAddr.sin_family = AF_INET;
                                            targetAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                                            targetAddr.sin_port = htons(toh.targetPort);
                                            int rc = connect(ts, reinterpret_cast<sockaddr*>(&targetAddr), sizeof(targetAddr));
                                            const bool inProgress = (rc == SOCKET_ERROR && socket_is_wouldblock(socket_errno()));
                                            if (rc == 0 || inProgress) {
                                                std::lock_guard<std::mutex> lk(tunnelMutex_);
                                                auto it = hostTunnels_.find(toh.tunnelId);
                                                if (it != hostTunnels_.end()) {
                                                    duplicateId = true;
                                                    socket_close(ts);
                                                } else if (hostTunnels_.size() >= MAX_ACTIVE_TUNNELS) {
                                                    socket_close(ts);
                                                } else {
                                                    ActiveTunnel at;
                                                    at.tunnelId = toh.tunnelId;
                                                    at.ruleId = 0;
                                                    at.sock = ts;
                                                    at.targetPort = toh.targetPort;
                                                    at.connecting = inProgress;
                                                    at.connectStartMs = nowTickMs();
                                                    hostTunnels_.emplace(toh.tunnelId, std::move(at));
                                                    ok = true;
                                                }
                                            } else {
                                                socket_close(ts);
                                            }
                                        }
                                    }
                                }
                                if (!ok && !duplicateId) {
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
                                        size_t sentNow = 0;
                                        std::lock_guard<std::mutex> lk(tunnelMutex_);
                                        auto it = hostTunnels_.find(tdh.tunnelId);
                                        if (it != hostTunnels_.end()) {
                                            enqueueTunnelWriteLocked(it->second, r.currentPtr(), tdh.dataLen, sentNow);
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
                                    it->second.closeRequested = true;
                                    socket_shutdown(it->second.sock);
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
                                            if (!hostTerminalActive_.load()) {
                                                startHostTerminal(false);
                                            }
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
                            if (perms & PERM_INPUT) {
                                AnnotationStroke stroke{};
                                if (WhiteboardManager::deserializeStroke(r.currentPtr(), r.remaining(), stroke)) {
                                    whiteboardMgr_.showHostOverlay();
                                    whiteboardMgr_.applyRemoteStroke(stroke);
                                }
                            }
                            break;
                        }
                        case PacketType::DIAGNOSTICS_REQ: {
                            if ((perms & PERM_INPUT) == 0) { hostDiagnosticsStreamActive_.store(false); break; }
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
                        case PacketType::PROCESS_LAUNCH_REQ: {
                            ProcessLaunchReqPayload req;
                            if (deserializeProcessLaunchReq(rpay.data(), rpay.size(), req)) {
                                ProcessLaunchRespPayload resp;
                                resp.launchId = req.launchId;
                                uint32_t outPid = 0;
                                std::string outErr;
                                bool ok = executeProcessLaunch(req.commandLine, req.workingDir, perms, outPid, outErr);
                                resp.success = ok ? 1 : 0;
                                resp.pid = outPid;
                                resp.statusMessage = ok ? ("Launched PID " + std::to_string(outPid)) : outErr;
                                std::vector<uint8_t> respBuf;
                                serializeProcessLaunchResp(resp, respBuf);
                                sendPacketHelper(PacketType::PROCESS_LAUNCH_RESP, respBuf);
                            }
                            break;
                        }
                        case PacketType::FILE_BROWSER_LIST_REQ: {
                            if (perms & PERM_FILE_TRANSFER) {
                                FileBrowserListReqPayload req;
                                if (deserializeFileBrowserListReq(rpay.data(), rpay.size(), req)) {
                                    FileBrowserListRespPayload resp;
                                    resp.requestId = req.requestId;
                                    std::string outCanon, outErr;
                                    bool ok = RemoteFileBrowserManager::listLocalDirectory(req.path, resp.entries, outCanon, outErr);
                                    resp.statusCode = ok ? 0 : 1;
                                    resp.currentPath = outCanon;
                                    resp.errorMessage = outErr;
                                    std::vector<uint8_t> respBuf;
                                    serializeFileBrowserListResp(resp, respBuf);
                                    sendPacketHelper(PacketType::FILE_BROWSER_LIST_RESP, respBuf);
                                }
                            } else {
                                // Answer a denied request so the viewer's pane does not wait on it forever.
                                FileBrowserListReqPayload req;
                                if (deserializeFileBrowserListReq(rpay.data(), rpay.size(), req)) {
                                    FileBrowserListRespPayload resp;
                                    resp.requestId = req.requestId;
                                    resp.statusCode = 1;
                                    resp.currentPath = req.path;
                                    resp.errorMessage = "File access is not permitted for this session";
                                    std::vector<uint8_t> respBuf;
                                    serializeFileBrowserListResp(resp, respBuf);
                                    sendPacketHelper(PacketType::FILE_BROWSER_LIST_RESP, respBuf);
                                }
                            }
                            break;
                        }
                        case PacketType::FILE_BROWSER_ACTION_REQ: {
                            if (perms & PERM_FILE_TRANSFER) {
                                FileBrowserActionReqPayload req;
                                if (deserializeFileBrowserActionReq(rpay.data(), rpay.size(), req)) {
                                    FileBrowserActionRespPayload resp;
                                    resp.actionId = req.actionId;
                                    resp.actionType = req.actionType;
                                    std::string outErr;
                                    bool ok = false;
                                    if (req.actionType == 1) { // CreateDir
                                        ok = RemoteFileBrowserManager::createLocalDirectory(req.targetPath, outErr);
                                    } else if (req.actionType == 2) { // Delete
                                        ok = RemoteFileBrowserManager::deleteLocalItem(req.targetPath, outErr);
                                    } else if (req.actionType == 3) { // Rename
                                        ok = RemoteFileBrowserManager::renameLocalItem(req.targetPath, req.newOrDestName, outErr);
                                    }
                                    resp.statusCode = ok ? 0 : 1;
                                    resp.message = ok ? "Success" : outErr;
                                    std::vector<uint8_t> respBuf;
                                    serializeFileBrowserActionResp(resp, respBuf);
                                    sendPacketHelper(PacketType::FILE_BROWSER_ACTION_RESP, respBuf);
                                }
                            } else {
                                FileBrowserActionReqPayload req;
                                if (deserializeFileBrowserActionReq(rpay.data(), rpay.size(), req)) {
                                    FileBrowserActionRespPayload resp;
                                    resp.actionId = req.actionId;
                                    resp.actionType = req.actionType;
                                    resp.statusCode = 1;
                                    resp.message = "File access is not permitted for this session";
                                    std::vector<uint8_t> respBuf;
                                    serializeFileBrowserActionResp(resp, respBuf);
                                    sendPacketHelper(PacketType::FILE_BROWSER_ACTION_RESP, respBuf);
                                }
                            }
                            break;
                        }
                        case PacketType::FILE_BROWSER_DOWNLOAD_REQ: {
                            FileBrowserDownloadReqPayload req;
                            if (deserializeFileBrowserDownloadReq(rpay.data(), rpay.size(), req)) {
                                FileBrowserActionRespPayload resp;
                                resp.actionId = req.requestId;
                                resp.actionType = 0;
                                resp.statusCode = 1;
                                if (!(perms & PERM_FILE_TRANSFER)) {
                                    resp.message = "File access is not permitted for this session";
                                } else {
#if defined(_WIN32)
                                    std::filesystem::path dlPath(utf8ToWide(req.remoteFilePath));
#else
                                    std::filesystem::path dlPath(req.remoteFilePath);
#endif
                                    uint32_t tid = fileManager_.startOutgoingFile(dlPath, [&](PacketType pt, const std::vector<uint8_t>& pbuf) {
                                        return sendPacketHelper(pt, pbuf);
                                    });
                                    if (tid != 0) resp.statusCode = 0;
                                    else resp.message = "Cannot send '" + req.remoteFilePath + "' (not a regular file, too large, or unreadable)";
                                }
                                // Success is signalled by the FILE_OFFER itself; only failures need an answer.
                                if (resp.statusCode != 0) {
                                    std::vector<uint8_t> respBuf;
                                    serializeFileBrowserActionResp(resp, respBuf);
                                    sendPacketHelper(PacketType::FILE_BROWSER_ACTION_RESP, respBuf);
                                }
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
                                if (deserializeSyncScanReq(rpay.data(), rpay.size(), req)) {
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
                                if (deserializeSyncHashReq(rpay.data(), rpay.size(), req)) {
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
                                if (deserializeSyncDeltaBlock(rpay.data(), rpay.size(), block)) {
                                    fileSyncMgr_.handleDeltaBlock(block);
                                }
                            }
                            break;
                        }
                        case PacketType::SYNC_ACTION_REQ: {
                            if (perms & PERM_FILE_TRANSFER) {
                                SyncActionReqPayload req;
                                if (deserializeSyncActionReq(rpay.data(), rpay.size(), req)) {
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
                            if (perms & PERM_FILE_TRANSFER) {
                                SyncStatusUpdatePayload status;
                                if (deserializeSyncStatusUpdate(rpay.data(), rpay.size(), status)) {
                                    fileSyncMgr_.handleStatusUpdate(status);
                                }
                            }
                            break;
                        }
                        case PacketType::VERSION_UPGRADE_NOTICE: {
                            VersionUpgradeNoticePayload notice;
                            if (deserializeVersionUpgradeNotice(rpay.data(), rpay.size(), notice)) {
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
                            if (deserializeScratchpadUpdate(rpay.data(), rpay.size(), pad)) {
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
                                if (deserializeCommandPaletteExec(rpay.data(), rpay.size(), execReq)) {
                                    uint32_t cmdId = execReq.commandId;
                                    uint64_t sessionGen = hostSessionGen_.load();
                                    std::string runCmd = execReq.customCommand.empty() ? execReq.commandKey : execReq.customCommand;
                                    commandPaletteMgr_.executeAsync(runCmd, [this, cmdId, sessionGen](int32_t exitCode, const std::string& output) {
                                        if (!running_.load() || hostSessionGen_.load() != sessionGen) {
                                            return;
                                        }
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
                        case PacketType::PAIR_REQUEST: {
                            // Reached only after login. The viewer offers its key + proof of possession;
                            // view-only or reduced-permission viewers cannot escalate via pairing.
                            PairingKey::PublicKey viewerKey{};
                            PairingKey::Signature popSig{};
                            std::string viewerName;
                            bool wellFormed = false;
                            try {
                                wellFormed = r.readU16() == viewerKey.size() && r.hasRemaining(viewerKey.size());
                                if (wellFormed) {
                                    r.readBytes(viewerKey.data(), viewerKey.size());
                                    viewerName = r.readString();
                                    wellFormed = viewerName.size() <= 255 &&
                                                 r.hasRemaining(2) &&
                                                 r.readU16() == popSig.size() &&
                                                 r.hasRemaining(popSig.size());
                                    if (wellFormed) {
                                        r.readBytes(popSig.data(), popSig.size());
                                    }
                                }
                            } catch (...) { wellFormed = false; }
                            std::array<uint8_t, 32> sessionKeyCopy{};
                            {
                                std::lock_guard<std::mutex> lock(hostCipherMutex_);
                                sessionKeyCopy = hostSessionKey_;
                            }
                            const bool popValid = wellFormed &&
                                CryptoUtils::isUsableKey(sessionKeyCopy) &&
                                PairingKey::verify(viewerKey, pairPopDigest(sessionKeyCopy, viewerId, viewerKey), popSig);
                            CryptoUtils::secureZero(sessionKeyCopy.data(), sessionKeyCopy.size());

                            const bool allowed = identity_.settings().allowPairing;   // checked now, not at login
                            const uint8_t requiredPerms = static_cast<uint8_t>(identity_.settings().defaultPermissions | PERM_INPUT);
                            const bool hasPerms = (perms & requiredPerms) == requiredPerms;
                            // viewerId is only a claim in a password login: it must not overwrite another PC's pinned key.
                            const auto existing = identity_.findPairedViewer(viewerId);
                            const bool replacesOtherKey = popValid && !pairedLogin && existing && existing->publicKey != viewerKey;
                            const bool accept = popValid && hasPerms && allowed && !replacesOtherKey;
                            if (accept) {
                                identity_.addPairedViewer({viewerId, viewerName, viewerKey, static_cast<uint64_t>(std::time(nullptr))});
                            }
                            ByteWriter pw;
                            pw.writeU8(accept ? 1 : 0);
                            if (accept) {
                                const auto& hostKey = identity_.pairingKey().publicKey();
                                pw.writeU16(static_cast<uint16_t>(hostKey.size()));
                                pw.writeBytes(hostKey.data(), hostKey.size());
                            } else {
                                pw.writeU16(0);
                            }
                            pw.writeString(accept ? "Paired" : (!allowed ? "This PC is not accepting new pairings."
                                : !hasPerms ? "Insufficient session permissions to pair."
                                : replacesOtherKey ? "Already paired; forget it on the host first." : "Invalid pairing request."));
                            enqueueHostPacket(PacketType::PAIR_RESULT, 0, pw.buffer(), false);
                            break;
                        }
                        case PacketType::PROFILE_PICTURE:
                            if (auto pic = readWirePicture(r)) storePeerPicture(viewerId, *pic);
                            break;
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

            const uint64_t curCloakGen = privacyCloakGen_.load(std::memory_order_acquire);
            if (curCloakGen != appliedCloakGen) {
                appliedCloakGen = curCloakGen;
                applyPrivacyCloakConfig(capturer.privacyCloak());
            }
            const bool gotFrame = capturer.captureDirtyTiles(wantKf, preset, dirtyTiles, isKf, curState);
            shareHub_.setNotice(capturer.shareNotice());
            if (std::string shareToken; capturer.takeRestoreTokenChange(shareToken)) shareHub_.pushToken(shareToken);
            if (const wayland::ShareNotice ended = capturer.shareNotice(); wayland::isShareEnd(ended.state)) {
                // Refused, or ended on this desktop: tell the viewer why and end the session. A viewer that is merely
                // cut off would reconnect and ask this desktop's question again.
                ByteWriter bye;
                bye.writeString(wayland::endMessageForViewer(ended.state));
                sendHostEncryptedPacket(PacketType::DISCONNECT, 0, bye.buffer().data(), bye.buffer().size());
                break;
            }
            if (gotFrame) {
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
                                      (effectiveFps >= 45) ? 22 :
                                      (effectiveFps >= 30) ? 33 : 66;
            if (elapsed < targetInterval) {
                std::this_thread::sleep_for(std::chrono::milliseconds(targetInterval - elapsed));
            }
        }

        sessionAlive.store(false);
        hostEncrypted_.store(false);
        outboxCv.notify_all();
        // Wake readerThread and sendWorkerThread without closing yet; cleanup() closes clientSock once after joins.
        {
            uintptr_t expected = clientSock;
            activeHostClientSock_.compare_exchange_strong(expected, ~uintptr_t(0));
            socket_shutdown(toSocket(clientSock));
        }
        if (sendWorkerThread.joinable()) {
            try { sendWorkerThread.join(); } catch (...) {}
        }
        if (readerThread.joinable()) {
            try { readerThread.join(); } catch (...) {}
        }

        if (identity_.settings().lockWorkstationOnDisconnect) {
#if defined(_WIN32)
            LockWorkStation();
#endif
        }
    } catch (...) {}

    cleanup();
}

// ---------------- Viewer Session Client ----------------

bool NetworkEngine::connectToRemote(const std::string& targetIdOrAddr, const std::string& password, uint64_t requirePairedDeskId) {
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
        pairingState_.store(PairingRequestState::Idle);
        viewerStats_.state = ViewerConnectionState::ResolvingId;
        viewerStats_.statusMessage = "Resolving address...";
        viewerStats_.qualityPreset = identity_.settings().defaultQuality;
        viewerStats_.targetFps = clampTargetFps(identity_.settings().targetFps);
        viewerStats_.effectiveFpsCap = viewerStats_.targetFps;
        viewerStats_.adaptiveFps = identity_.settings().adaptiveFps;
    }

    viewerThread_ = std::thread(&NetworkEngine::runViewerSession, this, targetIdOrAddr, password, requirePairedDeskId);
    return true;
}

void NetworkEngine::disconnectViewer() {
    viewerActive_.store(false);
    viewerTunnelActive_.store(false);
    stopVoiceIntercom(SessionTarget::Viewer);
    viewerPrivacyModeActive_.store(false);
    whiteboardMgr_.clearAllStrokes();
    clipFileMgr_.reset();
    uintptr_t s = viewerSock_.exchange(~uintptr_t(0));
    if (s != ~uintptr_t(0)) {
        bool enc = viewerEncrypted_.exchange(false);
        if (enc) {
            std::unique_lock<std::mutex> lock(viewerCipherMutex_, std::try_to_lock);
            if (lock.owns_lock() && viewerCipher_.isInitialized()) {
                sendFrame(s, PacketType::DISCONNECT, 0, nullptr, 0, viewerSendMutex_,
                          &viewerCipher_, &viewerSendSeq_);
            }
        }
        socket_t sock = toSocket(s);
        if (sock != INVALID_SOCKET_VAL) {
            linger l{1, 0};
            setsockopt(sock, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
            // The viewer session thread exclusively owns closing its socket once woken.
            socket_shutdown(sock);
        }
    }
    fileManager_.abortActiveTransfers();
    {
        std::lock_guard<std::mutex> lock(viewerTotpMutex_);
        viewerTotpCancelled_ = true;
        viewerTotpSubmitted_ = false;
        viewerTotpCv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(viewerThreadMutex_);
        if (viewerThread_.joinable()) {
            if (std::this_thread::get_id() != viewerThread_.get_id()) {
                viewerThread_.join();
            }
        }
    }
    shutdownViewerAudioPlayback();
    stopViewerTunnelMultiplexer();
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        viewerStats_ = ViewerSessionStats{};
        viewerStats_.state = ViewerConnectionState::Disconnected;
        viewerStats_.statusMessage = "Disconnected";
    }
    {
        std::lock_guard<std::mutex> lock(viewerFrameMutex_);
        viewerCanvasBgra_.clear();
        viewerCanvasBgra_.shrink_to_fit();
        viewerCanvasW_ = 0;
        viewerCanvasH_ = 0;
        viewerFrameSeq_ = 0;
        viewerLastConsumedSeq_ = 0;
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

bool NetworkEngine::requestPairing() {
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        if (viewerStats_.state != ViewerConnectionState::Connected || !viewerStats_.offerPairing) return false;
    }
    const PairingKey& key = identity_.pairingKey();
    if (!key.isValid()) return false;
    PairingRequestState idleOrDone = pairingState_.load();
    if (idleOrDone == PairingRequestState::Pending || idleOrDone == PairingRequestState::Paired) return false;

    std::array<uint8_t, 32> sessionKeyCopy{};
    {
        std::lock_guard<std::mutex> lock(viewerCipherMutex_);
        if (!viewerEncrypted_.load()) return false;
        sessionKeyCopy = viewerSessionKey_;
    }
    PairingKey::Signature popSig{};
    const bool signedOk = CryptoUtils::isUsableKey(sessionKeyCopy) &&
        key.sign(pairPopDigest(sessionKeyCopy, identity_.deskId(), key.publicKey()), popSig);
    CryptoUtils::secureZero(sessionKeyCopy.data(), sessionKeyCopy.size());
    if (!signedOk) return false;

    ByteWriter w;
    w.writeU16(static_cast<uint16_t>(key.publicKey().size()));
    w.writeBytes(key.publicKey().data(), key.publicKey().size());
    w.writeString(identity_.wireName());
    w.writeU16(static_cast<uint16_t>(popSig.size()));
    w.writeBytes(popSig.data(), popSig.size());
    pairingState_.store(PairingRequestState::Pending);   // before the send: the answer may beat us back
    if (!sendViewerEncryptedPacket(PacketType::PAIR_REQUEST, 0, w.buffer().data(), w.buffer().size())) {
        pairingState_.store(PairingRequestState::Idle);
        return false;
    }
    return true;
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

    bool missedFrames = (inOutSeq > 0 && inOutSeq != viewerLastConsumedSeq_);
    bool resChanged = (outW != viewerCanvasW_ || outH != viewerCanvasH_ || outBgra.size() != viewerCanvasBgra_.size());
    inOutSeq = viewerFrameSeq_;
    outW = viewerCanvasW_;
    outH = viewerCanvasH_;

    if (resChanged || viewerFullCanvasDirty_ || missedFrames) {
        outBgra = viewerCanvasBgra_;
        if (outDirtyBounds) {
            *outDirtyBounds = RECT{0, 0, static_cast<LONG>(outW), static_cast<LONG>(outH)};
        }
        viewerFullCanvasDirty_ = false;
        viewerDirtyBounds_ = RECT{0, 0, 0, 0};
    } else {
        RECT r = viewerDirtyBounds_;
        r.left = std::clamp<LONG>(r.left, 0, static_cast<LONG>(outW));
        r.right = std::clamp<LONG>(r.right, 0, static_cast<LONG>(outW));
        r.top = std::clamp<LONG>(r.top, 0, static_cast<LONG>(outH));
        r.bottom = std::clamp<LONG>(r.bottom, 0, static_cast<LONG>(outH));
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
        viewerDirtyBounds_ = RECT{0, 0, 0, 0};
    }
    viewerLastConsumedSeq_ = viewerFrameSeq_;
    return true;
}

void NetworkEngine::injectViewerFrameForTest(const std::vector<uint8_t>& bgra, int width, int height) {
    std::lock_guard<std::mutex> lock(viewerFrameMutex_);
    viewerCanvasBgra_ = bgra;
    viewerCanvasW_ = width;
    viewerCanvasH_ = height;
    ++viewerFrameSeq_;
    viewerFullCanvasDirty_ = true;
    viewerDirtyBounds_ = RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
}

void NetworkEngine::markViewerCanvasFullDirty() {
    std::lock_guard<std::mutex> lock(viewerFrameMutex_);
    viewerFullCanvasDirty_ = true;
    if (viewerCanvasW_ > 0 && viewerCanvasH_ > 0) {
        viewerDirtyBounds_ = RECT{0, 0, static_cast<LONG>(viewerCanvasW_), static_cast<LONG>(viewerCanvasH_)};
    }
}

void NetworkEngine::setOnFrameDecodedCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(onFrameDecodedMutex_);
    onFrameDecoded_ = std::move(cb);
}

void NetworkEngine::runViewerSession(std::string targetInput, std::string password, uint64_t requirePairedDeskId) {
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
    uintptr_t activeVSock = ~uintptr_t(0);

    auto doConnectAndAuth = [&](bool isReconnecting) -> bool {
        SOCKET connectedSock = INVALID_SOCKET;

        if (targetDeskId > 0) {
            if (isReconnecting && !resolvedIp.empty() && resolvedIp.rfind("Relay:", 0) != 0) {
                connectedSock = connectTcpWithTimeout(resolvedIp, resolvedPort, 1200);
            }

            if (connectedSock == INVALID_SOCKET) {
                resolvedIp.clear();
                resolvedPort = DEFAULT_HOST_PORT;
                if (targetDeskId == identity_.deskId()) {
                    resolvedIp = "127.0.0.1";
                    resolvedPort = identity_.listenPort();
                    connectedSock = connectTcpWithTimeout(resolvedIp, resolvedPort, 1500);
                }
            }

            if (connectedSock == INVALID_SOCKET) {
                resolvedIp.clear();
                resolvedPort = DEFAULT_HOST_PORT;
                sendDiscoveryQuery(targetDeskId);
                for (int waitStep = 0; waitStep < (isReconnecting ? 4 : 6) && viewerActive_.load(); ++waitStep) {
                    const uint64_t now = nowTickMs();
                    {
                        std::lock_guard<std::mutex> lock(peersMutex_);
                        for (const auto& p : peers_) {
                            if (p.deskId == targetDeskId && (now - p.lastSeenTickMs <= 12000)) {
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
                resolvedIp.clear();
                resolvedPort = DEFAULT_HOST_PORT;
                std::string rHost;
                uint16_t rPort = DEFAULT_RELAY_PORT;
                if (parseHostPort(identity_.relayServerAddress(), rHost, rPort, DEFAULT_RELAY_PORT)) {
                    const std::string relayAuthKey = identity_.settings().relayAuthKey;
                    auto sendRelayFrame = [&](uintptr_t targetSock, PacketType type, const std::vector<uint8_t>& body, std::mutex& mu) {
                        if (relayAuthKey.empty()) {
                            return sendFrame(targetSock, type, 0, body.data(), body.size(), mu);
                        }
                        std::vector<uint8_t> tagged = body;
                        relayowner::Key tag = relayowner::computeAuthTag(relayAuthKey, static_cast<uint8_t>(type), body.data(), body.size());
                        tagged.insert(tagged.end(), tag.begin(), tag.end());
                        return sendFrame(targetSock, type, relayowner::FLAG_RELAY_AUTH, tagged.data(), tagged.size(), mu);
                    };

                    if (!isReconnecting) {
                        setStatus(ViewerConnectionState::ResolvingId, "Querying Relay Server (" + rHost + ":" + std::to_string(rPort) + ")...");
                    }
                    bool relayLookupFound = false;
                    SOCKET ls = connectTcpWithTimeout(rHost, rPort, 1500);
                    if (ls != INVALID_SOCKET) {
                        setSocketTimeoutMs(ls, 5000);
                        uintptr_t lSock = fromWinSock(ls);
                        viewerSock_.store(lSock);
                        ByteWriter lw;
                        lw.writeU64(targetDeskId);
                        std::mutex lm;
                        if (sendRelayFrame(lSock, PacketType::RELAY_LOOKUP, lw.buffer(), lm)) {
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
                                        relayLookupFound = true;
                                    }
                                } catch (...) {}
                            }
                        }
                        viewerSock_.store(~uintptr_t(0));
                        closeWinSock(lSock);
                    }

                    if (relayLookupFound && !resolvedIp.empty()) {
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
                            setSocketTimeoutMs(bs, 5000);
                            uintptr_t bSock = fromWinSock(bs);
                            viewerSock_.store(bSock);
                            ByteWriter bw;
                            bw.writeU64(targetDeskId);
                            std::mutex bm;
                            if (sendRelayFrame(bSock, PacketType::RELAY_CONNECT_REQ, bw.buffer(), bm)) {
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
                                viewerSock_.store(~uintptr_t(0));
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
        setSocketTimeoutMs(connectedSock, 8000);
        viewerSock_.store(vSock);
        viewerEncrypted_.store(false);

        auto abortHandshake = [&]() {
            viewerEncrypted_.store(false);
            {
                std::lock_guard<std::mutex> lock(viewerCipherMutex_);
                viewerCipher_.reset();
                viewerRecvCipher_.reset();
                CryptoUtils::secureZero(viewerSessionKey_.data(), viewerSessionKey_.size());
            }
            closeWinSock(vSock);
            viewerSock_.store(~uintptr_t(0));
        };

        try {
            if (!isReconnecting) {
                setStatus(ViewerConnectionState::Authenticating, "Performing cryptographic handshake...");
            }

            // Initialize Ephemeral ECDH (NIST P-256) for forward secrecy
            EcdhKeyExchange viewerEcdh;
            if (!viewerEcdh.initialize() || viewerEcdh.localPublicKey().empty()) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Failed to initialize key exchange.");
                }
                abortHandshake();
                return false;
            }

            // 1. Send HELLO
            {
                ByteWriter w;
                w.writeU16(PROTOCOL_VERSION);
                w.writeU64(identity_.deskId());
                w.writeString(identity_.wireName());
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
                    abortHandshake();
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
                abortHandshake();
                return false;
            }

            if (static_cast<PacketType>(hdr.type) == PacketType::AUTH_RESULT) {
                ByteReader r(payload);
                r.readU8(); r.readU8();
                std::string msg = r.readString();
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, msg.empty() ? "Host busy or rate-limited." : msg);
                }
                abortHandshake();
                return false;
            }

            if (static_cast<PacketType>(hdr.type) != PacketType::AUTH_CHALLENGE) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Unexpected handshake packet.");
                }
                abortHandshake();
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
            // Missing capability byte or missing AUTH_CAP_BOUND_RESPONSE must fail closed, never downgrade.
            if (!chalReader.hasRemaining(1)) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Host authentication challenge missing required capabilities.");
                }
                abortHandshake();
                return false;
            }
            const uint8_t hostAuthCaps = chalReader.readU8();
            if ((hostAuthCaps & AUTH_CAP_BOUND_RESPONSE) == 0) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Host does not support bound authentication.");
                }
                abortHandshake();
                return false;
            }

            // Pairing: check the host's key and session signature before sending any credential.
            const bool hostAdvertisesPairing = (hostAuthCaps & AUTH_CAP_PAIRING) != 0;
            uint8_t pairFlags = 0;
            viewerHostPairingKey_ = {};
            viewerHostKeyVerified_ = false;
            if (hostAdvertisesPairing && chalReader.hasRemaining(1 + 2 + 64 + 2 + 64)) {
                PairingKey::Signature hostSig{};
                pairFlags = chalReader.readU8();
                bool lengthsOk = chalReader.readU16() == viewerHostPairingKey_.size();
                chalReader.readBytes(viewerHostPairingKey_.data(), viewerHostPairingKey_.size());
                lengthsOk = chalReader.readU16() == hostSig.size() && lengthsOk;
                chalReader.readBytes(hostSig.data(), hostSig.size());
                viewerHostKeyVerified_ = lengthsOk && PairingKey::verify(viewerHostPairingKey_,
                    CryptoUtils::pairingTranscript('H', remoteId, identity_.deskId(), nonce,
                                                   hostPubBlob, viewerEcdh.localPublicKey()),
                    hostSig);
            }
            // Read mandatory PBKDF2 salt + iteration count from AUTH_CHALLENGE.
            std::array<uint8_t, 16> challengeSalt{};
            uint32_t challengeIterations = 0;
            if (!chalReader.hasRemaining(challengeSalt.size() + sizeof(challengeIterations))) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Host challenge missing KDF parameters.");
                }
                abortHandshake();
                return false;
            }
            chalReader.readBytes(challengeSalt.data(), challengeSalt.size());
            challengeIterations = chalReader.readU32();
            if (challengeIterations < CryptoUtils::kMinPbkdf2Iterations ||
                challengeIterations > CryptoUtils::kMaxPbkdf2Iterations) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Host sent invalid KDF parameters.");
                }
                abortHandshake();
                return false;
            }

            // A dialled desk ID is resolved over unauthenticated discovery: if it is pinned, the PC that answers must
            // claim that same ID, or a rogue could answer for it under another ID and dodge the pin.
            const bool pinnedIdAnsweredByOther =
                targetDeskId > 0 && remoteId != targetDeskId && identity_.findPairedHost(targetDeskId).has_value();
            ViewerPairingDecision pairing = pinnedIdAnsweredByOther
                ? ViewerPairingDecision::RefuseIdentityChanged
                : decideViewerPairing(hostAdvertisesPairing, viewerHostKeyVerified_,
                                      identity_.findPairedHost(remoteId), viewerHostPairingKey_);
            // A required paired PC (dialled by a spoofable ip:port) must be exactly that PC, logged in with its keys.
            if (requirePairedDeskId != 0 && (remoteId != requirePairedDeskId || pairing == ViewerPairingDecision::Legacy))
                pairing = ViewerPairingDecision::RefuseIdentityChanged;
            if (pairing == ViewerPairingDecision::RefuseIdentityChanged ||
                pairing == ViewerPairingDecision::RefuseNoPairingSupport) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, pairing == ViewerPairingDecision::RefuseIdentityChanged
                        ? "This PC's identity doesn't match the one you paired with, so the connection was refused. If it was reinstalled, forget the pairing and pair again."
                        : "This PC no longer offers pairing, so the connection was refused. Forget the pairing to connect with a password.");
                }
                abortHandshake();
                return false;
            }

            // Activate mandatory AES-256-GCM encryption before sending AUTH_RESPONSE, reboot tokens, or TOTP codes.
            if (hostPubBlob.empty() ||
                !viewerEcdh.computeSharedSessionKey(
                    hostPubBlob.data(), hostPubBlob.size(),
                    remoteId, identity_.deskId(), nonce, viewerSessionKey_) ||
                !CryptoUtils::isUsableKey(viewerSessionKey_)) {
                if (!isReconnecting) {
                    setStatus(ViewerConnectionState::Error, "Failed to establish secure session.");
                }
                abortHandshake();
                return false;
            }
            std::string sasFingerprint;
            {
                std::lock_guard<std::mutex> lock(viewerCipherMutex_);
                if (!viewerCipher_.initialize(viewerSessionKey_, /*isHost=*/false) ||
                    !viewerRecvCipher_.initialize(viewerSessionKey_, /*isHost=*/false)) {
                    abortHandshake();
                    return false;
                }
                viewerSendSeq_ = 0;
                viewerEncrypted_.store(true);
                sasFingerprint = CryptoUtils::sessionFingerprintHex(viewerSessionKey_);
            }
            uint64_t viewerRecvSeq = 0;

            // 3. Compute & send AUTH_RESPONSE (encrypted)
            {
                ByteWriter w;
                std::string tokenCopy;
                {
                    std::lock_guard<std::mutex> lock(rebootTokenMutex_);
                    if (autoReconnectingWithToken_.load()) {
                        tokenCopy = rebootResumeToken_;
                    }
                }
                if (pairing == ViewerPairingDecision::UsePaired) {
                    // A pinned host overrides a typed password: the password is never sent to it.
                    PairingKey::Signature sig{};
                    if (!identity_.pairingKey().sign(
                            CryptoUtils::pairingTranscript('V', remoteId, identity_.deskId(), nonce,
                                                           hostPubBlob, viewerEcdh.localPublicKey()), sig)) {
                        if (!isReconnecting) {
                            setStatus(ViewerConnectionState::Error, "Failed to establish secure session.");
                        }
                        abortHandshake();
                        return false;
                    }
                    w.writeU8(AUTH_MODE_PAIRED);
                    std::array<uint8_t, 32> zero{};
                    w.writeBytes(zero.data(), zero.size());
                    w.writeU16(static_cast<uint16_t>(sig.size()));
                    w.writeBytes(sig.data(), sig.size());
                } else if (!tokenCopy.empty()) {
                    w.writeU8(AUTH_MODE_REBOOT_TOKEN);
                    auto tokenBytes = CryptoUtils::fromHex(tokenCopy);
                    if (tokenBytes.size() == 32) {
                        w.writeBytes(tokenBytes.data(), 32);
                    } else {
                        std::array<uint8_t, 32> tb{};
                        std::memcpy(tb.data(), tokenBytes.data(), std::min<size_t>(32, tokenBytes.size()));
                        w.writeBytes(tb.data(), 32);
                    }
                    CryptoUtils::secureZero(tokenCopy.data(), tokenCopy.size());
                    if (!tokenBytes.empty()) CryptoUtils::secureZero(tokenBytes.data(), tokenBytes.size());
                } else if (!password.empty()) {
                    auto digest = CryptoUtils::computeChallengeResponse(
                        password, challengeSalt, challengeIterations, remoteId, identity_.deskId(), nonce);
                    w.writeU8(AUTH_MODE_PASSWORD_BOUND);
                    digest = CryptoUtils::bindChallengeResponse(viewerSessionKey_, digest);
                    w.writeBytes(digest.data(), digest.size());
                } else {
                    w.writeU8(AUTH_MODE_NONE);
                    std::array<uint8_t, 32> zero{};
                    w.writeBytes(zero.data(), zero.size());
                }
                writeWirePicture(w, ownPicture());   // trailing field: 1.0 hosts stop reading before it
                if (!sendViewerEncryptedPacket(PacketType::AUTH_RESPONSE, 0, w.buffer().data(), w.buffer().size())) {
                    if (!isReconnecting) {
                        setStatus(ViewerConnectionState::Error, "Failed to send authentication response.");
                    }
                    abortHandshake();
                    return false;
                }
            }

            // 4. Receive AUTH_WAITING and/or AUTH_RESULT (encrypted)
            while (viewerActive_.load()) {
                if (!recvFrame(vSock, hdr, payload, &viewerRecvCipher_, &viewerRecvSeq)) {
                    if (!isReconnecting) {
                        setStatus(ViewerConnectionState::Error, "Connection closed during authorization.");
                    }
                    abortHandshake();
                    return false;
                }
                PacketType pt = static_cast<PacketType>(hdr.type);
                if (pt == PacketType::AUTH_WAITING) {
                    setSocketTimeoutMs(toWinSock(vSock), 50000);
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
                        setSocketTimeoutMs(toWinSock(vSock), 50000);
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
                                    abortHandshake();
                                    return false;
                                }
                                codeToSend = viewerTotpCode_;
                            }

                            // Send PacketType::TOTP_VERIFY (encrypted over active AES-GCM session)
                            ByteWriter tw;
                            tw.writeString(codeToSend);
                            if (!sendViewerEncryptedPacket(PacketType::TOTP_VERIFY, 0, tw.buffer().data(), tw.buffer().size())) {
                                setStatus(ViewerConnectionState::Error, "Failed to send 2FA verification code.");
                                abortHandshake();
                                return false;
                            }

                            // Receive next result packet (encrypted)
                            if (!recvFrame(vSock, hdr, payload, &viewerRecvCipher_, &viewerRecvSeq)) {
                                setStatus(ViewerConnectionState::Error, "Connection closed during 2FA verification.");
                                abortHandshake();
                                return false;
                            }

                            if (static_cast<PacketType>(hdr.type) == PacketType::AUTH_RESULT) {
                                ByteReader r2(payload);
                                code = static_cast<AuthResultCode>(r2.readU8());
                                perms = r2.readU8();
                                msg = r2.readString();

                                if (code == AuthResultCode::Accepted) {
                                    // Successfully verified! Break out and proceed to connected state
                                    break;
                                } else if (code == AuthResultCode::TotpInvalid) {
                                    // Code was wrong, loop again to prompt user with error caption
                                    continue;
                                } else {
                                    // Locked out, rate-limited, or host error
                                    setStatus(ViewerConnectionState::Error, msg.empty() ? "2FA verification failed." : msg);
                                    abortHandshake();
                                    return false;
                                }
                            }
                        }
                    }

                    if (code != AuthResultCode::Accepted) {
                        if (!isReconnecting) {
                            setStatus(ViewerConnectionState::Error, msg.empty() ? "Authentication rejected." : msg);
                        }
                        abortHandshake();
                        return false;
                    }

                    // Clear handshake timeout once connected; AES-256-GCM cipher is already active.
                    setSocketTimeoutMs(toWinSock(vSock), 0);
                    setSocketSendTimeoutMs(toWinSock(vSock), 10000);

                    int activeMonIdx = 0;
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
                        viewerStats_.pairedSession = (pairing == ViewerPairingDecision::UsePaired);
                        viewerStats_.hostAllowsNewPairing = hostAdvertisesPairing && (pairFlags & PAIR_FLAG_ALLOW_NEW);
                        viewerStats_.offerPairing = viewerStats_.hostAllowsNewPairing && viewerHostKeyVerified_ &&
                            (perms & PERM_INPUT) != 0 &&
                            !viewerStats_.pairedSession && !identity_.findPairedHost(remoteId);
                        pairingState_.store(PairingRequestState::Idle);
                        activeMonIdx = viewerStats_.activeMonitorIndex;
                    }
                    // Viewer-only engines share one identity across threads; its recents list and save() are unlocked.
                    if (!viewerOnly_.load()) {
                        identity_.addOrUpdateRecentSession(remoteId, remoteHost, targetInput);
                        initViewerAudioPlayback();
                    }
                    syncAudioControl();   // the host learns this viewer's mute and volume now, and again after a reconnect
                    startViewerTunnelMultiplexer();

                    requestVideoSettings(
                        identity_.settings().defaultQuality,
                        activeMonIdx,
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
                            notice.downloadUrl = "https://github.com/oocsng/cppdesk/releases/latest";
                            notice.message = "Connected viewer is running CppDesk " + std::string(CPP_DESK_VERSION) + ". Host update recommended.";
                            std::vector<uint8_t> noticeBuf;
                            serializeVersionUpgradeNotice(notice, noticeBuf);
                            sendViewerEncryptedPacket(PacketType::VERSION_UPGRADE_NOTICE, 0, noticeBuf.data(), noticeBuf.size());
                        }
                    }
                    activeVSock = vSock;
                    return true;
                }
            }
        } catch (...) {}

        abortHandshake();
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

        uintptr_t vSock = activeVSock;
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

                pollfd_t pfd{};
                pfd.fd = ws;
                pfd.events = POLLIN;
                int sel = socketPoll(&pfd, 1, 20);
                if (sel < 0) {
                    socketDropped = true;
                    break;
                }
                if (sel == 0) continue;

                FrameHeader hdr{};
                std::vector<uint8_t> payload;
                if (!recvFrame(vSock, hdr, payload, &viewerRecvCipher_, &viewerRecvSeq)) {
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
                            if (fw < 32 || fw > 8192 || fh < 32 || fh > 8192) {
                                break;
                            }
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


                                std::vector<uint8_t> fresh(static_cast<size_t>(fw) * fh * 4, 0);
                                viewerCanvasBgra_.swap(fresh);
                                viewerCanvasW_ = fw;
                                viewerCanvasH_ = fh;
    
    
                                viewerFullCanvasDirty_ = true;
                                viewerDirtyBounds_ = RECT{0, 0, static_cast<LONG>(fw), static_cast<LONG>(fh)};
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
                            if (fw < 32 || fw > 8192 || fh < 32 || fh > 8192) break;

                            auto decStart = std::chrono::steady_clock::now();
                            {
                                std::lock_guard<std::mutex> lock(viewerFrameMutex_);
                                bool resChanged = (viewerCanvasW_ != fw || viewerCanvasH_ != fh);
                                if (resChanged) {
    
    
                                    std::vector<uint8_t> fresh(static_cast<size_t>(fw) * fh * 4, 0);
                                viewerCanvasBgra_.swap(fresh);
                                viewerCanvasW_ = fw;
                                viewerCanvasH_ = fh;
    
    
                                    viewerFullCanvasDirty_ = true;
                                    viewerDirtyBounds_ = RECT{0, 0, static_cast<LONG>(fw), static_cast<LONG>(fh)};
                                }
                                if (tileCount > 0) {
                                    thread_local std::vector<TileThreadPool::DecodeTask> decodeTasks;
                                    decodeTasks.clear();
                                    if (decodeTasks.capacity() < tileCount) {
                                        decodeTasks.reserve(tileCount);
                                    }
                                    const size_t maxFramePixels = static_cast<size_t>(fw) * fh;
                                    size_t totalTilePixels = 0;
                                    int minX = fw, minY = fh, maxX = 0, maxY = 0;
                                    for (uint16_t i = 0; i < tileCount; ++i) {
                                        if (!r.hasRemaining(sizeof(TileHeader))) break;
                                        TileHeader th{};
                                        r.readBytes(&th, sizeof(th));
                                        if (!r.hasRemaining(th.dataSize)) break;
                                        const uint8_t* tileDataPtr = r.currentPtr();
                                        r.skip(th.dataSize);

                                        if (th.width == 0 || th.height == 0 ||
                                            th.width > 1024 || th.height > 1024 ||
                                            static_cast<int>(th.x) + th.width > fw ||
                                            static_cast<int>(th.y) + th.height > fh) {
                                            continue;
                                        }
                                        const size_t tilePixels = static_cast<size_t>(th.width) * th.height;
                                        if (tilePixels > maxFramePixels - totalTilePixels) {
                                            continue;
                                        }
                                        totalTilePixels += tilePixels;

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

                                    // Detect overlapping tiles via sorted-X sweep; decode sequentially if any overlap
                                    // to prevent concurrent writes to the same viewerCanvasBgra_ pixels.
                                    bool hasOverlap = false;
                                    if (decodeTasks.size() > 1) {
                                        thread_local std::vector<size_t> order;
                                        order.resize(decodeTasks.size());
                                        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                                        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                                            return decodeTasks[a].rx < decodeTasks[b].rx;
                                        });
                                        for (size_t i = 0; i < order.size() && !hasOverlap; ++i) {
                                            const auto& a = decodeTasks[order[i]];
                                            int aRight = static_cast<int>(a.rx) + a.rw;
                                            int aBottom = static_cast<int>(a.ry) + a.rh;
                                            for (size_t j = i + 1; j < order.size(); ++j) {
                                                const auto& b = decodeTasks[order[j]];
                                                if (static_cast<int>(b.rx) >= aRight) break;
                                                int bBottom = static_cast<int>(b.ry) + b.rh;
                                                if (static_cast<int>(a.ry) < bBottom && static_cast<int>(b.ry) < aBottom) {
                                                    hasOverlap = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                    if (hasOverlap) {
                                        for (const auto& t : decodeTasks) {
                                            TileCodec::decodeTileIntoCanvas(
                                                t.rx, t.ry, t.rw, t.rh,
                                                t.encoding, t.data, t.dataSize,
                                                t.canvasBgra, t.canvasW, t.canvasH);
                                        }
                                    } else {
                                        TileThreadPool::instance().parallelDecode(decodeTasks);
                                    }
                                    if (!resChanged) {
                                        if (viewerFullCanvasDirty_) {
                                            viewerDirtyBounds_ = RECT{0, 0, static_cast<LONG>(fw), static_cast<LONG>(fh)};
                                        } else {
                                            if (maxX > minX && maxY > minY) {
                                                LONG nLeft = static_cast<LONG>(std::clamp(minX, 0, static_cast<int>(fw)));
                                                LONG nTop = static_cast<LONG>(std::clamp(minY, 0, static_cast<int>(fh)));
                                                LONG nRight = static_cast<LONG>(std::clamp(maxX, 0, static_cast<int>(fw)));
                                                LONG nBottom = static_cast<LONG>(std::clamp(maxY, 0, static_cast<int>(fh)));
                                                if (viewerDirtyBounds_.right > viewerDirtyBounds_.left && viewerDirtyBounds_.bottom > viewerDirtyBounds_.top) {
                                                    viewerDirtyBounds_.left = std::min(viewerDirtyBounds_.left, nLeft);
                                                    viewerDirtyBounds_.top = std::min(viewerDirtyBounds_.top, nTop);
                                                    viewerDirtyBounds_.right = std::max(viewerDirtyBounds_.right, nRight);
                                                    viewerDirtyBounds_.bottom = std::max(viewerDirtyBounds_.bottom, nBottom);
                                                } else {
                                                    viewerDirtyBounds_ = RECT{ nLeft, nTop, nRight, nBottom };
                                                }
                                            } else if (viewerDirtyBounds_.right <= viewerDirtyBounds_.left) {
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
                        case PacketType::PAIR_RESULT: {
                            if (pairingState_.load() != PairingRequestState::Pending) break;   // nothing was asked
                            bool stored = false;
                            try {
                                PairingKey::PublicKey hostKey{};
                                const bool accepted = r.readU8() != 0;
                                const bool keyLen = r.readU16() == hostKey.size() && r.hasRemaining(hostKey.size());
                                if (accepted && keyLen) {
                                    r.readBytes(hostKey.data(), hostKey.size());
                                    // Only the key this host proved in the handshake is ever stored.
                                    if (viewerHostKeyVerified_ && hostKey == viewerHostPairingKey_) {
                                        uint64_t pairedId = 0;
                                        std::string pairedName;
                                        {
                                            std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                            pairedId = viewerStats_.remoteDeskId;
                                            pairedName = viewerStats_.remoteHostname;
                                        }
                                        identity_.addPairedHost({pairedId, pairedName, hostKey, static_cast<uint64_t>(std::time(nullptr))});
                                        stored = true;
                                    }
                                }
                            } catch (...) {}
                            if (stored) {
                                std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                viewerStats_.offerPairing = false;
                            }
                            pairingState_.store(stored ? PairingRequestState::Paired : PairingRequestState::Refused);
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
                            // Viewer-only: the host's copied files never take over this PC's clipboard.
                            if (!viewerOnly_.load() && deserializeClipboardFileList(r.currentPtr(), r.remaining(), tid, files)) {
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
                            // Viewer-only (Fleet) engines never accept unsolicited host file drops, and
                            // interactive viewers always save into the sandboxed downloads directory (never Desktop).
                            if (!viewerOnly_.load()) {
                                uint32_t tid = r.readU32();
                                uint64_t fsz = r.readU64();
                                std::string fname = r.readString();
                                float dropNx = 0.0f, dropNy = 0.0f;
                                if (r.hasRemaining(1)) {
                                    (void)r.readU8(); // ignore remote targetHint; never write to viewer Desktop
                                }
                                if (r.hasRemaining(8)) {
                                    dropNx = r.readF32();
                                    dropNy = r.readF32();
                                }
                                fileManager_.handleFileOffer(tid, fsz, fname, FileOfferTarget::DefaultDownloads, dropNx, dropNy);
                            }
                            break;
                        }
                        case PacketType::FILE_CHUNK: {
                            if (!viewerOnly_.load()) {
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
                            if (!viewerOnly_.load()) {
                                uint32_t tid = r.readU32();
                                std::string sha = r.readString();
                                fileManager_.handleFileComplete(tid, sha);
                            }
                            break;
                        }
                        case PacketType::FILE_CANCEL: {
                            if (!viewerOnly_.load()) {
                                uint32_t tid = r.readU32();
                                fileManager_.handleFileCancel(tid);
                            }
                            break;
                        }
                        case PacketType::CHAT_MESSAGE: {
                            std::string sender = r.readString();
                            std::string msg = r.readString();
                            if (!msg.empty()) {
                                {
                                    std::lock_guard<std::mutex> cl(chatMutex_);
                                    appendChatMessageLocked({ sender, msg, false, nowTickMs() });
                                }
                                unreadChatCount_.fetch_add(1);
                            }
                            break;
                        }
                        case PacketType::CHAT_MEDIA_MESSAGE: {
                            ChatMediaPayload p{};
                            if (deserializeChatMedia(payload.data(), payload.size(), p) &&
                                p.jpegData.size() <= 2 * 1024 * 1024) {
                                {
                                    std::lock_guard<std::mutex> cl(chatMutex_);
                                    appendChatMessageLocked({ p.senderName, p.captionText, false, p.timestampMs, true, std::move(p.jpegData), p.imgWidth, p.imgHeight });
                                }
                                unreadChatCount_.fetch_add(1);
                            }
                            break;
                        }
                        case PacketType::AUDIO_STREAM_CHUNK: {
                            if (!viewerOnly_.load()) enqueueViewerAudioChunk(payload.data(), payload.size());
                            break;
                        }
                        case PacketType::VOICE_INTERCOM_CHUNK: {
                            handleIncomingVoiceChunk(payload.data(), payload.size(), SessionTarget::Viewer);
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
                                    std::lock_guard<std::mutex> lk(tunnelMutex_);
                                    auto it = viewerTunnels_.find(tdh.tunnelId);
                                    if (it != viewerTunnels_.end()) {
                                        size_t sentNow = 0;
                                        enqueueTunnelWriteLocked(it->second, tdata, tlen, sentNow);
                                        if (sentNow > 0) {
                                            uint32_t rId = it->second.ruleId;
                                            for (auto& r : tunnelRules_) {
                                                if (r.ruleId == rId) {
                                                    r.bytesTransferredIn += sentNow;
                                                    break;
                                                }
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
                                    it->second.closeRequested = true;
                                    socket_shutdown(it->second.sock);
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
                                    constexpr size_t kMaxTerminalLineBytes = 4096;
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
                                            if (terminalLines_.back().size() >= kMaxTerminalLineBytes) {
                                                terminalLines_.push_back("");
                                                if (terminalLines_.size() > 500) {
                                                    terminalLines_.erase(terminalLines_.begin());
                                                }
                                            }
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
                        case PacketType::PROCESS_LAUNCH_RESP: {
                            ProcessLaunchRespPayload resp{};
                            if (deserializeProcessLaunchResp(payload.data(), payload.size(), resp)) {
                                std::lock_guard<std::mutex> lk(processLaunchMutex_);
                                latestProcessLaunchResp_ = std::move(resp);
                                hasNewProcessLaunchResp_ = true;
                            }
                            break;
                        }
                        case PacketType::FILE_BROWSER_LIST_RESP: {
                            FileBrowserListRespPayload resp{};
                            if (deserializeFileBrowserListResp(payload.data(), payload.size(), resp)) {
                                std::lock_guard<std::mutex> lk(fileBrowserMutex_);
                                latestRemoteFileList_ = std::move(resp);
                                hasNewRemoteFileList_ = true;
                            }
                            break;
                        }
                        case PacketType::FILE_BROWSER_ACTION_RESP: {
                            FileBrowserActionRespPayload resp{};
                            if (deserializeFileBrowserActionResp(payload.data(), payload.size(), resp)) {
                                std::lock_guard<std::mutex> lk(fileBrowserMutex_);
                                latestRemoteFileAction_ = std::move(resp);
                                hasNewRemoteFileAction_ = true;
                            }
                            break;
                        }
                        case PacketType::PROFILE_PICTURE:
                            if (auto pic = readWirePicture(r)) {
                                uint64_t hostDeskId = 0;
                                {
                                    std::lock_guard<std::mutex> lock(viewerStatsMutex_);
                                    hostDeskId = viewerStats_.remoteDeskId;
                                }
                                storePeerPicture(hostDeskId, *pic);
                            }
                            break;
                        case PacketType::DISCONNECT:
                            // The host may say why it ended the session (an older host sends nothing).
                            if (!payload.empty()) {
                                try {
                                    const std::string why = r.readString();
                                    if (!why.empty()) setStatus(ViewerConnectionState::Error, why);
                                } catch (...) {}
                            }
                            viewerActive_.store(false);
                            break;
                        default:
                            break;
                    }
                } catch (...) {}
            }
        } catch (...) {}

        viewerEncrypted_.store(false);
        viewerSock_.store(~uintptr_t(0));
        closeWinSock(activeVSock);
        {
            std::lock_guard<std::mutex> lock(viewerCipherMutex_);
            viewerCipher_.reset();
            viewerRecvCipher_.reset();
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

    viewerEncrypted_.store(false);
    viewerSock_.store(~uintptr_t(0));
    closeWinSock(activeVSock);
    shutdownViewerAudioPlayback();
    stopViewerTunnelMultiplexer();
    viewerPrivacyModeActive_.store(false);
    whiteboardMgr_.clearAllStrokes();
    {
        std::lock_guard<std::mutex> lock(viewerCipherMutex_);
        viewerCipher_.reset();
        viewerRecvCipher_.reset();
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

uint32_t NetworkEngine::sendFile(const std::string& filePath, FileOfferTarget targetHint, float dropNx, float dropNy, SessionTarget target) {
    return sendFilePath(std::filesystem::path(filePath), targetHint, dropNx, dropNy, target);
}

uint32_t NetworkEngine::sendFilePath(const std::filesystem::path& filePath, FileOfferTarget targetHint, float dropNx, float dropNy, SessionTarget target) {
    SessionTarget resolved = target;
    if (resolved == SessionTarget::Auto) {
        if (viewerSock_.load() != ~uintptr_t(0)) resolved = SessionTarget::Viewer;
        else if (activeHostClientSock_.load() != ~uintptr_t(0)) resolved = SessionTarget::Host;
        else return 0;
    }
    if (resolved == SessionTarget::Viewer && viewerSock_.load() != ~uintptr_t(0)) {
        return fileManager_.startOutgoingFile(filePath, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendViewerEncryptedPacket(pt, 0, buf.data(), buf.size());
        }, targetHint, dropNx, dropNy);
    } else if (resolved == SessionTarget::Host && activeHostClientSock_.load() != ~uintptr_t(0)) {
        return fileManager_.startOutgoingFile(filePath, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendHostEncryptedPacket(pt, 0, buf.data(), buf.size());
        }, targetHint, dropNx, dropNy);
    }
    return 0;
}

int NetworkEngine::sendDropPath(const std::string& path, FileOfferTarget targetHint, float dropNx, float dropNy, SessionTarget target) {
    SessionTarget resolved = target;
    if (resolved == SessionTarget::Auto) {
        if (viewerSock_.load() != ~uintptr_t(0)) resolved = SessionTarget::Viewer;
        else if (activeHostClientSock_.load() != ~uintptr_t(0)) resolved = SessionTarget::Host;
        else return 0;
    }
    if (resolved == SessionTarget::Viewer && viewerSock_.load() != ~uintptr_t(0)) {
        return fileManager_.startOutgoingPath(path, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendViewerEncryptedPacket(pt, 0, buf.data(), buf.size());
        }, targetHint, dropNx, dropNy);
    } else if (resolved == SessionTarget::Host && activeHostClientSock_.load() != ~uintptr_t(0)) {
        return fileManager_.startOutgoingPath(path, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendHostEncryptedPacket(pt, 0, buf.data(), buf.size());
        }, targetHint, dropNx, dropNy);
    }
    return 0;
}

bool NetworkEngine::cancelFileTransfer(uint32_t transferId, SessionTarget target) {
    SessionTarget resolved = target;
    if (resolved == SessionTarget::Auto) {
        if (viewerSock_.load() != ~uintptr_t(0)) resolved = SessionTarget::Viewer;
        else if (activeHostClientSock_.load() != ~uintptr_t(0)) resolved = SessionTarget::Host;
    }
    if (resolved == SessionTarget::Viewer && viewerSock_.load() != ~uintptr_t(0)) {
        return fileManager_.cancelTransfer(transferId, [this](PacketType pt, const std::vector<uint8_t>& buf) {
            return sendViewerEncryptedPacket(pt, 0, buf.data(), buf.size());
        });
    } else if (resolved == SessionTarget::Host && activeHostClientSock_.load() != ~uintptr_t(0)) {
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

bool NetworkEngine::requestRemoteScan(const std::string& remotePath, SessionTarget target) {
    fileSyncMgr_.setRemoteRoot(remotePath);
    SyncScanReqPayload req;
    req.scanId = static_cast<uint32_t>(GetTickCount64());
    req.rootPath = remotePath;
    req.flags = 0x01; // Recursive
    std::vector<uint8_t> buf;
    serializeSyncScanReq(req, buf);
    return sendToSessionTarget(target, PacketType::SYNC_SCAN_REQ, 0, buf.data(), buf.size());
}

bool NetworkEngine::requestRemoteBlockHashes(const std::string& relativePath, SessionTarget target) {
    SyncHashReqPayload req;
    req.syncId = 1;
    req.fileId = 1;
    req.relativePath = relativePath;
    std::vector<uint8_t> buf;
    serializeSyncHashReq(req, buf);
    return sendToSessionTarget(target, PacketType::SYNC_HASH_REQ, 0, buf.data(), buf.size());
}

bool NetworkEngine::startFolderSync(const std::string& localPath, const std::string& remotePath, SyncMode mode, bool purge) {
    fileSyncMgr_.setLocalRoot(localPath);
    fileSyncMgr_.setRemoteRoot(remotePath);
    fileSyncMgr_.setSyncMode(mode);
    fileSyncMgr_.setMirrorPurge(purge);
    return fileSyncMgr_.startSync();
}

void NetworkEngine::cancelFolderSync(SessionTarget target) {
    fileSyncMgr_.cancelSync();
    SyncActionReqPayload req;
    req.actionId = static_cast<uint32_t>(GetTickCount64());
    req.actionType = static_cast<uint8_t>(SyncActionType::AbortSync);
    std::vector<uint8_t> buf;
    serializeSyncActionReq(req, buf);
    sendToSessionTarget(target, PacketType::SYNC_ACTION_REQ, 0, buf.data(), buf.size());
}

bool NetworkEngine::sendChatMessage(const std::string& text) {
    if (text.empty() || text.size() > 2048) return false;

    ByteWriter w;
    w.writeString(identity_.wireName());
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
        appendChatMessageLocked({ "You (" + (identity_.displayName().empty() ? identity_.hostname() : identity_.displayName()) + ")", text, true, nowTickMs() });
    }
    return sent;
}

void writeWirePicture(ByteWriter& w, const std::vector<uint8_t>& jpeg) {
    w.writeU16(static_cast<uint16_t>(jpeg.size()));
    if (!jpeg.empty()) w.writeBytes(jpeg.data(), jpeg.size());
}

std::optional<std::vector<uint8_t>> readWirePicture(ByteReader& r) {
    if (!r.hasRemaining(2)) return std::nullopt;
    const uint16_t len = r.readU16();
    if (len == 0) return std::vector<uint8_t>{};
    if (len > avatar::kMaxWireBytes || !r.hasRemaining(len)) return std::nullopt;
    auto jpeg = r.readBytesVector(len);
    if (!avatar::validateWireJpeg(jpeg)) return std::nullopt;
    return jpeg;
}

static std::vector<uint8_t> readPictureFile(const std::filesystem::path& path, size_t cap) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > cap) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream f(path, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return {};
    return bytes;
}

std::vector<uint8_t> NetworkEngine::ownPicture() const {
    return readPictureFile(identity_.ownPicturePath(), 0xFFFF);   // the u16 wire length; the receiver enforces the real cap
}

std::vector<uint8_t> NetworkEngine::peerPicture(uint64_t deskId) const {
    {
        std::lock_guard<std::mutex> lk(peerPictureMutex_);
        auto it = peerPictures_.find(deskId);
        if (it != peerPictures_.end()) return it->second;
    }
    auto bytes = readPictureFile(identity_.peerPicturePath(deskId), avatar::kMaxWireBytes);
    return avatar::validateWireJpeg(bytes) ? bytes : std::vector<uint8_t>{};
}

void NetworkEngine::storePeerPicture(uint64_t deskId, const std::vector<uint8_t>& jpeg) {
    const auto path = identity_.peerPicturePath(deskId);
    {
        std::lock_guard<std::mutex> lk(peerPictureMutex_);
        std::error_code ec;
        if (jpeg.empty()) {
            peerPictures_.erase(deskId);
            std::filesystem::remove(path, ec);
        } else {
            peerPictures_[deskId] = jpeg;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        }
    }
    peerPictureGeneration_.fetch_add(1);
}

void NetworkEngine::injectPeerPictureForTest(uint64_t deskId, const std::vector<uint8_t>& jpeg) {
    {
        std::lock_guard<std::mutex> lk(peerPictureMutex_);
        if (jpeg.empty()) peerPictures_.erase(deskId); else peerPictures_[deskId] = jpeg;
    }
    peerPictureGeneration_.fetch_add(1);
}

void NetworkEngine::sendProfilePicture() {
    ByteWriter w;
    writeWirePicture(w, ownPicture());
    bool hostAccepted = false, viewerConnected = false;
    {
        std::lock_guard<std::mutex> lock(hostStatusMutex_);
        hostAccepted = hostStatus_.active;
    }
    {
        std::lock_guard<std::mutex> lock(viewerStatsMutex_);
        viewerConnected = viewerStats_.state == ViewerConnectionState::Connected;
    }
    // Never before accept: a host mid-handshake would also drop the session on an unexpected packet.
    if (hostAccepted) sendHostEncryptedPacket(PacketType::PROFILE_PICTURE, 0, w.buffer().data(), w.buffer().size());
    if (viewerConnected) sendViewerEncryptedPacket(PacketType::PROFILE_PICTURE, 0, w.buffer().data(), w.buffer().size());
    peerPictureGeneration_.fetch_add(1);
}

bool NetworkEngine::sendChatImage(const std::vector<uint8_t>& jpegData, uint32_t w, uint32_t h, const std::string& caption) {
    if (jpegData.empty() || jpegData.size() > 2 * 1024 * 1024) return false;

    ChatMediaPayload p;
    p.senderName = identity_.wireName();
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
        appendChatMessageLocked({ "You (" + (identity_.displayName().empty() ? identity_.hostname() : identity_.displayName()) + ")", caption, true, p.timestampMs, true, jpegData, w, h });
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
    std::lock_guard<std::mutex> lk(privacyCurtainMutex_);
    privacyCurtainNotice_ = notice;
    privacyCurtainBrand_ = brand;
    privacyCurtainShowId_ = showId;
}

void NetworkEngine::configurePrivacyCloak(bool enabled, uint8_t style, const std::string& keywords) {
    {
        std::lock_guard<std::mutex> lk(privacyCloakMutex_);
        privacyCloakEnabled_ = enabled;
        privacyCloakStyle_ = style;
        privacyCloakKeywords_ = keywords;
    }
    privacyCloakGen_.fetch_add(1, std::memory_order_release);
}

void NetworkEngine::applyPrivacyCloakConfig(PrivacyCloakManager& cloak) const {
    bool enabled = false;
    uint8_t style = 0;
    std::string keywords;
    {
        std::lock_guard<std::mutex> lk(privacyCloakMutex_);
        if (privacyCloakGen_.load(std::memory_order_acquire) == 0) {
            const auto& s = identity_.settings();
            enabled = s.privacyCloakEnabled;
            style = s.privacyCloakStyle;
            keywords = s.privacyCloakKeywords;
        } else {
            enabled = privacyCloakEnabled_;
            style = privacyCloakStyle_;
            keywords = privacyCloakKeywords_;
        }
    }
    cloak.setEnabled(enabled);
    cloak.setRedactionStyle(style);
    cloak.setCustomKeywords(keywords);
}

void NetworkEngine::requestTogglePrivacyMode(ScreenBlankMode blankMode) {
    bool current = viewerPrivacyModeActive_.load();
    const auto& s = identity_.settings();
    if (!s.hardwareDpmsBlanking && blankMode == ScreenBlankMode::Unified) {
        blankMode = ScreenBlankMode::CurtainOnly;
    }

    std::string notice;
    std::string brand;
    bool showId = true;
    {
        std::lock_guard<std::mutex> lk(privacyCurtainMutex_);
        notice = !privacyCurtainNotice_.empty() ? privacyCurtainNotice_ : s.privacyCustomNotice;
        brand = !privacyCurtainBrand_.empty() ? privacyCurtainBrand_ : s.privacyBrandName;
        showId = privacyCurtainShowId_;
    }
    PrivacyModeConfigPayload p{};
    p.enable = current ? 0 : 1;
    p.acknowledge = 0;
    p.showDeskId = showId ? 1 : 0;
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
        std::string activeNotice;
        std::string activeBrand;
        bool activeShowId = showId;
        {
            std::lock_guard<std::mutex> lk(privacyCurtainMutex_);
            if (!notice.empty()) privacyCurtainNotice_ = notice;
            else if (privacyCurtainNotice_.empty()) privacyCurtainNotice_ = identity_.settings().privacyCustomNotice;

            if (!brand.empty()) privacyCurtainBrand_ = brand;
            else if (privacyCurtainBrand_.empty()) privacyCurtainBrand_ = identity_.settings().privacyBrandName;

            privacyCurtainShowId_ = showId;
            activeNotice = privacyCurtainNotice_;
            activeBrand = privacyCurtainBrand_;
            activeShowId = privacyCurtainShowId_;
        }

        // Auto-provision virtual display if host is headless
        if ((blankMode == ScreenBlankMode::DpmsOnly || blankMode == ScreenBlankMode::Unified) &&
            identity_.settings().autoVirtualDisplay && VirtualDisplayManager::isHostHeadless()) {
            VirtualDisplayManager::instance().ensureHeadlessDisplay(1920, 1080, 60);
        }

        // Report privacy mode as active only if the curtain really came up (it is Windows-only).
        hostPrivacyModeActive_.store(ScreenBlankManager::instance().engageBlanking(blankMode, activeNotice, activeBrand, activeShowId, identity_.deskId()));
    } else {
        ScreenBlankManager::instance().disengageBlanking();
        hostPrivacyModeActive_.store(false);
    }
}

void NetworkEngine::startHostAudioCapture() {
    std::lock_guard<std::mutex> lk(hostAudioLifecycleMutex_);
    hostAudioActive_.store(false);
    try {
        if (hostAudioThread_.joinable()) hostAudioThread_.join();
    } catch (...) {}
    hostAudioActive_.store(true);
    hostAudioThread_ = std::thread(&NetworkEngine::hostAudioCaptureLoop, this);
}

void NetworkEngine::stopHostAudioCapture() {
    std::lock_guard<std::mutex> lk(hostAudioLifecycleMutex_);
    hostAudioActive_.store(false);
    try {
        if (hostAudioThread_.joinable()) hostAudioThread_.join();
    } catch (...) {}
}

void NetworkEngine::hostAudioCaptureLoop() {
#if defined(_WIN32)
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
            } else if (pData && srcChannels > 0) {
                bool convertedOk = false;
                convertedPcm.resize(numFramesRead * 2);
                if (isFloat) {
                    const float* fData = reinterpret_cast<const float*>(pData);
                    for (UINT32 f = 0; f < numFramesRead; ++f) {
                        float left = fData[f * srcChannels];
                        float right = (srcChannels > 1) ? fData[f * srcChannels + 1] : left;
                        convertedPcm[f * 2 + 0] = static_cast<int16_t>(std::clamp(left, -1.0f, 1.0f) * 32767.0f);
                        convertedPcm[f * 2 + 1] = static_cast<int16_t>(std::clamp(right, -1.0f, 1.0f) * 32767.0f);
                    }
                    convertedOk = true;
                } else if (pwfx->wBitsPerSample == 16) {
                    const int16_t* sData = reinterpret_cast<const int16_t*>(pData);
                    for (UINT32 f = 0; f < numFramesRead; ++f) {
                        int16_t left = sData[f * srcChannels];
                        int16_t right = (srcChannels > 1) ? sData[f * srcChannels + 1] : left;
                        convertedPcm[f * 2 + 0] = left;
                        convertedPcm[f * 2 + 1] = right;
                    }
                    convertedOk = true;
                } else if (pwfx->wBitsPerSample == 32) {
                    const int32_t* iData = reinterpret_cast<const int32_t*>(pData);
                    for (UINT32 f = 0; f < numFramesRead; ++f) {
                        int32_t left = iData[f * srcChannels] >> 16;
                        int32_t right = ((srcChannels > 1) ? iData[f * srcChannels + 1] : iData[f * srcChannels]) >> 16;
                        convertedPcm[f * 2 + 0] = static_cast<int16_t>(std::clamp<int32_t>(left, -32768, 32767));
                        convertedPcm[f * 2 + 1] = static_cast<int16_t>(std::clamp<int32_t>(right, -32768, 32767));
                    }
                    convertedOk = true;
                }

                if (!convertedOk) {
                    pCaptureClient->ReleaseBuffer(numFramesRead);
                    continue;
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
#else
    // Linux captures the monitor of the default output here; macOS gets its sound with the screen stream, so it has no
    // source of its own. When nothing can be captured the session carries on without sound.
    std::unique_ptr<SystemAudioSource> source = sourceFactory_ ? sourceFactory_() : makeSystemAudioSource();
    const bool capturing = source && source->start([this](const int16_t* pcm, size_t frames) { sendHostSoundChunk(pcm, frames); });
    while (hostAudioActive_.load() && running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (capturing) source->stop();
#endif
}

void NetworkEngine::sendHostSoundChunk(const int16_t* stereo48k, size_t frames) {
    if (!stereo48k || frames == 0 || hostAudioSuspended_.load() || activeHostClientSock_.load() == ~uintptr_t(0)) return;
    AudioChunkHeader hdr{};
    hdr.sampleRate = 48000;
    hdr.channels = 2;
    hdr.bitsPerSample = 16;
    hdr.isSilent = 0;
    hdr.sampleFrames = static_cast<uint32_t>(frames);
    const size_t pcmBytes = frames * 2 * sizeof(int16_t);
    thread_local std::vector<uint8_t> packet;
    packet.resize(sizeof(hdr) + pcmBytes);
    std::memcpy(packet.data(), &hdr, sizeof(hdr));
    std::memcpy(packet.data() + sizeof(hdr), stereo48k, pcmBytes);
    sendHostEncryptedPacket(PacketType::AUDIO_STREAM_CHUNK, 0, packet.data(), packet.size());
}

void NetworkEngine::initViewerAudioPlayback() {
#if defined(_WIN32)
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
#else
    // The player is made when the first sound arrives; this only opens the session for it.
    std::lock_guard<std::mutex> lk(audioPlaybackMutex_);
    viewerAudioOpen_ = true;
    viewerPlayerTried_ = false;
    viewerConvert_ = audio::ConvertState{};
#endif
}

void NetworkEngine::shutdownViewerAudioPlayback() {
#if defined(_WIN32)
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
#else
    // Under the lock, so no sound reaches a player after this returns.
    std::lock_guard<std::mutex> lk(audioPlaybackMutex_);
    viewerAudioOpen_ = false;
    if (viewerPlayer_) {
        viewerPlayer_->stop();
        viewerPlayer_.reset();
    }
#endif
}

void NetworkEngine::enqueueViewerAudioChunk(const uint8_t* payload, size_t len) {
#if defined(_WIN32)
    if (!payload || len < sizeof(AudioChunkHeader)) return;
    if (audioMuted_.load()) return;
    int volPercent = audioVolumePercent_.load();
    if (volPercent <= 0) return;

    AudioChunkHeader hdr{};
    std::memcpy(&hdr, payload, sizeof(hdr));
    if (hdr.isSilent || hdr.sampleFrames == 0 || hdr.bitsPerSample != 16 || hdr.channels == 0) return;
    if (hdr.sampleRate < 8000 || hdr.sampleRate > 192000 || hdr.channels > 8) return;
    // waveOut is opened for 48 kHz 16-bit stereo (4 bytes per frame); drop unsupported wire formats and cap frame count.
    if (hdr.sampleRate != 48000 || hdr.channels != 2) return;

    constexpr size_t kMaxAudioFrames = 96000;
    const size_t availFrames = (len - sizeof(AudioChunkHeader)) / (2 * sizeof(int16_t));
    const size_t frames = std::min<size_t>({ static_cast<size_t>(hdr.sampleFrames), availFrames, kMaxAudioFrames });
    if (frames == 0) return;
    const size_t pcmBytes = frames * 2 * sizeof(int16_t);

    const int16_t* inSamples = reinterpret_cast<const int16_t*>(payload + sizeof(AudioChunkHeader));
    const size_t sampleCount = frames * 2;

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
#else
    if (len < sizeof(AudioChunkHeader) || audioMuted_.load()) return;
    const int volPercent = audioVolumePercent_.load();
    if (volPercent <= 0) return;
    AudioChunkHeader hdr{};
    std::memcpy(&hdr, payload, sizeof(hdr));
    if (hdr.isSilent || hdr.sampleFrames == 0 || hdr.bitsPerSample != 16 || hdr.channels == 0) return;
    // Anything outside what a sound device does is dropped, so a broken or hostile host cannot make the viewer resample
    // (and allocate) without bound.
    if (hdr.sampleRate < 8000 || hdr.sampleRate > 192000 || hdr.channels > 8) return;
    const size_t frames = std::min<size_t>({ static_cast<size_t>(hdr.sampleFrames), (len - sizeof(hdr)) / (hdr.channels * sizeof(int16_t)), size_t{96000} });
    if (frames == 0) return;

    std::lock_guard<std::mutex> lk(audioPlaybackMutex_);
    if (!viewerAudioOpen_) return;
    if (!viewerPlayer_) {
        if (viewerPlayerTried_) return;   // it could not start this session: stay silent without retrying every chunk
        viewerPlayerTried_ = true;
        auto player = playerFactory_ ? playerFactory_() : makeAudioPlayer();
        if (!player || !player->start()) return;
        viewerPlayer_ = std::move(player);
    }

    std::vector<int16_t> samples(frames * hdr.channels);
    std::memcpy(samples.data(), payload + sizeof(hdr), samples.size() * sizeof(int16_t));
    if (hdr.channels != 2 || hdr.sampleRate != 48000) {
        std::vector<int16_t> wire;
        audio::toWire({samples.data(), frames, hdr.channels, static_cast<int>(hdr.sampleRate), audio::SampleFormat::S16, false}, viewerConvert_, wire);
        samples.swap(wire);
    }
    if (samples.empty()) return;
    if (volPercent != 100) VoiceIntercom::applyGain(samples.data(), samples.size(), static_cast<float>(volPercent) / 100.0f);
    viewerPlayer_->play(samples.data(), samples.size() / 2);
#endif
}

// ---------------- Bidirectional Voice Intercom (Feature 4) ----------------

bool NetworkEngine::startVoiceIntercom(SessionTarget target) {
    SessionTarget resolved = target;
    if (resolved == SessionTarget::Auto) {
        if (viewerActive_.load() || viewerSock_.load() != ~uintptr_t(0)) resolved = SessionTarget::Viewer;
        else if (activeHostClientSock_.load() != ~uintptr_t(0)) resolved = SessionTarget::Host;
    }
    voiceIntercomTarget_.store(resolved);
    voiceIntercom_.startPlayback(48000, 1);

    return voiceIntercom_.startCapture([this, resolved](const uint8_t* pcm, size_t bytes, uint32_t sampleRate, uint8_t channels) {
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

        sendToSessionTarget(resolved, PacketType::VOICE_INTERCOM_CHUNK, 0, tl_voicePacketScratch.data(), tl_voicePacketScratch.size());
    }, 48000, 1);
}

void NetworkEngine::stopVoiceIntercom(SessionTarget target) {
    SessionTarget currentTarget = voiceIntercomTarget_.load();
    if (target != SessionTarget::Auto && currentTarget != SessionTarget::Auto && currentTarget != target) {
        return;
    }
    SessionTarget sendTarget = (target != SessionTarget::Auto) ? target : currentTarget;

    VoiceChunkHeader hdr{};
    hdr.sampleRate = 48000;
    hdr.channels = 1;
    hdr.bitsPerSample = 16;
    hdr.flags = 0x04;
    hdr.sampleFrames = 0;

    sendToSessionTarget(sendTarget, PacketType::VOICE_INTERCOM_CHUNK, 0, &hdr, sizeof(hdr));

    voiceIntercom_.stopCapture();
    voiceIntercom_.stopPlayback();
    voiceIntercomTarget_.store(SessionTarget::Auto);
    hostMicLive_.store(false);
    voiceOpenFailed_.store(false);
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

void NetworkEngine::setVoiceInputDevice(int id) {
    bool wasMonitoring = (voiceIntercom_.isCapturing() && !viewerActive_.load() && activeHostClientSock_.load() == ~uintptr_t(0));
    voiceIntercom_.setInputDeviceId(id);
    if (wasMonitoring) {
        voiceIntercom_.stopCapture();
        voiceIntercom_.startCapture(nullptr, 48000, 1);
    }
}

void NetworkEngine::setVoiceOutputDevice(int id) {
    voiceIntercom_.setOutputDeviceId(id);
}

bool NetworkEngine::startVoiceMonitor() {
    if (voiceIntercom_.isCapturing()) return true;
    return voiceIntercom_.startCapture(nullptr, 48000, 1);
}

void NetworkEngine::stopVoiceMonitor() {
    if (!viewerActive_.load() && activeHostClientSock_.load() == ~uintptr_t(0)) {
        if (voiceIntercom_.isCapturing()) {
            voiceIntercom_.stopCapture();
        }
    }
}

bool NetworkEngine::isVoiceMonitoring() const {
    return voiceIntercom_.isCapturing();
}

void NetworkEngine::handleIncomingVoiceChunk(const uint8_t* payload, size_t len, SessionTarget source) {
    if (!payload || len < sizeof(VoiceChunkHeader)) return;
    const auto* hdr = reinterpret_cast<const VoiceChunkHeader*>(payload);

    if (hdr->flags & 0x04) {
        SessionTarget activeTarget = voiceIntercomTarget_.load();
        if (source == SessionTarget::Auto || activeTarget == SessionTarget::Auto || activeTarget == source) {
            voiceIntercom_.stopCapture();
            voiceIntercom_.stopPlayback();
            voiceIntercomTarget_.store(SessionTarget::Auto);
            hostMicLive_.store(false);
            voiceOpenFailed_.store(false);
        }
        return;
    }

    const uint8_t* pcm = payload + sizeof(VoiceChunkHeader);
    size_t pcmBytes = len - sizeof(VoiceChunkHeader);
    if (pcmBytes == 0) return;

    uint32_t srate = (hdr->sampleRate >= 8000 && hdr->sampleRate <= 96000) ? hdr->sampleRate : 48000;
    uint8_t ch = (hdr->channels == 1 || hdr->channels == 2) ? hdr->channels : 1;

    if (!voiceOpenFailed_.load() && !voiceIntercom_.isPlaying() && !voiceIntercom_.startPlayback(srate, ch)) {
        voiceOpenFailed_.store(true);
    }
    voiceIntercom_.enqueuePlaybackChunk(pcm, pcmBytes, ch);

    const bool fromHostSession = (source == SessionTarget::Host) ||
                                 (source == SessionTarget::Auto && !viewerActive_.load());
    if (fromHostSession && !voiceOpenFailed_.load() && !voiceIntercom_.isCapturing() &&
        activeHostClientSock_.load() != ~uintptr_t(0)) {
        if (startVoiceIntercom(SessionTarget::Host)) hostMicLive_.store(true);
        else voiceOpenFailed_.store(true);
    }
}

// ---------------- Remote Terminal & TCP Port Forwarding (v2.1.0) ----------------

void NetworkEngine::sendTerminalCommand(const std::string& cmd) {
    if (cmd.empty()) return;
    {
        std::lock_guard<std::mutex> lk(terminalMutex_);
        terminalHistory_.push_back(cmd);
        if (terminalHistory_.size() > 50) terminalHistory_.erase(terminalHistory_.begin());
        if (!terminalLines_.empty() && terminalLines_.back().empty()) terminalLines_.back() = "> " + cmd;
        else terminalLines_.push_back("> " + cmd);
        terminalLines_.push_back("");   // the host's output starts on its own line, not after the command
        while (terminalLines_.size() > 500) terminalLines_.erase(terminalLines_.begin());
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

void NetworkEngine::sendWhiteboardStroke(const AnnotationStroke& stroke, SessionTarget target) {
    auto payload = WhiteboardManager::serializeStroke(stroke);
    sendToSessionTarget(target, PacketType::WHITEBOARD_PACKET, 0, payload.data(), payload.size());
}

void NetworkEngine::sendWhiteboardClear(SessionTarget target) {
    auto payload = WhiteboardManager::serializeClearPacket();
    sendToSessionTarget(target, PacketType::WHITEBOARD_PACKET, 0, payload.data(), payload.size());
}

void NetworkEngine::sendWhiteboardLaser(float normX, float normY, SessionTarget target) {
    AnnotationStroke stroke{};
    stroke.tool = WhiteboardTool::LaserPointer;
    stroke.points.push_back({ normX, normY });
    auto payload = WhiteboardManager::serializeStroke(stroke);
    sendToSessionTarget(target, PacketType::WHITEBOARD_PACKET, 0, payload.data(), payload.size());
}

#if defined(_WIN32)
void NetworkEngine::startHostTerminal(bool usePowerShell) {
    std::lock_guard<std::mutex> termLk(hostTerminalMutex_);
    hostTerminalActive_.store(false);
    try {
        if (hostTerminalThread_.joinable()) hostTerminalThread_.join();
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

    // Only the pipe ends reach the shell. Sockets are inheritable, and a shell holding a copy keeps a closed listener
    // or connection open, so another engine in this process could never hang up on its host.
    HANDLE inheritOnly[2] = {hStdinRead, hStdoutWrite};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    const bool attrsInited = attrSize > 0 && InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    STARTUPINFOEXW six{};
    six.StartupInfo = si;
    six.StartupInfo.cb = sizeof(six);
    if (attrsInited && UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritOnly,
                                                 sizeof(inheritOnly), nullptr, nullptr)) {
        six.lpAttributeList = attrs;
    }

    BOOL created = CreateProcessW(
        nullptr, cmdLine, nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &six.StartupInfo, &pi
    );
    if (attrsInited) DeleteProcThreadAttributeList(attrs);

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
    std::lock_guard<std::mutex> termLk(hostTerminalMutex_);
    hostTerminalActive_.store(false);
    try {
        if (hostTerminalThread_.joinable()) hostTerminalThread_.join();
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
    std::lock_guard<std::mutex> termLk(hostTerminalMutex_);
    if (!hChildStdinWrite_) return;
    std::string toWrite = input + "\r\n";
    DWORD written = 0;
    WriteFile(hChildStdinWrite_, toWrite.data(), static_cast<DWORD>(toWrite.size()), &written, nullptr);
}

void NetworkEngine::hostTerminalReaderLoop() {
    char buf[4096];
    while (hostTerminalActive_.load() && running_.load() &&
           activeHostClientSock_.load() != ~uintptr_t(0) &&
           (hostLivePermissions_.load() & PERM_INPUT) != 0) {
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
#endif

#if !defined(_WIN32)
// The shell runs on plain pipes, as cmd.exe does on Windows: no echo and no prompt, the viewer shows each command itself.
// The Windows "PowerShell" switch has no equivalent here, so both start bash (or sh where bash is missing).
void NetworkEngine::startHostTerminal(bool /*usePowerShell*/) {
    std::lock_guard<std::mutex> termLk(hostTerminalMutex_);
    hostTerminalActive_.store(false);
    try {
        if (hostTerminalThread_.joinable()) hostTerminalThread_.join();
    } catch (...) {}
    if (childStdinFd_ >= 0) { close(childStdinFd_); childStdinFd_ = -1; }
    if (childStdoutFd_ >= 0) { close(childStdoutFd_); childStdoutFd_ = -1; }
    if (childPid_ > 0) {
        kill(-childPid_, SIGKILL);
        kill(childPid_, SIGKILL);
        waitpid(childPid_, nullptr, 0);
        childPid_ = -1;
    }

    int in[2], out[2];
    if (pipe(in) != 0) return;
    if (pipe(out) != 0) {
        close(in[0]); close(in[1]);
        return;
    }
    for (int fd : { in[0], in[1], out[0], out[1] }) fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct rlimit rl{};
    const int maxFd = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
                          ? static_cast<int>(std::min<rlim_t>(rl.rlim_cur, 65536)) : 65536;
    const char* shell = access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";

    pid_t pid = fork();
    if (pid == 0) {
        // Only async-signal-safe calls between fork and exec: this process has other threads.
        setsid();   // its own process group, so stopping the terminal also ends what was started from it
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        for (int fd = 3; fd < maxFd; ++fd) close(fd);   // the session socket and everything else stays with us
        execl(shell, shell, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]); close(out[0]);
        return;
    }

    childStdinFd_ = in[1];
    childStdoutFd_ = out[0];
    childPid_ = pid;

    hostTerminalActive_.store(true);
    hostTerminalThread_ = std::thread(&NetworkEngine::hostTerminalReaderLoop, this);
}

void NetworkEngine::stopHostTerminal() {
    std::lock_guard<std::mutex> termLk(hostTerminalMutex_);
    hostTerminalActive_.store(false);
    try {
        if (hostTerminalThread_.joinable()) hostTerminalThread_.join();
    } catch (...) {}
    if (childStdinFd_ >= 0) { close(childStdinFd_); childStdinFd_ = -1; }
    if (childStdoutFd_ >= 0) { close(childStdoutFd_); childStdoutFd_ = -1; }
    if (childPid_ > 0) {
        kill(-childPid_, SIGKILL);   // what was started from the shell
        kill(childPid_, SIGKILL);    // the shell itself, even if it has not reached setsid() yet
        waitpid(childPid_, nullptr, 0);
        childPid_ = -1;
    }
}

void NetworkEngine::injectHostTerminalStdin(const std::string& input) {
    std::lock_guard<std::mutex> termLk(hostTerminalMutex_);
    if (childStdinFd_ < 0) return;
    std::string toWrite = input + "\n";
    const char* p = toWrite.data();
    size_t left = toWrite.size();
    while (left > 0) {   // SIGPIPE is ignored, so a shell that has exited only makes this fail
        ssize_t n = write(childStdinFd_, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        p += n;
        left -= static_cast<size_t>(n);
    }
}

void NetworkEngine::hostTerminalReaderLoop() {
    char buf[4096];
    while (hostTerminalActive_.load() && running_.load() &&
           activeHostClientSock_.load() != ~uintptr_t(0) &&
           (hostLivePermissions_.load() & PERM_INPUT) != 0) {
        pollfd pfd{ childStdoutFd_, POLLIN, 0 };
        int pr = poll(&pfd, 1, 20);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        ssize_t numRead = read(childStdoutFd_, buf, sizeof(buf));
        if (numRead < 0 && errno == EINTR) continue;
        if (numRead <= 0) break;   // the shell exited
        ByteWriter w;
        w.writeU8(static_cast<uint8_t>(TerminalStreamKind::StdoutChunk));
        w.writeU32(static_cast<uint32_t>(numRead));
        w.writeBytes(buf, static_cast<size_t>(numRead));
        sendHostEncryptedPacket(PacketType::TERMINAL_DATA, 0, w.buffer().data(), w.buffer().size());
    }
}
#endif

bool NetworkEngine::enqueueTunnelWriteLocked(ActiveTunnel& t, const uint8_t* data, size_t len, size_t& outImmediateSent) {
    outImmediateSent = 0;
    if (t.closeRequested || t.sock == INVALID_SOCKET_VAL) return false;
    if (len == 0) return true;

    const size_t pending = t.writeBuf.size() - t.writeOffset;
    if (!t.connecting && pending == 0) {
        int n = send(t.sock, reinterpret_cast<const char*>(data), static_cast<int>(std::min<size_t>(len, 65536)), 0);
        if (n > 0) {
            outImmediateSent = static_cast<size_t>(n);
            data += n;
            len -= static_cast<size_t>(n);
            if (len == 0) return true;
        } else if (n == 0 || !socket_is_wouldblock(socket_errno())) {
            t.closeRequested = true;
            return false;
        }
    }

    if (pending + len > MAX_TUNNEL_WRITE_QUEUE) {
        t.closeRequested = true;
        return false;
    }
    if (t.writeOffset > 0 && t.writeOffset == t.writeBuf.size()) {
        t.writeBuf.clear();
        t.writeOffset = 0;
    } else if (t.writeOffset > 65536) {
        t.writeBuf.erase(t.writeBuf.begin(), t.writeBuf.begin() + static_cast<std::ptrdiff_t>(t.writeOffset));
        t.writeOffset = 0;
    }
    t.writeBuf.insert(t.writeBuf.end(), data, data + len);
    return true;
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
            socket_set_nonblocking(ls, true);
            socket_set_nosigpipe(ls);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(localPort);
            if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                listen(ls, 5) == 0) {
                tunnelListeners_.push_back({ r.ruleId, localPort, targetPort, ls, false });
            } else {
                socket_close(ls);
            }
        }
    }
    return r.ruleId;
}

void NetworkEngine::removePortForwardRule(uint32_t ruleId) {
    std::lock_guard<std::mutex> lk(tunnelMutex_);
    const bool muxRunning = viewerTunnelActive_.load();
    for (auto it = tunnelListeners_.begin(); it != tunnelListeners_.end(); ) {
        if (it->ruleId == ruleId) {
            if (muxRunning) {
                it->closeRequested = true;
                socket_shutdown(it->listenSock);
                ++it;
            } else {
                socket_close(it->listenSock);
                it = tunnelListeners_.erase(it);
            }
        } else {
            ++it;
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
    const bool muxRunning = viewerTunnelActive_.load();
    for (auto& r : tunnelRules_) {
        if (r.ruleId == ruleId) {
            r.active = active;
            if (!active) {
                for (auto it = tunnelListeners_.begin(); it != tunnelListeners_.end(); ) {
                    if (it->ruleId == ruleId) {
                        if (muxRunning) {
                            it->closeRequested = true;
                            socket_shutdown(it->listenSock);
                            ++it;
                        } else {
                            socket_close(it->listenSock);
                            it = tunnelListeners_.erase(it);
                        }
                    } else {
                        ++it;
                    }
                }
            } else if (viewerActive_.load()) {
                bool alreadyListening = false;
                for (const auto& l : tunnelListeners_) {
                    if (l.ruleId == ruleId && !l.closeRequested) {
                        alreadyListening = true;
                        break;
                    }
                }
                if (!alreadyListening) {
                    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (ls != INVALID_SOCKET) {
                        socket_set_nonblocking(ls, true);
                        socket_set_nosigpipe(ls);
                        sockaddr_in addr{};
                        addr.sin_family = AF_INET;
                        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                        addr.sin_port = htons(r.localPort);
                        if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                            listen(ls, 5) == 0) {
                            tunnelListeners_.push_back({ r.ruleId, r.localPort, r.targetPort, ls, false });
                        } else {
                            socket_close(ls);
                        }
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
    std::lock_guard<std::mutex> lifeLk(viewerTunnelLifecycleMutex_);
    viewerTunnelActive_.store(false);
    try {
        if (viewerTunnelThread_.joinable()) viewerTunnelThread_.join();
    } catch (...) {}
    {
        std::lock_guard<std::mutex> lk(tunnelMutex_);
        for (auto& l : tunnelListeners_) {
            socket_close(l.listenSock);
        }
        tunnelListeners_.clear();
        for (auto& kv : viewerTunnels_) {
            socket_close(kv.second.sock);
        }
        viewerTunnels_.clear();
    }
    viewerTunnelActive_.store(true);
    viewerTunnelThread_ = std::thread(&NetworkEngine::viewerTunnelMultiplexerLoop, this);
}

void NetworkEngine::stopViewerTunnelMultiplexer() {
    std::lock_guard<std::mutex> lifeLk(viewerTunnelLifecycleMutex_);
    viewerTunnelActive_.store(false);
    try {
        if (viewerTunnelThread_.joinable()) {
            viewerTunnelThread_.join();
        }
    } catch (...) {}
    std::lock_guard<std::mutex> lk(tunnelMutex_);
    for (auto& l : tunnelListeners_) {
        socket_close(l.listenSock);
    }
    tunnelListeners_.clear();
    for (auto& kv : viewerTunnels_) {
        socket_close(kv.second.sock);
    }
    viewerTunnels_.clear();
}

void NetworkEngine::viewerTunnelMultiplexerLoop() {
    {
        std::lock_guard<std::mutex> lk(tunnelMutex_);
        for (const auto& r : tunnelRules_) {
            if (r.active) {
                bool alreadyListening = false;
                for (const auto& l : tunnelListeners_) {
                    if (l.ruleId == r.ruleId && !l.closeRequested) {
                        alreadyListening = true;
                        break;
                    }
                }
                if (alreadyListening) continue;
                SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (ls != INVALID_SOCKET) {
                    socket_set_nonblocking(ls, true);
                    socket_set_nosigpipe(ls);
                    sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    addr.sin_port = htons(r.localPort);
                    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                        listen(ls, 5) == 0) {
                        tunnelListeners_.push_back({ r.ruleId, r.localPort, r.targetPort, ls, false });
                    } else {
                        socket_close(ls);
                    }
                }
            }
        }
    }

    std::vector<uint8_t> readBuf(16384);

    while (viewerTunnelActive_.load() && running_.load() && viewerActive_.load()) {
        struct PollItem {
            bool     isListener = false;
            uint32_t id = 0;       // ruleId or tunnelId
            uint32_t ruleId = 0;
            uint16_t targetPort = 0;
            socket_t sock = INVALID_SOCKET_VAL;
        };
        std::vector<PollItem> items;
        std::vector<pollfd_t> pollFds;

        {
            std::lock_guard<std::mutex> lk(tunnelMutex_);
            // Sweep listeners and tunnels marked for close exclusively on this owning thread.
            for (auto it = tunnelListeners_.begin(); it != tunnelListeners_.end(); ) {
                if (it->closeRequested) {
                    socket_close(it->listenSock);
                    it = tunnelListeners_.erase(it);
                } else {
                    pollfd_t pfd{};
                    pfd.fd = it->listenSock;
                    pfd.events = POLLIN;
                    pollFds.push_back(pfd);
                    items.push_back({ true, it->ruleId, it->ruleId, it->targetPort, it->listenSock });
                    ++it;
                }
            }
            for (auto it = viewerTunnels_.begin(); it != viewerTunnels_.end(); ) {
                if (it->second.closeRequested) {
                    socket_close(it->second.sock);
                    it = viewerTunnels_.erase(it);
                } else {
                    pollfd_t pfd{};
                    pfd.fd = it->second.sock;
                    pfd.events = POLLIN;
                    if (it->second.writeOffset < it->second.writeBuf.size()) {
                        pfd.events |= POLLOUT;
                    }
                    pollFds.push_back(pfd);
                    items.push_back({ false, it->second.tunnelId, it->second.ruleId, it->second.targetPort, it->second.sock });
                    ++it;
                }
            }
        }

        if (pollFds.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        int res = socketPoll(pollFds.data(), pollFds.size(), 20);
        if (res <= 0) continue;

        std::vector<uint32_t> closedTunnels;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto revents = pollFds[i].revents;
            if (revents == 0) continue;

            if (items[i].isListener) {
                if (revents & POLLIN) {
                    sockaddr_in clientAddr{};
                    socklen_t addrLen = sizeof(clientAddr);
                    SOCKET cs = accept(items[i].sock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
                    if (cs != INVALID_SOCKET) {
                        socket_set_nonblocking(cs, true);
                        socket_set_nosigpipe(cs);

                        uint32_t tid = 0;
                        bool accepted = false;
                        {
                            std::lock_guard<std::mutex> lk(tunnelMutex_);
                            if (viewerTunnels_.size() < MAX_ACTIVE_TUNNELS) {
                                tid = nextTunnelId_++;
                                ActiveTunnel at;
                                at.tunnelId = tid;
                                at.ruleId = items[i].ruleId;
                                at.sock = cs;
                                at.targetPort = items[i].targetPort;
                                viewerTunnels_.emplace(tid, std::move(at));
                                accepted = true;
                            }
                        }
                        if (!accepted) {
                            socket_close(cs);
                            continue;
                        }

                        TunnelOpenHeader toh{};
                        toh.tunnelId = tid;
                        toh.targetPort = items[i].targetPort;
                        toh.flags = 0;
                        ByteWriter w;
                        w.writeBytes(&toh, sizeof(toh));
                        sendViewerEncryptedPacket(PacketType::TUNNEL_OPEN, 0, w.buffer().data(), w.buffer().size());
                    }
                }
                continue;
            }

            // Active viewer tunnel socket
            bool tunnelFailed = false;
            if (revents & POLLOUT) {
                std::lock_guard<std::mutex> lk(tunnelMutex_);
                auto it = viewerTunnels_.find(items[i].id);
                if (it != viewerTunnels_.end() && !it->second.closeRequested) {
                    auto& t = it->second;
                    while (t.writeOffset < t.writeBuf.size()) {
                        const size_t rem = t.writeBuf.size() - t.writeOffset;
                        int n = send(t.sock, reinterpret_cast<const char*>(t.writeBuf.data() + t.writeOffset),
                                     static_cast<int>(std::min<size_t>(rem, 65536)), 0);
                        if (n > 0) {
                            t.writeOffset += static_cast<size_t>(n);
                            for (auto& r : tunnelRules_) {
                                if (r.ruleId == t.ruleId) {
                                    r.bytesTransferredIn += static_cast<size_t>(n);
                                    break;
                                }
                            }
                        } else if (n < 0 && socket_is_wouldblock(socket_errno())) {
                            break;
                        } else {
                            tunnelFailed = true;
                            break;
                        }
                    }
                    if (t.writeOffset == t.writeBuf.size()) {
                        t.writeBuf.clear();
                        t.writeOffset = 0;
                    }
                }
            }

            if (!tunnelFailed && (revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                int n = recv(items[i].sock, reinterpret_cast<char*>(readBuf.data()), static_cast<int>(readBuf.size()), 0);
                if (n > 0) {
                    TunnelDataHeader tdh{};
                    tdh.tunnelId = items[i].id;
                    tdh.dataLen = static_cast<uint32_t>(n);
                    ByteWriter w;
                    w.writeBytes(&tdh, sizeof(tdh));
                    w.writeBytes(readBuf.data(), n);
                    sendViewerEncryptedPacket(PacketType::TUNNEL_DATA, 0, w.buffer().data(), w.buffer().size());

                    std::lock_guard<std::mutex> lk(tunnelMutex_);
                    for (auto& r : tunnelRules_) {
                        if (r.ruleId == items[i].ruleId) {
                            r.bytesTransferredOut += static_cast<size_t>(n);
                            break;
                        }
                    }
                } else if (n < 0 && socket_is_wouldblock(socket_errno())) {
                    // Spurious wakeup on non-blocking socket; keep tunnel alive.
                } else {
                    tunnelFailed = true;
                }
            }

            if (tunnelFailed) {
                closedTunnels.push_back(items[i].id);
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
                socket_close(it->second.sock);
                viewerTunnels_.erase(it);
            }
        }
    }
}

void NetworkEngine::startHostTunnelProxy() {
    std::lock_guard<std::mutex> lifeLk(hostTunnelLifecycleMutex_);
    hostTunnelActive_.store(false);
    try {
        if (hostTunnelThread_.joinable()) hostTunnelThread_.join();
    } catch (...) {}
    {
        std::lock_guard<std::mutex> lk(tunnelMutex_);
        for (auto& kv : hostTunnels_) {
            socket_close(kv.second.sock);
        }
        hostTunnels_.clear();
    }
    hostTunnelActive_.store(true);
    hostTunnelThread_ = std::thread(&NetworkEngine::hostTunnelProxyLoop, this);
}

void NetworkEngine::stopHostTunnelProxy() {
    std::lock_guard<std::mutex> lifeLk(hostTunnelLifecycleMutex_);
    hostTunnelActive_.store(false);
    try {
        if (hostTunnelThread_.joinable()) hostTunnelThread_.join();
    } catch (...) {}
    std::lock_guard<std::mutex> lk(tunnelMutex_);
    for (auto& kv : hostTunnels_) {
        socket_close(kv.second.sock);
    }
    hostTunnels_.clear();
}

void NetworkEngine::hostTunnelProxyLoop() {
    std::vector<uint8_t> readBuf(16384);

    while (hostTunnelActive_.load() && running_.load() &&
           activeHostClientSock_.load() != ~uintptr_t(0) &&
           (hostLivePermissions_.load() & PERM_INPUT) != 0) {
        struct PollItem {
            uint32_t tunnelId = 0;
            socket_t sock = INVALID_SOCKET_VAL;
            bool     connecting = false;
        };
        std::vector<PollItem> items;
        std::vector<pollfd_t> pollFds;
        std::vector<std::pair<uint32_t, uint8_t>> timedOutTunnels;
        const uint64_t now = nowTickMs();

        {
            std::lock_guard<std::mutex> lk(tunnelMutex_);
            for (auto it = hostTunnels_.begin(); it != hostTunnels_.end(); ) {
                auto& t = it->second;
                if (t.closeRequested) {
                    socket_close(t.sock);
                    it = hostTunnels_.erase(it);
                    continue;
                }
                if (t.connecting && now - t.connectStartMs > 5000) {
                    timedOutTunnels.push_back({ t.tunnelId, 1 });
                    socket_close(t.sock);
                    it = hostTunnels_.erase(it);
                    continue;
                }
                pollfd_t pfd{};
                pfd.fd = t.sock;
                if (t.connecting) {
                    pfd.events = POLLOUT;
                } else {
                    pfd.events = POLLIN;
                    if (t.writeOffset < t.writeBuf.size()) {
                        pfd.events |= POLLOUT;
                    }
                }
                pollFds.push_back(pfd);
                items.push_back({ t.tunnelId, t.sock, t.connecting });
                ++it;
            }
        }

        for (const auto& tc : timedOutTunnels) {
            TunnelCloseHeader tch{};
            tch.tunnelId = tc.first;
            tch.reasonCode = tc.second;
            ByteWriter w;
            w.writeBytes(&tch, sizeof(tch));
            sendHostEncryptedPacket(PacketType::TUNNEL_CLOSE, 0, w.buffer().data(), w.buffer().size());
        }

        if (pollFds.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        int res = socketPoll(pollFds.data(), pollFds.size(), 20);
        if (res <= 0) continue;

        std::vector<std::pair<uint32_t, uint8_t>> closedTunnels;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto revents = pollFds[i].revents;
            if (revents == 0) continue;

            if (items[i].connecting) {
                int soErr = 0;
#if defined(_WIN32)
                int soLen = sizeof(soErr);
#else
                socklen_t soLen = sizeof(soErr);
#endif
                getsockopt(items[i].sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &soLen);
                if (soErr != 0 || (revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    closedTunnels.push_back({ items[i].tunnelId, 1 });
                } else if (revents & POLLOUT) {
                    std::lock_guard<std::mutex> lk(tunnelMutex_);
                    auto it = hostTunnels_.find(items[i].tunnelId);
                    if (it != hostTunnels_.end()) {
                        it->second.connecting = false;
                    }
                }
                continue;
            }

            bool tunnelFailed = false;
            if (revents & POLLOUT) {
                std::lock_guard<std::mutex> lk(tunnelMutex_);
                auto it = hostTunnels_.find(items[i].tunnelId);
                if (it != hostTunnels_.end() && !it->second.closeRequested) {
                    auto& t = it->second;
                    while (t.writeOffset < t.writeBuf.size()) {
                        const size_t rem = t.writeBuf.size() - t.writeOffset;
                        int n = send(t.sock, reinterpret_cast<const char*>(t.writeBuf.data() + t.writeOffset),
                                     static_cast<int>(std::min<size_t>(rem, 65536)), 0);
                        if (n > 0) {
                            t.writeOffset += static_cast<size_t>(n);
                        } else if (n < 0 && socket_is_wouldblock(socket_errno())) {
                            break;
                        } else {
                            tunnelFailed = true;
                            break;
                        }
                    }
                    if (t.writeOffset == t.writeBuf.size()) {
                        t.writeBuf.clear();
                        t.writeOffset = 0;
                    }
                }
            }

            if (!tunnelFailed && (revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                int n = recv(items[i].sock, reinterpret_cast<char*>(readBuf.data()), static_cast<int>(readBuf.size()), 0);
                if (n > 0) {
                    TunnelDataHeader tdh{};
                    tdh.tunnelId = items[i].tunnelId;
                    tdh.dataLen = static_cast<uint32_t>(n);
                    ByteWriter w;
                    w.writeBytes(&tdh, sizeof(tdh));
                    w.writeBytes(readBuf.data(), n);
                    sendHostEncryptedPacket(PacketType::TUNNEL_DATA, 0, w.buffer().data(), w.buffer().size());
                } else if (n < 0 && socket_is_wouldblock(socket_errno())) {
                    // Spurious wakeup on non-blocking socket; keep tunnel alive.
                } else {
                    tunnelFailed = true;
                }
            }

            if (tunnelFailed) {
                closedTunnels.push_back({ items[i].tunnelId, 0 });
            }
        }

        for (const auto& tc : closedTunnels) {
            TunnelCloseHeader tch{};
            tch.tunnelId = tc.first;
            tch.reasonCode = tc.second;
            ByteWriter w;
            w.writeBytes(&tch, sizeof(tch));
            sendHostEncryptedPacket(PacketType::TUNNEL_CLOSE, 0, w.buffer().data(), w.buffer().size());

            std::lock_guard<std::mutex> lk(tunnelMutex_);
            auto it = hostTunnels_.find(tc.first);
            if (it != hostTunnels_.end()) {
                socket_close(it->second.sock);
                hostTunnels_.erase(it);
            }
        }
    }
}

// ---------------- Remote Hardware Diagnostics & Live Process Telemetry ----------------

#if !defined(_WIN32) && !defined(__APPLE__)
namespace {

// Reads the host's real numbers from /proc and statvfs. Anything that cannot be read stays zero or empty.
void sampleLinuxDiagnostics(SystemDiagnosticsPayload& diag) {
    // CPU: share of non-idle time since the previous sample (zero on the first call).
    {
        static std::mutex s_mutex;
        static uint64_t s_prevTotal = 0, s_prevIdle = 0;
        std::ifstream stat("/proc/stat");
        std::string tag;
        uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        if (stat >> tag >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal && tag == "cpu") {
            uint64_t idleAll = idle + iowait;
            uint64_t total = user + nice + sys + idleAll + irq + softirq + steal;
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_prevTotal != 0 && total > s_prevTotal) {
                double busy = static_cast<double>((total - s_prevTotal) - (idleAll - s_prevIdle));
                diag.cpuUsagePercent = static_cast<float>(std::clamp(100.0 * busy / static_cast<double>(total - s_prevTotal), 0.0, 100.0));
            }
            s_prevTotal = total;
            s_prevIdle = idleAll;
        }
    }

    // Memory
    {
        std::ifstream mem("/proc/meminfo");
        std::string key;
        uint64_t value = 0;
        std::string unit;
        uint64_t totalKb = 0, availKb = 0;
        while (mem >> key >> value) {
            std::getline(mem, unit);
            if (key == "MemTotal:") totalKb = value;
            else if (key == "MemAvailable:") availKb = value;
        }
        diag.ramTotalBytes = totalKb * 1024;
        diag.ramUsedBytes = (totalKb >= availKb ? totalKb - availKb : 0) * 1024;
    }

    // Disk: the root filesystem
    {
        struct statvfs fs{};
        if (statvfs("/", &fs) == 0) {
            diag.diskTotalBytes = static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
            uint64_t freeBytes = static_cast<uint64_t>(fs.f_bfree) * fs.f_frsize;
            diag.diskUsedBytes = diag.diskTotalBytes >= freeBytes ? diag.diskTotalBytes - freeBytes : 0;
        }
    }

    // Processes by resident memory
    {
        const uint64_t pageSize = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
        std::vector<ProcessTelemetryItem> candidates;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
            const std::string pidText = entry.path().filename().string();
            if (pidText.empty() || !std::all_of(pidText.begin(), pidText.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
            ProcessTelemetryItem item;
            item.pid = static_cast<uint32_t>(std::stoul(pidText));
            std::ifstream comm(entry.path() / "comm");
            std::getline(comm, item.name);
            if (item.name.empty()) item.name = "Unknown";
            std::ifstream statm(entry.path() / "statm");
            uint64_t sizePages = 0, residentPages = 0;
            if (statm >> sizePages >> residentPages) item.workingSetBytes = residentPages * pageSize;
            candidates.push_back(std::move(item));
        }
        std::sort(candidates.begin(), candidates.end(), [](const ProcessTelemetryItem& a, const ProcessTelemetryItem& b) {
            return a.workingSetBytes > b.workingSetBytes;
        });
        diag.processes.assign(candidates.begin(), candidates.begin() + std::min<size_t>(candidates.size(), 60));
    }

    // Hardware and OS
    {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        uint32_t cores = 0;
        while (std::getline(cpuinfo, line)) {
            if (line.rfind("model name", 0) == 0 && diag.cpuModel.empty()) {
                auto colon = line.find(':');
                if (colon != std::string::npos) diag.cpuModel = line.substr(colon + 2);
            }
            if (line.rfind("processor", 0) == 0) ++cores;
        }
        diag.cpuCores = cores != 0 ? cores : static_cast<uint32_t>(std::max(1u, std::thread::hardware_concurrency()));

        std::ifstream osRelease("/etc/os-release");
        while (std::getline(osRelease, line)) {
            if (line.rfind("PRETTY_NAME=", 0) == 0) {
                diag.osVersion = line.substr(12);
                diag.osVersion.erase(std::remove(diag.osVersion.begin(), diag.osVersion.end(), '"'), diag.osVersion.end());
            }
        }
        // shortcut: no GPU probe off Windows, so the sheet says so instead of guessing.
        diag.gpuModel = "Not detected";

        std::ifstream up("/proc/uptime");
        double seconds = 0;
        if (up >> seconds) diag.uptimeSeconds = static_cast<uint64_t>(seconds);
    }
}

} // namespace
#elif defined(__APPLE__)
namespace {

std::string macSysctlString(const char* name) {
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    std::string out(len, '\0');
    if (sysctlbyname(name, out.data(), &len, nullptr, 0) != 0) return {};
    out.resize(strnlen(out.c_str(), len));
    return out;
}

// The host's real numbers from sysctl, Mach and libproc. Anything that cannot be read stays zero or empty.
void sampleMacDiagnostics(SystemDiagnosticsPayload& diag) {
    // CPU: share of non-idle ticks since the previous sample (zero on the first call).
    {
        static std::mutex s_mutex;
        static uint64_t s_prevTotal = 0, s_prevIdle = 0;
        host_cpu_load_info_data_t load{};
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&load), &count) == KERN_SUCCESS) {
            const uint64_t idle = load.cpu_ticks[CPU_STATE_IDLE];
            const uint64_t total = static_cast<uint64_t>(load.cpu_ticks[CPU_STATE_USER]) + load.cpu_ticks[CPU_STATE_SYSTEM] +
                                   load.cpu_ticks[CPU_STATE_NICE] + idle;
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_prevTotal != 0 && total > s_prevTotal) {
                const double busy = static_cast<double>((total - s_prevTotal) - (idle - s_prevIdle));
                diag.cpuUsagePercent = static_cast<float>(std::clamp(100.0 * busy / static_cast<double>(total - s_prevTotal), 0.0, 100.0));
            }
            s_prevTotal = total;
            s_prevIdle = idle;
        }
    }

    // Memory: used = total minus free, inactive and speculative pages.
    {
        uint64_t total = 0;
        size_t len = sizeof(total);
        sysctlbyname("hw.memsize", &total, &len, nullptr, 0);
        diag.ramTotalBytes = total;
        vm_statistics64_data_t vm{};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (total != 0 && host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
            const uint64_t availableBytes = (static_cast<uint64_t>(vm.free_count) + vm.inactive_count + vm.speculative_count) * vm_page_size;
            diag.ramUsedBytes = total > availableBytes ? total - availableBytes : 0;
        }
    }

    // Disk: the root filesystem
    {
        struct statvfs fs{};
        if (statvfs("/", &fs) == 0) {
            diag.diskTotalBytes = static_cast<uint64_t>(fs.f_blocks) * fs.f_frsize;
            const uint64_t freeBytes = static_cast<uint64_t>(fs.f_bfree) * fs.f_frsize;
            diag.diskUsedBytes = diag.diskTotalBytes >= freeBytes ? diag.diskTotalBytes - freeBytes : 0;
        }
    }

    // Processes by resident memory (libproc lists the ones this user may inspect)
    {
        std::vector<pid_t> pids(2048);
        const int bytes = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
        const int n = bytes > 0 ? bytes / static_cast<int>(sizeof(pid_t)) : 0;
        std::vector<ProcessTelemetryItem> candidates;
        for (int i = 0; i < n; ++i) {
            if (pids[i] <= 0) continue;
            proc_taskinfo info{};
            if (proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &info, sizeof(info)) != static_cast<int>(sizeof(info))) continue;
            ProcessTelemetryItem item;
            item.pid = static_cast<uint32_t>(pids[i]);
            char name[256] = {};
            if (proc_name(pids[i], name, sizeof(name)) > 0) item.name = name;
            if (item.name.empty()) item.name = "Unknown";
            item.workingSetBytes = info.pti_resident_size;
            candidates.push_back(std::move(item));
        }
        std::sort(candidates.begin(), candidates.end(), [](const ProcessTelemetryItem& a, const ProcessTelemetryItem& b) {
            return a.workingSetBytes > b.workingSetBytes;
        });
        diag.processes.assign(candidates.begin(), candidates.begin() + std::min<size_t>(candidates.size(), 60));
    }

    // Hardware and OS
    {
        diag.cpuModel = macSysctlString("machdep.cpu.brand_string");
        int cores = 0;
        size_t len = sizeof(cores);
        sysctlbyname("hw.logicalcpu", &cores, &len, nullptr, 0);
        diag.cpuCores = cores > 0 ? static_cast<uint32_t>(cores) : static_cast<uint32_t>(std::max(1u, std::thread::hardware_concurrency()));
        const std::string os = macSysctlString("kern.osproductversion");
        diag.osVersion = os.empty() ? "macOS" : "macOS " + os;
        // shortcut: no GPU probe off Windows, so the sheet says so instead of guessing.
        diag.gpuModel = "Not detected";
        struct timeval boot{};
        len = sizeof(boot);
        int mib[2] = { CTL_KERN, KERN_BOOTTIME };
        if (sysctl(mib, 2, &boot, &len, nullptr, 0) == 0 && boot.tv_sec > 0) {
            const time_t now = std::time(nullptr);
            diag.uptimeSeconds = now > boot.tv_sec ? static_cast<uint64_t>(now - boot.tv_sec) : 0;
        }
    }
}

} // namespace
#endif

SystemDiagnosticsPayload NetworkEngine::sampleHostDiagnostics() {
    SystemDiagnosticsPayload diag{};
#if defined(_WIN32)

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

            // Keep top 60
            size_t maxProcs = std::min<size_t>(candidates.size(), 60);
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
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            char cpuBrand[49] = {0};
#if defined(_MSC_VER)
            int regs[4] = {0};
            __cpuid(regs, static_cast<int>(0x80000000));
            if (static_cast<unsigned int>(regs[0]) >= 0x80000004) {
                __cpuid(reinterpret_cast<int*>(cpuBrand),      static_cast<int>(0x80000002));
                __cpuid(reinterpret_cast<int*>(cpuBrand + 16), static_cast<int>(0x80000003));
                __cpuid(reinterpret_cast<int*>(cpuBrand + 32), static_cast<int>(0x80000004));
                char* p = cpuBrand;
                while (*p == ' ') p++;
                s_cachedCpuModel = p;
            }
#elif defined(__GNUC__) || defined(__clang__)
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            if (__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) && eax >= 0x80000004) {
                __get_cpuid(0x80000002, (unsigned int*)(cpuBrand), (unsigned int*)(cpuBrand + 4), (unsigned int*)(cpuBrand + 8), (unsigned int*)(cpuBrand + 12));
                __get_cpuid(0x80000003, (unsigned int*)(cpuBrand + 16), (unsigned int*)(cpuBrand + 20), (unsigned int*)(cpuBrand + 24), (unsigned int*)(cpuBrand + 28));
                __get_cpuid(0x80000004, (unsigned int*)(cpuBrand + 32), (unsigned int*)(cpuBrand + 36), (unsigned int*)(cpuBrand + 40), (unsigned int*)(cpuBrand + 44));
                char* p = cpuBrand;
                while (*p == ' ') p++;
                s_cachedCpuModel = p;
            }
#endif
#endif
            if (s_cachedCpuModel.empty()) {
                s_cachedCpuModel = "Processor";
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
                DWORD sz = sizeof(prodName) - sizeof(wchar_t);
                DWORD prodType = 0;
                if (RegQueryValueExW(hKey, L"ProductName", nullptr, &prodType, (LPBYTE)prodName, &sz) == ERROR_SUCCESS &&
                    (prodType == REG_SZ || prodType == REG_EXPAND_SZ)) {
                    prodName[(sizeof(prodName) / sizeof(wchar_t)) - 1] = L'\0';
                    wchar_t buildLab[64] = {0};
                    DWORD bsz = sizeof(buildLab) - sizeof(wchar_t);
                    DWORD buildType = 0;
                    if (RegQueryValueExW(hKey, L"CurrentBuild", nullptr, &buildType, (LPBYTE)buildLab, &bsz) == ERROR_SUCCESS &&
                        (buildType == REG_SZ || buildType == REG_EXPAND_SZ)) {
                        buildLab[(sizeof(buildLab) / sizeof(wchar_t)) - 1] = L'\0';
                    } else {
                        buildLab[0] = L'\0';
                    }
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
#elif defined(__APPLE__)
    sampleMacDiagnostics(diag);
#else
    sampleLinuxDiagnostics(diag);
#endif

    return diag;
}

bool NetworkEngine::executeProcessKill(uint32_t pid, uint8_t callerPermissions) {
    if ((callerPermissions & PERM_INPUT) == 0) {
        return false;
    }
    if (pid <= 1 || pid == 4 || pid > static_cast<uint32_t>(INT32_MAX)) {
        return false;
    }
#if defined(_WIN32)
    const uint32_t selfPid = GetCurrentProcessId();
#else
    const uint32_t selfPid = static_cast<uint32_t>(getpid());
#endif
    if (pid == selfPid) {
        return false;
    }

#if defined(_WIN32)
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) {
        return false;
    }
    BOOL success = TerminateProcess(hProc, 1);
    CloseHandle(hProc);
    return success != FALSE;
#else
    const pid_t targetPid = static_cast<pid_t>(pid);
    if (targetPid <= 1) {
        return false;
    }
    return kill(targetPid, SIGKILL) == 0;
#endif
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

bool NetworkEngine::executeProcessLaunch(const std::string& commandLine, const std::string& workingDir,
                                        uint8_t callerPermissions, uint32_t& outPid, std::string& outError) {
    outPid = 0;
    outError.clear();
    if ((callerPermissions & PERM_INPUT) == 0) {
        outError = "Input permission denied";
        return false;
    }
    if (commandLine.empty()) {
        outError = "Empty command line";
        return false;
    }

#if defined(_WIN32)
    std::wstring wCmd = utf8ToWide(commandLine);
    std::wstring wDir = utf8ToWide(workingDir);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdBuf(wCmd.begin(), wCmd.end());
    cmdBuf.push_back(L'\0');

    const wchar_t* pDir = wDir.empty() ? nullptr : wDir.c_str();

    BOOL ok = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        pDir,
        &si,
        &pi
    );

    if (!ok) {
        DWORD err = GetLastError();
        outError = "CreateProcess error: " + std::to_string(err);
        return false;
    }

    outPid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)workingDir;
    outError = "Not supported on non-Windows host";
    return false;
#endif
}

void NetworkEngine::sendProcessLaunch(const std::string& commandLine, const std::string& workingDir) {
    ProcessLaunchReqPayload req;
    req.launchId = static_cast<uint32_t>(GetTickCount64());
    req.commandLine = commandLine;
    req.workingDir = workingDir;
    std::vector<uint8_t> buf;
    serializeProcessLaunchReq(req, buf);
    sendViewerEncryptedPacket(PacketType::PROCESS_LAUNCH_REQ, 0, buf.data(), buf.size());
}

ProcessLaunchRespPayload NetworkEngine::latestProcessLaunchResp() const {
    std::lock_guard<std::mutex> lk(processLaunchMutex_);
    return latestProcessLaunchResp_;
}

bool NetworkEngine::hasNewProcessLaunchResp() {
    std::lock_guard<std::mutex> lk(processLaunchMutex_);
    bool val = hasNewProcessLaunchResp_;
    hasNewProcessLaunchResp_ = false;
    return val;
}

void NetworkEngine::requestRemoteFileList(const std::string& path) {
    FileBrowserListReqPayload req;
    req.requestId = static_cast<uint32_t>(GetTickCount64());
    req.path = path;
    std::vector<uint8_t> buf;
    serializeFileBrowserListReq(req, buf);
    sendViewerEncryptedPacket(PacketType::FILE_BROWSER_LIST_REQ, 0, buf.data(), buf.size());
}

void NetworkEngine::sendRemoteFileAction(uint8_t actionType, const std::string& targetPath, const std::string& newOrDestName) {
    FileBrowserActionReqPayload req;
    req.actionId = static_cast<uint32_t>(GetTickCount64());
    req.actionType = actionType;
    req.targetPath = targetPath;
    req.newOrDestName = newOrDestName;
    std::vector<uint8_t> buf;
    serializeFileBrowserActionReq(req, buf);
    sendViewerEncryptedPacket(PacketType::FILE_BROWSER_ACTION_REQ, 0, buf.data(), buf.size());
}

void NetworkEngine::sendRemoteFileDownload(const std::string& remoteFilePath, const std::string& destFileName) {
    FileBrowserDownloadReqPayload req;
    req.requestId = static_cast<uint32_t>(GetTickCount64());
    req.remoteFilePath = remoteFilePath;
    req.destFileName = destFileName;
    std::vector<uint8_t> buf;
    serializeFileBrowserDownloadReq(req, buf);
    sendViewerEncryptedPacket(PacketType::FILE_BROWSER_DOWNLOAD_REQ, 0, buf.data(), buf.size());
}

FileBrowserListRespPayload NetworkEngine::latestRemoteFileList() const {
    std::lock_guard<std::mutex> lk(fileBrowserMutex_);
    return latestRemoteFileList_;
}

bool NetworkEngine::hasNewRemoteFileList() {
    std::lock_guard<std::mutex> lk(fileBrowserMutex_);
    bool val = hasNewRemoteFileList_;
    hasNewRemoteFileList_ = false;
    return val;
}

FileBrowserActionRespPayload NetworkEngine::latestRemoteFileAction() const {
    std::lock_guard<std::mutex> lk(fileBrowserMutex_);
    return latestRemoteFileAction_;
}

bool NetworkEngine::hasNewRemoteFileAction() {
    std::lock_guard<std::mutex> lk(fileBrowserMutex_);
    bool val = hasNewRemoteFileAction_;
    hasNewRemoteFileAction_ = false;
    return val;
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
#if defined(_WIN32)
    int sent = sendto(s, reinterpret_cast<const char*>(&req), sizeof(req), 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
#else
    // A connected UDP socket reports "port unreachable" at once, as Winsock does for an unconnected one,
    // instead of leaving the caller to wait out the whole timeout.
    int sent = (connect(s, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) == 0)
        ? static_cast<int>(send(s, reinterpret_cast<const char*>(&req), sizeof(req), 0))
        : -1;
#endif
    freeaddrinfo(res);

    if (sent != sizeof(req)) {
        closesocket(s);
        result.natTypeDescription = "Failed to send STUN request";
        return result;
    }

    pollfd_t pfd{};
    pfd.fd = s;
    pfd.events = POLLIN;

    int sel = socketPoll(&pfd, 1, static_cast<int>(timeoutMs));
    if (sel <= 0 || !(pfd.revents & POLLIN)) {
        closesocket(s);
        result.natTypeDescription = "STUN query timed out";
        return result;
    }

    uint8_t recvBuf[1024] = {};
    sockaddr_in fromAddr{};
    socklen_t fromLen = sizeof(fromAddr);
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

    relayReconnectRequested_.store(true);
    uintptr_t rs = relayControlSock_.load();
    if (rs != ~uintptr_t(0)) {
        socket_shutdown(toWinSock(rs));
    }
}

void NetworkEngine::startNetworkDiagnostics(const std::string& relayAddr, const std::string& stunAddr) {
    {
        std::lock_guard<std::mutex> lock(netDiagMutex_);
        if (netDiagResult_.running) {
            return;
        }
    }
    // A finished thread is joined with the lock released: it takes the lock to publish its result.
    std::thread finished;
    {
        std::lock_guard<std::mutex> lock(netDiagMutex_);
        finished = std::move(netDiagThread_);
    }
    if (finished.joinable()) {
        try { finished.join(); } catch (...) {}
    }

    std::lock_guard<std::mutex> lock(netDiagMutex_);
    if (netDiagResult_.running) {
        return;
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

bool NetworkEngine::sendScratchpadText(const std::string& text, uint32_t cursorPos, SessionTarget target) {
    uint32_t myId = static_cast<uint32_t>(identity_.deskId());
    ScratchpadUpdatePayload payload = scratchpadMgr_.updateLocalText(text, myId, cursorPos);
    std::vector<uint8_t> buf;
    serializeScratchpadUpdate(payload, buf);
    return sendToSessionTarget(target, PacketType::SCRATCHPAD_UPDATE, FLAG_ENCRYPTED, buf.data(), buf.size());
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

