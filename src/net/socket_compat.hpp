#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/uio.h>
#include <netdb.h>
#include <errno.h>
#endif

#include <cstdint>
#include <cstddef>
#include <mutex>

namespace cppdesk {

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCKET_VAL = INVALID_SOCKET;
constexpr int SOCKET_ERROR_VAL = SOCKET_ERROR;

inline int socket_errno() {
    return WSAGetLastError();
}

inline bool socket_is_wouldblock(int err) {
    return (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
}

inline void socket_close(socket_t s) {
    if (s != INVALID_SOCKET_VAL) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}

inline bool socket_set_nonblocking(socket_t s, bool nonblock) {
    u_long mode = nonblock ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

inline bool socket_init_network() {
    static std::once_flag flag;
    static bool ok = false;
    std::call_once(flag, []() {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    });
    return ok;
}

inline void socket_set_nosigpipe(socket_t s) {
    (void)s;
}

#else

using socket_t = int;
constexpr socket_t INVALID_SOCKET_VAL = -1;
constexpr int SOCKET_ERROR_VAL = -1;

inline int socket_errno() {
    return errno;
}

inline bool socket_is_wouldblock(int err) {
    return (err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS);
}

inline void socket_close(socket_t s) {
    if (s != INVALID_SOCKET_VAL) {
        shutdown(s, SHUT_RDWR);
        close(s);
    }
}

inline bool socket_set_nonblocking(socket_t s, bool nonblock) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0;
}

inline bool socket_init_network() {
    return true;
}

inline void socket_set_nosigpipe(socket_t s) {
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#else
    (void)s;
#endif
}

#endif

// Fallback geometry types for non-Windows platforms
#if !defined(_WIN32)
#ifndef _RECT_DEFINED
#define _RECT_DEFINED
struct RECT {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
};
#endif
#ifndef _POINT_DEFINED
#define _POINT_DEFINED
struct POINT {
    int32_t x = 0;
    int32_t y = 0;
};
#endif
#endif

// Portable scatter-gather buffer structure
struct SocketBuffer {
    void* buf = nullptr;
    size_t len = 0;
};

// Handle conversion helpers
inline socket_t toSocket(uintptr_t s) {
    return (s == ~uintptr_t(0)) ? INVALID_SOCKET_VAL : static_cast<socket_t>(s);
}

inline uintptr_t fromSocket(socket_t s) {
    return (s == INVALID_SOCKET_VAL) ? ~uintptr_t(0) : static_cast<uintptr_t>(s);
}

inline void closeSocketHandle(uintptr_t& s) {
    socket_t sock = toSocket(s);
    if (sock != INVALID_SOCKET_VAL) {
        socket_close(sock);
        s = ~uintptr_t(0);
    }
}

// Backward compatibility aliases
inline socket_t toWinSock(uintptr_t s) { return toSocket(s); }
inline uintptr_t fromWinSock(socket_t s) { return fromSocket(s); }
inline void closeWinSock(uintptr_t& s) { closeSocketHandle(s); }

} // namespace cppdesk
