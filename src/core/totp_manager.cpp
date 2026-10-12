#include "totp_manager.hpp"
#include "crypto_identity.hpp"
#include "crypto_compat.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

#include <ctime>
#include <cstdio>
#include <cctype>
#include <algorithm>

namespace cppdesk {

namespace {

const uint32_t kPowersOf10[] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
};

} // namespace

TotpManager::TotpManager() = default;

TotpManager::~TotpManager() {
    clear();
}

bool TotpManager::isConfigured() const {
    return !secretBytes_.empty();
}

void TotpManager::clear() {
    if (!secretBytes_.empty()) {
        CryptoUtils::secureZero(secretBytes_.data(), secretBytes_.size());
        secretBytes_.clear();
    }
    if (!secretBase32_.empty()) {
        CryptoUtils::secureZero(secretBase32_.data(), secretBase32_.size());
        secretBase32_.clear();
    }
    lastConsumedStep_ = 0;
}

void TotpManager::setSecret(const std::string& base32Secret, TotpAlgorithm alg) {
    clear();
    algorithm_ = alg;
    if (decodeBase32(base32Secret, secretBytes_)) {
        secretBase32_ = encodeBase32(secretBytes_.data(), secretBytes_.size());
    } else {
        clear();
    }
}

void TotpManager::setSecretRaw(const uint8_t* key, size_t keyLen, TotpAlgorithm alg) {
    clear();
    algorithm_ = alg;
    if (key && keyLen > 0) {
        secretBytes_.assign(key, key + keyLen);
        secretBase32_ = encodeBase32(key, keyLen);
    }
}

void TotpManager::generateNewSecret(TotpAlgorithm alg) {
    clear();
    algorithm_ = alg;
    secretBytes_.resize(20); // 160-bit cryptographically secure random secret
    CryptoUtils::randomBytes(secretBytes_.data(), secretBytes_.size());
    secretBase32_ = encodeBase32(secretBytes_.data(), secretBytes_.size());
}

std::string TotpManager::generateCode(uint64_t unixTime, int digits) const {
    if (secretBytes_.empty()) {
        return "";
    }
    if (unixTime == 0) {
        unixTime = static_cast<uint64_t>(std::time(nullptr));
    }
    uint64_t timeStep = unixTime / 30;
    uint32_t codeNum = computeHotpCode(algorithm_, secretBytes_.data(), secretBytes_.size(), timeStep, digits);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*u", digits, codeNum);
    return std::string(buf);
}

bool TotpManager::verifyCode(const std::string& code, uint64_t unixTime, int driftSteps) {
    if (secretBytes_.empty() || code.empty()) {
        return false;
    }

    std::string cleanCode;
    for (char c : code) {
        if (c >= '0' && c <= '9') {
            cleanCode.push_back(c);
        } else if (c == ' ' || c == '-' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        } else {
            return false;
        }
    }

    if (cleanCode.length() != 6 && cleanCode.length() != 8) {
        return false;
    }
    int digits = static_cast<int>(cleanCode.length());

    if (unixTime == 0) {
        unixTime = static_cast<uint64_t>(std::time(nullptr));
    }
    uint64_t currentStep = unixTime / 30;

    std::vector<int> offsets;
    offsets.push_back(0);
    for (int d = 1; d <= driftSteps; ++d) {
        offsets.push_back(-d);
        offsets.push_back(d);
    }

    for (int offset : offsets) {
        int64_t stepCandidate = static_cast<int64_t>(currentStep) + offset;
        if (stepCandidate < 0) {
            continue;
        }
        uint64_t step = static_cast<uint64_t>(stepCandidate);
        uint32_t computed = computeHotpCode(algorithm_, secretBytes_.data(), secretBytes_.size(), step, digits);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%0*u", digits, computed);
        if (cleanCode == buf) {
            if (step <= lastConsumedStep_) {
                return false; // Replay attempt rejected
            }
            lastConsumedStep_ = step;
            return true;
        }
    }

    return false;
}

std::string TotpManager::buildOtpAuthUri(uint64_t deskId, const std::string& issuer) const {
    std::string algStr = (algorithm_ == TotpAlgorithm::Sha256) ? "SHA256" : "SHA1";
    return "otpauth://totp/" + issuer + ":" + std::to_string(deskId) +
           "?secret=" + secretBase32_ +
           "&issuer=" + issuer +
           "&algorithm=" + algStr +
           "&digits=6&period=30";
}

std::string TotpManager::formatBase32Secret(const std::string& rawBase32) {
    std::string clean;
    for (char c : rawBase32) {
        if (c != ' ' && c != '-' && c != '\t' && c != '\r' && c != '\n') {
            clean.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    std::string formatted;
    for (size_t i = 0; i < clean.length(); ++i) {
        if (i > 0 && (i % 4 == 0)) {
            formatted.push_back(' ');
        }
        formatted.push_back(clean[i]);
    }
    return formatted;
}

std::string TotpManager::encodeBase32(const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        return "";
    }
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string result;
    result.reserve(((length + 4) / 5) * 8);

    uint32_t buffer = 0;
    int bitsLeft = 0;
    for (size_t i = 0; i < length; ++i) {
        buffer = (buffer << 8) | data[i];
        bitsLeft += 8;
        while (bitsLeft >= 5) {
            bitsLeft -= 5;
            result.push_back(kAlphabet[(buffer >> bitsLeft) & 0x1F]);
        }
    }
    if (bitsLeft > 0) {
        buffer <<= (5 - bitsLeft);
        result.push_back(kAlphabet[buffer & 0x1F]);
    }
    while (result.size() % 8 != 0) {
        result.push_back('=');
    }
    return result;
}

bool TotpManager::decodeBase32(const std::string& input, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t buffer = 0;
    int bitsLeft = 0;

    for (char c : input) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-') {
            continue;
        }
        if (c == '=') {
            break; // Padding
        }
        int val = -1;
        if (c >= 'A' && c <= 'Z') {
            val = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            val = c - 'a';
        } else if (c >= '2' && c <= '7') {
            val = c - '2' + 26;
        } else {
            out.clear();
            return false;
        }

        buffer = (buffer << 5) | static_cast<uint32_t>(val);
        bitsLeft += 5;
        if (bitsLeft >= 8) {
            bitsLeft -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bitsLeft) & 0xFF));
        }
    }
    return true;
}

bool TotpManager::computeHmac(
    TotpAlgorithm alg,
    const uint8_t* key,
    size_t keyLen,
    const uint8_t* data,
    size_t dataLen,
    std::vector<uint8_t>& digestOut)
{
#if defined(_WIN32)
#ifndef BCRYPT_ALG_HANDLE_HMAC_FLAG
#define BCRYPT_ALG_HANDLE_HMAC_FLAG 0x00000008
#endif

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    LPCWSTR algId = (alg == TotpAlgorithm::Sha256) ? BCRYPT_SHA256_ALGORITHM : BCRYPT_SHA1_ALGORITHM;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, algId, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) {
        return false;
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    status = BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0);
    if (status < 0) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    ULONG digestLen = (alg == TotpAlgorithm::Sha256) ? 32 : 20;
    digestOut.resize(digestLen);
    status = BCryptFinishHash(hHash, digestOut.data(), digestLen, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return status >= 0;
#elif defined(__APPLE__)
    CCHmacAlgorithm ccAlg = (alg == TotpAlgorithm::Sha256) ? kCCHmacAlgSHA256 : kCCHmacAlgSHA1;
    size_t digestLen = (alg == TotpAlgorithm::Sha256) ? CC_SHA256_DIGEST_LENGTH : CC_SHA1_DIGEST_LENGTH;
    digestOut.resize(digestLen);
    CCHmac(ccAlg, key, keyLen, data, dataLen, digestOut.data());
    return true;
#else
    return false;
#endif
}

uint32_t TotpManager::computeHotpCode(
    TotpAlgorithm alg,
    const uint8_t* key,
    size_t keyLen,
    uint64_t timeStep,
    int digits)
{
    uint8_t counterBytes[8];
    for (int i = 7; i >= 0; --i) {
        counterBytes[i] = static_cast<uint8_t>(timeStep & 0xFF);
        timeStep >>= 8;
    }

    std::vector<uint8_t> digest;
    if (!computeHmac(alg, key, keyLen, counterBytes, sizeof(counterBytes), digest) || digest.empty()) {
        return 0;
    }

    int offset = digest[digest.size() - 1] & 0x0F;
    if (static_cast<size_t>(offset + 4) > digest.size()) {
        return 0;
    }

    uint32_t binaryCode = ((static_cast<uint32_t>(digest[offset]) & 0x7F) << 24)
                        | ((static_cast<uint32_t>(digest[offset + 1]) & 0xFF) << 16)
                        | ((static_cast<uint32_t>(digest[offset + 2]) & 0xFF) << 8)
                        | (static_cast<uint32_t>(digest[offset + 3]) & 0xFF);

    uint32_t mod = (digits >= 1 && digits <= 8) ? kPowersOf10[digits] : 1000000;
    return binaryCode % mod;
}

} // namespace cppdesk
