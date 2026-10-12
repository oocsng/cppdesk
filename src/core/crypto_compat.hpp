#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonCryptor.h>
#include <Security/SecRandom.h>
#include <Security/Security.h>
#include <sys/mman.h>
#else
#include <sys/mman.h>
#endif

namespace cppdesk {

inline void crypto_secure_zero(void* ptr, size_t len) {
    if (!ptr || len == 0) return;
#if defined(_WIN32)
    SecureZeroMemory(ptr, len);
#else
    volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
    while (len--) {
        *p++ = 0;
    }
#endif
}

inline bool crypto_pin_memory(void* ptr, size_t len) {
    if (!ptr || len == 0) return false;
#if defined(_WIN32)
    return VirtualLock(ptr, len) != FALSE;
#elif defined(__APPLE__) || defined(__unix__)
    return mlock(ptr, len) == 0;
#else
    return true;
#endif
}

inline void crypto_unpin_memory(void* ptr, size_t len) {
    if (!ptr || len == 0) return;
#if defined(_WIN32)
    VirtualUnlock(ptr, len);
#elif defined(__APPLE__) || defined(__unix__)
    munlock(ptr, len);
#endif
}

} // namespace cppdesk
