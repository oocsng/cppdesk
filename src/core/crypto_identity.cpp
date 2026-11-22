#include "crypto_identity.hpp"
#include "protocol.hpp"
#include "profile.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
// ECDH, PBKDF2, SHA-256, HMAC and AES-GCM use OpenSSL on Linux and macOS.
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/param_build.h>
#endif

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <thread>

namespace cppdesk {

namespace {

std::string generateRandomSixCharCode() {
    const char* chars = "23456789abcdefghjkmnpqrstuvwxyz";
    uint8_t rnd[6] = {};
    if (!CryptoUtils::randomBytes(rnd, sizeof(rnd))) {
        return "";
    }
    std::string code;
    code.reserve(6);
    for (int i = 0; i < 6; ++i) {
        code.push_back(chars[rnd[i] % 31]);
    }
    CryptoUtils::secureZero(rnd, sizeof(rnd));
    return code;
}

bool constantTimeEquals32(const std::array<uint8_t, 32>& a, const std::array<uint8_t, 32>& b) {
    uint8_t diff = 0;
    for (size_t i = 0; i < 32; ++i) {
        diff |= (a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace

bool CryptoUtils::randomBytes(void* buffer, size_t len) {
    if (len == 0) return true;
    if (!buffer) return false;
#if defined(_WIN32)
    NTSTATUS status = BCryptGenRandom(
        nullptr,
        static_cast<PUCHAR>(buffer),
        static_cast<ULONG>(len),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG
    );
    return status >= 0;
#elif defined(__APPLE__)
    return SecRandomCopyBytes(kSecRandomDefault, len, buffer) == errSecSuccess;
#elif defined(__linux__)
    uint8_t* p = static_cast<uint8_t*>(buffer);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::getrandom(p, remaining, 0);
        if (n > 0) {
            p += n;
            remaining -= static_cast<size_t>(n);
        } else if (n == -1 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
#else
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t rd = std::fread(buffer, 1, len, f);
    std::fclose(f);
    return rd == len;
#endif
}

uint64_t CryptoUtils::generateNineDigitId() {
    uint64_t rnd = 0;
    if (!randomBytes(&rnd, sizeof(rnd))) {
        return 0;
    }
    return 100000000ULL + (rnd % 900000000ULL);
}

std::array<uint8_t, 32> CryptoUtils::sha256(const void* data, size_t len) {
    std::array<uint8_t, 32> digest{};
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) std::abort();

    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        std::abort();
    }
    if (len > 0 && data != nullptr) {
        status = BCryptHashData(
            hHash,
            const_cast<PUCHAR>(static_cast<const uint8_t*>(data)),
            static_cast<ULONG>(len),
            0
        );
        if (status < 0) {
            BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            std::abort();
        }
    }
    status = BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (status < 0) std::abort();
#elif defined(__APPLE__)
    CC_SHA256(data, static_cast<CC_LONG>(len), digest.data());
#else
    unsigned int outLen = 0;
    if (EVP_Digest(data, len, digest.data(), &outLen, EVP_sha256(), nullptr) != 1 || outLen != 32) {
        std::abort();
    }
#endif
    return digest;
}

std::array<uint8_t, 32> CryptoUtils::sha256(const std::string& text) {
    return sha256(text.data(), text.size());
}

std::string CryptoUtils::toHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::vector<uint8_t> CryptoUtils::fromHex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = hexVal(hex[i]);
        int lo = hexVal(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return bytes;
}

std::string CryptoUtils::sha256Hex(const std::string& text) {
    auto d = sha256(text);
    return toHex(d.data(), d.size());
}

bool CryptoUtils::pbkdf2Sha256(
    const std::string& password,
    const uint8_t* salt,
    size_t saltLen,
    uint32_t iterations,
    std::array<uint8_t, 32>& outDerivedKey)
{
    outDerivedKey.fill(0);
    if (!salt || saltLen == 0 || iterations == 0) return false;
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) return false;
    status = BCryptDeriveKeyPBKDF2(
        hAlg,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt),
        static_cast<ULONG>(saltLen),
        static_cast<ULONGLONG>(iterations),
        outDerivedKey.data(),
        static_cast<ULONG>(outDerivedKey.size()),
        0
    );
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return status >= 0;
#else
    return PKCS5_PBKDF2_HMAC(
        password.data(),
        static_cast<int>(password.size()),
        salt,
        static_cast<int>(saltLen),
        static_cast<int>(iterations),
        EVP_sha256(),
        static_cast<int>(outDerivedKey.size()),
        outDerivedKey.data()) == 1;
#endif
}

std::string CryptoUtils::hashPassword(const std::string& password, uint32_t iterations) {
    uint32_t clampedIter = std::clamp(iterations, kMinPbkdf2Iterations, kMaxPbkdf2Iterations);
    std::array<uint8_t, 16> salt{};
    if (!randomBytes(salt.data(), salt.size())) return "";
    std::array<uint8_t, 32> dk{};
    if (!pbkdf2Sha256(password, salt.data(), salt.size(), clampedIter, dk)) {
        secureZero(dk.data(), dk.size());
        return "";
    }
    std::string result = "pbkdf2_sha256$" + std::to_string(clampedIter) + "$"
                       + toHex(salt.data(), salt.size()) + "$"
                       + toHex(dk.data(), dk.size());
    secureZero(dk.data(), dk.size());
    return result;
}

bool CryptoUtils::parsePasswordVerifier(
    const std::string& verifier,
    uint32_t& outIterations,
    std::array<uint8_t, 16>& outSalt,
    std::array<uint8_t, 32>& outDerivedKey)
{
    static const std::string kPrefix = "pbkdf2_sha256$";
    if (verifier.rfind(kPrefix, 0) != 0) return false;
    size_t p1 = kPrefix.size();
    size_t d1 = verifier.find('$', p1);
    if (d1 == std::string::npos) return false;
    size_t d2 = verifier.find('$', d1 + 1);
    if (d2 == std::string::npos || verifier.find('$', d2 + 1) != std::string::npos) return false;

    uint32_t iter = 0;
    try {
        std::string iterStr = verifier.substr(p1, d1 - p1);
        if (iterStr.empty()) return false;
        unsigned long long v = std::stoull(iterStr);
        if (v < kMinPbkdf2Iterations || v > kMaxPbkdf2Iterations) return false;
        iter = static_cast<uint32_t>(v);
    } catch (...) {
        return false;
    }

    std::string saltHex = verifier.substr(d1 + 1, d2 - (d1 + 1));
    std::string dkHex = verifier.substr(d2 + 1);
    if (saltHex.size() != 32 || dkHex.size() != 64) return false;

    auto saltBytes = fromHex(saltHex);
    auto dkBytes = fromHex(dkHex);
    if (saltBytes.size() != 16 || dkBytes.size() != 32) return false;

    outIterations = iter;
    std::copy(saltBytes.begin(), saltBytes.end(), outSalt.begin());
    std::copy(dkBytes.begin(), dkBytes.end(), outDerivedKey.begin());
    secureZero(dkBytes.data(), dkBytes.size());
    return true;
}

std::array<uint8_t, 32> CryptoUtils::computeChallengeResponseFromToken(
    const std::string& passwordVerifierToken,
    uint64_t hostId,
    uint64_t clientId,
    const std::array<uint8_t, 32>& nonce)
{
    ByteWriter w;
    w.writeString(passwordVerifierToken);
    w.writeU64(hostId);
    w.writeU64(clientId);
    w.writeBytes(nonce.data(), nonce.size());
    return sha256(w.buffer().data(), w.buffer().size());
}

std::array<uint8_t, 32> CryptoUtils::computeChallengeResponse(
    const std::string& passwordPlain,
    const std::array<uint8_t, 16>& salt,
    uint32_t iterations,
    uint64_t hostId,
    uint64_t clientId,
    const std::array<uint8_t, 32>& nonce)
{
    std::array<uint8_t, 32> dk{};
    if (!pbkdf2Sha256(passwordPlain, salt.data(), salt.size(), iterations, dk)) {
        return std::array<uint8_t, 32>{};
    }
    std::string passToken = toHex(dk.data(), dk.size());
    auto digest = computeChallengeResponseFromToken(passToken, hostId, clientId, nonce);
    secureZero(passToken.data(), passToken.size());
    secureZero(dk.data(), dk.size());
    return digest;
}

std::array<uint8_t, 32> CryptoUtils::bindChallengeResponse(
    const std::array<uint8_t, 32>& sessionKey,
    const std::array<uint8_t, 32>& digest)
{
    static const char kLabel[] = "CppDesk-auth-bound-v1";
    std::vector<uint8_t> msg(kLabel, kLabel + sizeof(kLabel) - 1);
    msg.insert(msg.end(), digest.begin(), digest.end());
    return hmacSha256(sessionKey.data(), sessionKey.size(), msg.data(), msg.size());
}

void CryptoUtils::secureZero(void* ptr, size_t len) {
    crypto_secure_zero(ptr, len);
}

bool CryptoUtils::pinMemory(void* ptr, size_t len) {
    return crypto_pin_memory(ptr, len);
}

void CryptoUtils::unpinMemory(void* ptr, size_t len) {
    crypto_unpin_memory(ptr, len);
}

std::array<uint8_t, 32> CryptoUtils::hmacSha256(const void* key, size_t keyLen, const void* msg, size_t msgLen) {
    std::array<uint8_t, 32> digest{};
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) std::abort();

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                              const_cast<PUCHAR>(static_cast<const uint8_t*>(key)),
                              static_cast<ULONG>(keyLen), 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        std::abort();
    }
    if (msg && msgLen > 0) {
        status = BCryptHashData(hHash,
                                const_cast<PUCHAR>(static_cast<const uint8_t*>(msg)),
                                static_cast<ULONG>(msgLen), 0);
        if (status < 0) {
            BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            std::abort();
        }
    }
    status = BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (status < 0) std::abort();
    return digest;
#elif defined(__APPLE__)
    CCHmac(kCCHmacAlgSHA256, key, keyLen, msg, msgLen, digest.data());
    return digest;
#else
    unsigned int outLen = 0;
    if (!HMAC(EVP_sha256(), key, static_cast<int>(keyLen),
              static_cast<const unsigned char*>(msg), msgLen,
              digest.data(), &outLen) || outLen != 32) {
        std::abort();
    }
    return digest;
#endif
}

std::array<uint8_t, 32> CryptoUtils::hkdfSha256(
    const void* ikm, size_t ikmLen,
    const void* salt, size_t saltLen,
    const std::string& info)
{
    // HKDF-Extract: PRK = HMAC-Hash(salt, IKM)
    std::array<uint8_t, 32> prk = hmacSha256(salt, saltLen, ikm, ikmLen);

    // HKDF-Expand: OKM = HMAC-Hash(PRK, info || 0x01)
    std::vector<uint8_t> infoBytes(info.begin(), info.end());
    infoBytes.push_back(0x01);
    std::array<uint8_t, 32> okm = hmacSha256(prk.data(), prk.size(), infoBytes.data(), infoBytes.size());

    secureZero(prk.data(), prk.size());
    return okm;
}

std::array<uint8_t, 32> CryptoUtils::deriveSessionKey(
    uint64_t hostId,
    uint64_t clientId,
    const std::array<uint8_t, 32>& nonce,
    const uint8_t* ecdhSecret,
    size_t ecdhSecretLen)
{
    if (ecdhSecret && ecdhSecretLen > 0) {
        ByteWriter infoW;
        infoW.writeString("CppDesk-ECDH-AES256GCM-v3");
        infoW.writeU64(hostId);
        infoW.writeU64(clientId);
        std::string infoStr(reinterpret_cast<const char*>(infoW.buffer().data()), infoW.buffer().size());
        return hkdfSha256(ecdhSecret, ecdhSecretLen, nonce.data(), nonce.size(), infoStr);
    }

    return std::array<uint8_t, 32>{};
}

std::string CryptoUtils::sessionFingerprintHex(const std::array<uint8_t, 32>& sessionKey) {
    auto digest = sha256(sessionKey.data(), sessionKey.size());
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02X%02X-%02X%02X",
                  digest[0], digest[1], digest[2], digest[3]);
    return std::string(buf);
}

std::string CryptoUtils::formatDeskId(uint64_t id) {
    if (id < 100000000ULL || id > 999999999ULL) {
        return std::to_string(id);
    }
    char buf[32];
    uint32_t p1 = static_cast<uint32_t>(id / 1000000ULL);
    uint32_t p2 = static_cast<uint32_t>((id / 1000ULL) % 1000ULL);
    uint32_t p3 = static_cast<uint32_t>(id % 1000ULL);
    std::snprintf(buf, sizeof(buf), "%03u %03u %03u", p1, p2, p3);
    return std::string(buf);
}

uint64_t CryptoUtils::parseDeskId(const std::string& input) {
    if (input.find('.') != std::string::npos || input.find(':') != std::string::npos) {
        return 0;
    }
    std::string digits;
    digits.reserve(input.size());
    for (char c : input) {
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
        } else if (c != ' ' && c != '-' && c != '\t') {
            return 0;
        }
    }
    if (digits.size() != 9) return 0;
    try {
        uint64_t val = std::stoull(digits);
        if (val >= 100000000ULL && val <= 999999999ULL) {
            return val;
        }
    } catch (...) {}
    return 0;
}

// ---------------- IdentityManager ----------------

namespace {

// "<deskId>|<pub 128 hex>|<pairedAtUnix>|<hostname>"; false (entry skipped) on anything malformed.
bool parsePairedPeer(const std::string& val, PairedPeer& out) {
    auto number = [](const std::string& s, uint64_t& n) {
        if (s.empty() || s.size() > 19) return false;
        n = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + static_cast<uint64_t>(c - '0');
        }
        return true;
    };
    size_t b1 = val.find('|');
    size_t b2 = b1 == std::string::npos ? b1 : val.find('|', b1 + 1);
    size_t b3 = b2 == std::string::npos ? b2 : val.find('|', b2 + 1);
    if (b3 == std::string::npos) return false;
    std::string hex = val.substr(b1 + 1, b2 - b1 - 1);
    if (hex.size() != out.publicKey.size() * 2) return false;
    for (char c : hex) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    if (!number(val.substr(0, b1), out.deskId) || out.deskId == 0) return false;
    if (!number(val.substr(b2 + 1, b3 - b2 - 1), out.pairedAtUnix)) return false;
    auto key = CryptoUtils::fromHex(hex);
    if (key.size() != out.publicKey.size()) return false;
    std::copy(key.begin(), key.end(), out.publicKey.begin());
    out.hostname = val.substr(b3 + 1);
    return true;
}

void upsertPeer(std::vector<PairedPeer>& list, const PairedPeer& p) {
    for (auto& e : list) {
        if (e.deskId == p.deskId) { e = p; return; }
    }
    list.push_back(p);
}

std::optional<PairedPeer> findPeer(const std::vector<PairedPeer>& list, uint64_t deskId) {
    for (const auto& p : list) if (p.deskId == deskId) return p;
    return std::nullopt;
}

bool erasePeer(std::vector<PairedPeer>& list, uint64_t deskId) {
    auto it = std::remove_if(list.begin(), list.end(), [&](const PairedPeer& p) { return p.deskId == deskId; });
    if (it == list.end()) return false;
    list.erase(it, list.end());
    return true;
}

} // namespace

IdentityManager::IdentityManager(int instanceId)
    : instanceId_(std::max(1, instanceId))
{
    listenPort_ = static_cast<uint16_t>(DEFAULT_HOST_PORT + (instanceId_ - 1));
    sessionCode_ = generateRandomSixCharCode();

#if defined(_WIN32)
    char compName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD sz = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameA(compName, &sz)) {
        hostname_ = compName;
    } else {
        hostname_ = "Windows-PC";
    }
#elif defined(__APPLE__)
    char compName[256] = {};
    if (gethostname(compName, sizeof(compName)) == 0) {
        hostname_ = compName;
    } else {
        hostname_ = "Mac-Desktop";
    }
#else
    char compName[256] = {};
    if (gethostname(compName, sizeof(compName)) == 0) {
        hostname_ = compName;
    } else {
        hostname_ = "CppDesk-Host";
    }
#endif
    if (instanceId_ > 1) {
        hostname_ += " #" + std::to_string(instanceId_);
    }

#if defined(_WIN32)
    wchar_t appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        std::filesystem::path dir = std::filesystem::path(appData) / L"CppDesk";
        std::filesystem::path oldDir = std::filesystem::path(appData) / L"AeroDesk";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        // One-time automatic migration from AeroDesk
        if (std::filesystem::exists(oldDir, ec)) {
            std::filesystem::path oldCfg = oldDir / ("config_" + std::to_string(instanceId_) + ".ini");
            std::filesystem::path newCfg = dir / ("config_" + std::to_string(instanceId_) + ".ini");
            if (std::filesystem::exists(oldCfg, ec) && !std::filesystem::exists(newCfg, ec)) {
                std::filesystem::copy_file(oldCfg, newCfg, std::filesystem::copy_options::overwrite_existing, ec);
            }
        }

        configPath_ = dir / ("config_" + std::to_string(instanceId_) + ".ini");
    } else {
        std::filesystem::path oldLocal = "aerodesk_" + std::to_string(instanceId_) + ".ini";
        std::filesystem::path newLocal = "cppdesk_" + std::to_string(instanceId_) + ".ini";
        std::error_code ec;
        if (std::filesystem::exists(oldLocal, ec) && !std::filesystem::exists(newLocal, ec)) {
            std::filesystem::copy_file(oldLocal, newLocal, std::filesystem::copy_options::overwrite_existing, ec);
        }
        configPath_ = newLocal;
    }
#else
    const char* home = std::getenv("HOME");
    std::filesystem::path dir = home ? (std::filesystem::path(home) / ".cppdesk") : std::filesystem::path(".cppdesk");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ::chmod(dir.c_str(), 0700);
    configPath_ = dir / ("config_" + std::to_string(instanceId_) + ".ini");
#endif
}

std::string IdentityManager::displayName() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return profile::normaliseName(settings_.displayName);
}

std::string IdentityManager::wireName(size_t maxBytes) const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return profile::composeWireName(profile::normaliseName(settings_.displayName), hostname_, maxBytes);
}

std::filesystem::path IdentityManager::ownPicturePath() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return profile::ownPicturePath(configPath_, instanceId_);
}

std::filesystem::path IdentityManager::peerPicturePath(uint64_t deskId) const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return profile::peerPicturePath(configPath_, instanceId_, deskId);
}

std::string IdentityManager::configFilePath() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    auto u8 = configPath_.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

void IdentityManager::setConfigPathForTest(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    configPath_ = path;
}

bool IdentityManager::loadOrCreate() {
    std::string legacyPlainPassword;
    bool droppedLegacyVerifier = false;
    bool modified = false;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        std::ifstream in(configPath_);
        if (in.is_open()) {
            std::string line;
            recentSessions_.clear();
            std::lock_guard<std::mutex> pairLock(pairMutex_);
            pairedViewers_.clear();
            pairedHosts_.clear();
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);

                if (key == "desk_id") {
                    try { deskId_ = std::stoull(val); } catch (...) {}
                } else if (key == "listen_port") {
                    try { listenPort_ = static_cast<uint16_t>(std::stoi(val)); } catch (...) {}
                } else if (key == "unattended_enabled") {
                    unattendedEnabled_ = (val == "1" || val == "true");
                } else if (key == "unattended_verifier") {
                    uint32_t vIter = 0;
                    std::array<uint8_t, 16> vSalt{};
                    std::array<uint8_t, 32> vDk{};
                    if (CryptoUtils::parsePasswordVerifier(val, vIter, vSalt, vDk)) {
                        unattendedVerifier_ = val;
                        CryptoUtils::secureZero(vDk.data(), vDk.size());
                    } else if (!val.empty()) {
                        unattendedVerifier_.clear();
                        droppedLegacyVerifier = true;
                    }
                } else if (key == "unattended_password") {
                legacyPlainPassword = val;
            } else if (key == "relay_server") {
                if (!val.empty()) {
                    relayServerAddr_ = val;
                    settings_.relayServer = val;
                }
            } else if (key == "relay_auth_key") {
                settings_.relayAuthKey = val;
            } else if (key == "display_name") {
                settings_.displayName = val;
            } else if (key == "stun_server") {
                if (!val.empty()) settings_.stunServer = val;
            } else if (key == "relay_mode") {
                try { settings_.relayMode = static_cast<uint8_t>(std::clamp(std::stoi(val), 0, 2)); } catch (...) {}
            } else if (key == "dark_theme") {
                settings_.darkTheme = (val == "1" || val == "true");
            } else if (key == "reduce_motion") {
                try { settings_.reduceMotion = static_cast<uint8_t>(std::clamp(std::stoi(val), 0, 2)); } catch (...) {}
            } else if (key == "target_fps") {
                try { settings_.targetFps = clampTargetFps(static_cast<uint8_t>(std::stoi(val))); } catch (...) {}
            } else if (key == "adaptive_fps") {
                settings_.adaptiveFps = (val == "1" || val == "true");
            } else if (key == "default_quality") {
                try {
                    int q = std::clamp(std::stoi(val), 0, 2);
                    settings_.defaultQuality = static_cast<QualityPreset>(q);
                } catch (...) {}
            } else if (key == "connection_profile") {
                try {
                    int cp = std::clamp(std::stoi(val), 0, 4);
                    settings_.connectionProfile = static_cast<ConnectionProfile>(cp);
                } catch (...) {}
            } else if (key == "default_scale") {
                try {
                    settings_.defaultScaleMode = static_cast<uint8_t>(std::clamp(std::stoi(val), 0, 2));
                } catch (...) {}
            } else if (key == "show_remote_cursor") {
                settings_.showRemoteCursor = (val == "1" || val == "true");
            } else if (key == "show_session_hud") {
                settings_.showSessionHud = (val == "1" || val == "true");
            } else if (key == "auto_accept") {
                settings_.autoAcceptIncoming = (val == "1" || val == "true");
            } else if (key == "default_perms") {
                try {
                    settings_.defaultPermissions = static_cast<uint8_t>(std::stoi(val) & PERM_ALL);
                } catch (...) {}
            } else if (key == "lock_on_disconnect") {
                settings_.lockWorkstationOnDisconnect = (val == "1" || val == "true");
            } else if (key == "push_notifications") {
                settings_.enablePushNotifications = (val == "1" || val == "true");
            } else if (key == "taskbar_flash") {
                settings_.enableTaskbarFlash = (val == "1" || val == "true");
            } else if (key == "notification_sound") {
                settings_.enableNotificationSounds = (val == "1" || val == "true");
            } else if (key == "minimize_to_tray") {
                settings_.minimizeToTray = (val == "1" || val == "true");
            } else if (key == "hardware_acceleration") {
                settings_.hardwareAcceleration = (val == "1" || val == "true");
            } else if (key == "default_audio_volume") {
                try {
                    int v = std::stoi(val);
                    settings_.defaultAudioVolume = static_cast<uint8_t>(std::clamp(v, 0, 100));
                } catch (...) {}
            } else if (key == "audio_muted_default") {
                settings_.audioMutedDefault = (val == "1" || val == "true");
            } else if (key == "audio_input_device_id") {
                try { settings_.audioInputDeviceId = std::stoi(val); } catch (...) {}
            } else if (key == "audio_output_device_id") {
                try { settings_.audioOutputDeviceId = std::stoi(val); } catch (...) {}
            } else if (key == "audio_input_device_name") {
                settings_.audioInputDeviceName = val;
            } else if (key == "audio_output_device_name") {
                settings_.audioOutputDeviceName = val;
            } else if (key == "receive_dir") {
                settings_.receiveDir = val;
            } else if (key == "wayland_restore_token") {
                if (val.size() <= 256) settings_.waylandRestoreToken = val;
            } else if (key == "auto_virtual_display") {
                settings_.autoVirtualDisplay = (val == "1" || val == "true");
            } else if (key == "privacy_notice") {
                settings_.privacyCustomNotice = val;
            } else if (key == "privacy_brand") {
                settings_.privacyBrandName = val;
            } else if (key == "privacy_show_id") {
                settings_.privacyShowDeskId = (val == "1" || val == "true");
            } else if (key == "hardware_dpms_blanking") {
                settings_.hardwareDpmsBlanking = (val == "1" || val == "true");
            } else if (key == "totp_enabled") {
                settings_.totpEnabled = (val == "1" || val == "true");
            } else if (key == "totp_secret") {
                settings_.totpSecret = val;
            } else if (key == "totp_algorithm") {
                settings_.totpAlgorithm = val;
            } else if (key == "privacy_cloak_enabled") {
                settings_.privacyCloakEnabled = (val == "1" || val == "true");
            } else if (key == "privacy_cloak_keywords") {
                settings_.privacyCloakKeywords = val;
            } else if (key == "privacy_cloak_style") {
                settings_.privacyCloakStyle = static_cast<uint8_t>(std::atoi(val.c_str()));
            } else if (key == "allow_pairing") {
                settings_.allowPairing = (val == "1" || val == "true");
            } else if (key == "glide_left") {
                try { settings_.glideLeft = std::stoull(val); } catch (...) {}
            } else if (key == "glide_right") {
                try { settings_.glideRight = std::stoull(val); } catch (...) {}
            } else if (key == "glide_top") {
                try { settings_.glideTop = std::stoull(val); } catch (...) {}
            } else if (key == "glide_bottom") {
                try { settings_.glideBottom = std::stoull(val); } catch (...) {}
            } else if (key == "pair_key") {
                auto bytes = CryptoUtils::fromHex(val);
                PairingKey::KeyPair kp{};
                if (val.size() == kp.size() * 2 && bytes.size() == kp.size()) {
                    std::copy(bytes.begin(), bytes.end(), kp.begin());
                    pairingKey_.importKeyPair(kp);
                }
            } else if (key == "paired_viewer" || key == "paired_host") {
                PairedPeer peer;
                if (parsePairedPeer(val, peer)) upsertPeer(key == "paired_viewer" ? pairedViewers_ : pairedHosts_, peer);
            } else if (key == "recent") {
                // Backward-compatible pipe format:
                // 3 fields: deskId|hostname|address
                // 4 fields: deskId|hostname|address|fav
                // 7 fields: deskId|hostname|address|fav|alias|tag|notes
                std::vector<std::string> parts;
                size_t start = 0;
                while (start < val.size()) {
                    auto bar = val.find('|', start);
                    if (bar == std::string::npos) {
                        parts.push_back(val.substr(start));
                        break;
                    }
                    parts.push_back(val.substr(start, bar - start));
                    start = bar + 1;
                }
                if (parts.size() >= 3) {
                    RecentSessionEntry entry;
                    try { entry.deskId = std::stoull(parts[0]); } catch (...) {}
                    entry.hostname = parts[1];
                    entry.address = parts[2];
                    if (parts.size() >= 4) {
                        entry.isFavorite = (parts[3] == "1" || parts[3] == "true");
                    }
                    if (parts.size() >= 5) entry.alias = parts[4];
                    if (parts.size() >= 6) entry.tag = parts[5];
                    if (parts.size() >= 7) entry.notes = parts[6];

                    if (entry.deskId > 0 || !entry.address.empty()) {
                        recentSessions_.push_back(entry);
                    }
                }
            }
        }
        in.close();
    }

        settings_.targetFps = clampTargetFps(settings_.targetFps);

        if (!settings_.totpSecret.empty()) {
            TotpAlgorithm alg = (settings_.totpAlgorithm == "SHA256") ? TotpAlgorithm::Sha256 : TotpAlgorithm::Sha1;
            totpManager_.setSecret(settings_.totpSecret, alg);
        }

        if (deskId_ < 100000000ULL || deskId_ > 999999999ULL) {
            deskId_ = CryptoUtils::generateNineDigitId();
            if (deskId_ < 100000000ULL || deskId_ > 999999999ULL) {
                return false;
            }
            modified = true;
        }

        if (sessionCode_.empty()) {
            sessionCode_ = generateRandomSixCharCode();
        }
        bool hasSessionSalt = false;
        for (uint8_t b : sessionSalt_) {
            if (b != 0) { hasSessionSalt = true; break; }
        }
        if (!hasSessionSalt) {
            if (!CryptoUtils::randomBytes(sessionSalt_.data(), sessionSalt_.size())) {
                return false;
            }
        }

        if (!pairingKey_.isValid()) {
            pairingKey_.generate();
            modified = true;
        }

        if (droppedLegacyVerifier) {
            std::fprintf(stderr, "[CppDesk] Dropped legacy unattended password verifier; please set your unattended password again.\n");
            modified = true;
        }

        // Transparently migrate legacy plaintext password to PBKDF2-HMAC-SHA256 verifier
        if (!legacyPlainPassword.empty() && unattendedVerifier_.empty()) {
            unattendedVerifier_ = CryptoUtils::hashPassword(legacyPlainPassword);
            CryptoUtils::secureZero(legacyPlainPassword.data(), legacyPlainPassword.size());
            modified = true;
        }

        if (!unattendedEnabled_) {
            unattendedVerifier_.clear();
        }
    }

    if (modified) {
        save();
    }
    return true;
}

bool IdentityManager::save() const {
    // Host, viewer and UI threads all save: one at a time, into a temp file renamed over the config, so it never tears.
    std::lock_guard<std::mutex> saveLock(saveMutex_);
    std::filesystem::path targetPath;
    std::ostringstream out;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        targetPath = configPath_;

        auto sanitizeIniLine = [](const std::string& in) -> std::string {
            std::string res;
            res.reserve(in.size());
            for (char c : in) {
                if (c == '\r' || c == '\n') {
                    res.push_back(' ');
                } else {
                    res.push_back(c);
                }
            }
            return res;
        };
        auto sanitizeIniValue = [](const std::string& in) -> std::string {
            std::string res;
            res.reserve(in.size());
            for (char c : in) {
                if (c == '|' || c == '\r' || c == '\n') {
                    res.push_back(' ');
                } else {
                    res.push_back(c);
                }
            }
            return res;
        };

        out << "# CppDesk Configuration (PBKDF2-HMAC-SHA256 Verifier Protected)\n";
        out << "desk_id=" << deskId_ << "\n";
        out << "listen_port=" << listenPort_ << "\n";
        out << "unattended_enabled=" << (unattendedEnabled_ ? "1" : "0") << "\n";
        out << "unattended_verifier=" << sanitizeIniLine(unattendedVerifier_) << "\n";
        out << "relay_server=" << sanitizeIniLine(settings_.relayServer.empty() ? relayServerAddr_ : settings_.relayServer) << "\n";
        out << "relay_auth_key=" << sanitizeIniLine(settings_.relayAuthKey) << "\n";
        out << "stun_server=" << sanitizeIniLine(settings_.stunServer) << "\n";
        out << "display_name=" << sanitizeIniLine(settings_.displayName) << "\n";
        out << "relay_mode=" << static_cast<int>(settings_.relayMode) << "\n";
        out << "dark_theme=" << (settings_.darkTheme ? "1" : "0") << "\n";
        out << "reduce_motion=" << static_cast<int>(settings_.reduceMotion) << "\n";
        out << "target_fps=" << static_cast<int>(clampTargetFps(settings_.targetFps)) << "\n";
        out << "adaptive_fps=" << (settings_.adaptiveFps ? "1" : "0") << "\n";
        out << "default_quality=" << static_cast<int>(settings_.defaultQuality) << "\n";
        out << "connection_profile=" << static_cast<int>(settings_.connectionProfile) << "\n";
        out << "default_scale=" << static_cast<int>(settings_.defaultScaleMode) << "\n";
        out << "show_remote_cursor=" << (settings_.showRemoteCursor ? "1" : "0") << "\n";
        out << "show_session_hud=" << (settings_.showSessionHud ? "1" : "0") << "\n";
        out << "auto_accept=" << (settings_.autoAcceptIncoming ? "1" : "0") << "\n";
        out << "default_perms=" << static_cast<int>(settings_.defaultPermissions & PERM_ALL) << "\n";
        out << "lock_on_disconnect=" << (settings_.lockWorkstationOnDisconnect ? "1" : "0") << "\n";
        out << "push_notifications=" << (settings_.enablePushNotifications ? "1" : "0") << "\n";
        out << "taskbar_flash=" << (settings_.enableTaskbarFlash ? "1" : "0") << "\n";
        out << "notification_sound=" << (settings_.enableNotificationSounds ? "1" : "0") << "\n";
        out << "minimize_to_tray=" << (settings_.minimizeToTray ? "1" : "0") << "\n";
        out << "hardware_acceleration=" << (settings_.hardwareAcceleration ? "1" : "0") << "\n";
        out << "default_audio_volume=" << static_cast<int>(settings_.defaultAudioVolume) << "\n";
        out << "audio_muted_default=" << (settings_.audioMutedDefault ? "1" : "0") << "\n";
        out << "audio_input_device_id=" << settings_.audioInputDeviceId << "\n";
        out << "audio_output_device_id=" << settings_.audioOutputDeviceId << "\n";
        out << "audio_input_device_name=" << sanitizeIniLine(settings_.audioInputDeviceName) << "\n";
        out << "audio_output_device_name=" << sanitizeIniLine(settings_.audioOutputDeviceName) << "\n";
        out << "receive_dir=" << sanitizeIniLine(settings_.receiveDir) << "\n";
        out << "wayland_restore_token=" << sanitizeIniLine(settings_.waylandRestoreToken) << "\n";
        out << "auto_virtual_display=" << (settings_.autoVirtualDisplay ? "1" : "0") << "\n";
        out << "privacy_notice=" << sanitizeIniValue(settings_.privacyCustomNotice) << "\n";
        out << "privacy_brand=" << sanitizeIniValue(settings_.privacyBrandName) << "\n";
        out << "privacy_show_id=" << (settings_.privacyShowDeskId ? "1" : "0") << "\n";
        out << "hardware_dpms_blanking=" << (settings_.hardwareDpmsBlanking ? "1" : "0") << "\n";
        out << "totp_enabled=" << (settings_.totpEnabled ? "1" : "0") << "\n";
        out << "totp_secret=" << sanitizeIniLine(settings_.totpSecret) << "\n";
        out << "totp_algorithm=" << sanitizeIniLine(settings_.totpAlgorithm) << "\n";
        out << "privacy_cloak_enabled=" << (settings_.privacyCloakEnabled ? "1" : "0") << "\n";
        out << "privacy_cloak_keywords=" << sanitizeIniValue(settings_.privacyCloakKeywords) << "\n";
        out << "privacy_cloak_style=" << static_cast<int>(settings_.privacyCloakStyle) << "\n";
        out << "allow_pairing=" << (settings_.allowPairing ? "1" : "0") << "\n";
        out << "glide_left=" << settings_.glideLeft << "\n";
        out << "glide_right=" << settings_.glideRight << "\n";
        out << "glide_top=" << settings_.glideTop << "\n";
        out << "glide_bottom=" << settings_.glideBottom << "\n";
        if (pairingKey_.isValid()) {
            auto kp = pairingKey_.exportKeyPair();
            std::string kpHex = CryptoUtils::toHex(kp.data(), kp.size());
            out << "pair_key=" << kpHex << "\n";
            CryptoUtils::secureZero(kpHex.data(), kpHex.size());
            CryptoUtils::secureZero(kp.data(), kp.size());
        }

        for (const auto& r : recentSessions_) {
            out << "recent=" << r.deskId << "|" << sanitizeIniValue(r.hostname) << "|" << sanitizeIniValue(r.address) << "|" << (r.isFavorite ? "1" : "0")
                << "|" << sanitizeIniValue(r.alias) << "|" << sanitizeIniValue(r.tag) << "|" << sanitizeIniValue(r.notes) << "\n";
        }
        auto writePeers = [&](const char* name, const std::vector<PairedPeer>& list) {
            for (const auto& p : list) {
                out << name << "=" << p.deskId << "|" << CryptoUtils::toHex(p.publicKey.data(), p.publicKey.size())
                    << "|" << p.pairedAtUnix << "|" << sanitizeIniValue(p.hostname) << "\n";
            }
        };
        {
            std::lock_guard<std::mutex> pairLock(pairMutex_);
            writePeers("paired_viewer", pairedViewers_);
            writePeers("paired_host", pairedHosts_);
        }
    }

    std::string content = out.str();
    std::filesystem::path tmpPath = targetPath;
    tmpPath += ".tmp";
    std::error_code ec;

#if defined(_WIN32)
    {
        std::ofstream fileOut(tmpPath, std::ios::binary | std::ios::trunc);
        if (!fileOut.is_open()) {
            CryptoUtils::secureZero(content.data(), content.size());
            return false;
        }
        fileOut.write(content.data(), static_cast<std::streamsize>(content.size()));
        fileOut.close();
        if (!fileOut) {
            CryptoUtils::secureZero(content.data(), content.size());
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }
#else
    {
        // Create owner-only (0600) before any secret is written; never follow symlinks.
        int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            CryptoUtils::secureZero(content.data(), content.size());
            return false;
        }
        if (::fchmod(fd, 0600) != 0) {
            ::close(fd);
            ::unlink(tmpPath.c_str());
            CryptoUtils::secureZero(content.data(), content.size());
            return false;
        }
        const char* p = content.data();
        size_t rem = content.size();
        bool writeOk = true;
        while (rem > 0) {
            ssize_t n = ::write(fd, p, rem);
            if (n > 0) {
                p += n;
                rem -= static_cast<size_t>(n);
            } else if (n == -1 && errno == EINTR) {
                continue;
            } else {
                writeOk = false;
                break;
            }
        }
        if (::close(fd) != 0) writeOk = false;
        if (!writeOk) {
            ::unlink(tmpPath.c_str());
            CryptoUtils::secureZero(content.data(), content.size());
            return false;
        }
    }
#endif

    CryptoUtils::secureZero(content.data(), content.size());
    for (int attempt = 0; attempt < 50; ++attempt) {   // a reader holding the config open on Windows blocks the replace briefly
        std::filesystem::rename(tmpPath, targetPath, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::filesystem::remove(tmpPath, ec);
    return false;
}

std::vector<PairedPeer> IdentityManager::pairedViewers() const {
    std::lock_guard<std::mutex> lock(pairMutex_);
    return pairedViewers_;
}

std::vector<PairedPeer> IdentityManager::pairedHosts() const {
    std::lock_guard<std::mutex> lock(pairMutex_);
    return pairedHosts_;
}

std::optional<PairedPeer> IdentityManager::findPairedViewer(uint64_t deskId) const {
    std::lock_guard<std::mutex> lock(pairMutex_);
    return findPeer(pairedViewers_, deskId);
}

std::optional<PairedPeer> IdentityManager::findPairedHost(uint64_t deskId) const {
    std::lock_guard<std::mutex> lock(pairMutex_);
    return findPeer(pairedHosts_, deskId);
}

void IdentityManager::addPairedViewer(const PairedPeer& p) {
    { std::lock_guard<std::mutex> lock(pairMutex_); upsertPeer(pairedViewers_, p); }
    save();
}

void IdentityManager::addPairedHost(const PairedPeer& p) {
    { std::lock_guard<std::mutex> lock(pairMutex_); upsertPeer(pairedHosts_, p); }
    save();
}

bool IdentityManager::removePairedViewer(uint64_t deskId) {
    bool removed;
    { std::lock_guard<std::mutex> lock(pairMutex_); removed = erasePeer(pairedViewers_, deskId); }
    if (removed) save();
    return removed;
}

bool IdentityManager::removePairedHost(uint64_t deskId) {
    bool removed;
    { std::lock_guard<std::mutex> lock(pairMutex_); removed = erasePeer(pairedHosts_, deskId); }
    if (removed) {
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            for (uint64_t* g : {&settings_.glideLeft, &settings_.glideRight, &settings_.glideTop, &settings_.glideBottom})
                if (*g == deskId) *g = 0;
        }
        save();
    }
    return removed;
}

void IdentityManager::setUnattendedEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        unattendedEnabled_ = enabled;
        if (!enabled) {
            unattendedVerifier_.clear();
        }
    }
    save();
}

void IdentityManager::setUnattendedPassword(const std::string& newPassword) {
    if (newPassword.empty()) return;
    std::string verifier = CryptoUtils::hashPassword(newPassword);
    if (verifier.empty()) return;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        unattendedPassword_ = newPassword;
        unattendedVerifier_ = std::move(verifier);
    }
    save();
}

std::string IdentityManager::regenerateSessionCode() {
    std::string code = generateRandomSixCharCode();
    std::array<uint8_t, 16> newSalt{};
    bool hasNewSalt = CryptoUtils::randomBytes(newSalt.data(), newSalt.size());
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    sessionCode_ = code;
    if (hasNewSalt) {
        sessionSalt_ = newSalt;
    }
    return sessionCode_;
}

void IdentityManager::getAuthKdfParams(std::array<uint8_t, 16>& outSalt, uint32_t& outIterations) const {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (unattendedEnabled_ && !unattendedVerifier_.empty()) {
        uint32_t vIter = 0;
        std::array<uint8_t, 16> vSalt{};
        std::array<uint8_t, 32> vDk{};
        if (CryptoUtils::parsePasswordVerifier(unattendedVerifier_, vIter, vSalt, vDk)) {
            CryptoUtils::secureZero(vDk.data(), vDk.size());
            outSalt = vSalt;
            outIterations = vIter;
            return;
        }
    }
    outSalt = sessionSalt_;
    outIterations = sessionIterations_;
}

bool IdentityManager::verifyChallengeResponse(
    uint64_t clientId,
    const std::array<uint8_t, 32>& nonce,
    const std::array<uint8_t, 32>& clientDigest,
    const std::array<uint8_t, 32>& boundSessionKey,
    const std::array<uint8_t, 16>& challengeSalt,
    uint32_t challengeIterations) const
{
    if (challengeIterations < CryptoUtils::kMinPbkdf2Iterations ||
        challengeIterations > CryptoUtils::kMaxPbkdf2Iterations) {
        return false;
    }

    uint64_t deskId = 0;
    std::string sessionCode;
    bool unattendedEnabled = false;
    std::string unattendedVerifier;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        deskId = deskId_;
        sessionCode = sessionCode_;
        unattendedEnabled = unattendedEnabled_;
        unattendedVerifier = unattendedVerifier_;
    }

    // 1. Check against active One-Time Session Code
    if (!sessionCode.empty()) {
        auto expectedCode = CryptoUtils::computeChallengeResponse(
            sessionCode, challengeSalt, challengeIterations, deskId, clientId, nonce
        );
        expectedCode = CryptoUtils::bindChallengeResponse(boundSessionKey, expectedCode);
        if (constantTimeEquals32(expectedCode, clientDigest)) {
            return true;
        }
    }

    // 2. Check against stored unattended password verifier (only when unattended access is enabled)
    if (unattendedEnabled && !unattendedVerifier.empty()) {
        uint32_t vIter = 0;
        std::array<uint8_t, 16> vSalt{};
        std::array<uint8_t, 32> vDk{};
        if (CryptoUtils::parsePasswordVerifier(unattendedVerifier, vIter, vSalt, vDk) &&
            vIter == challengeIterations && vSalt == challengeSalt) {
            std::string dkHex = CryptoUtils::toHex(vDk.data(), vDk.size());
            auto expected = CryptoUtils::computeChallengeResponseFromToken(
                dkHex, deskId, clientId, nonce
            );
            CryptoUtils::secureZero(dkHex.data(), dkHex.size());
            CryptoUtils::secureZero(vDk.data(), vDk.size());
            expected = CryptoUtils::bindChallengeResponse(boundSessionKey, expected);
            if (constantTimeEquals32(expected, clientDigest)) {
                return true;
            }
        } else {
            CryptoUtils::secureZero(vDk.data(), vDk.size());
        }
    }

    return false;
}

void IdentityManager::setRelayServerAddress(const std::string& addr) {
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        relayServerAddr_ = addr;
        settings_.relayServer = addr;
    }
    save();
}

void IdentityManager::addOrUpdateRecentSession(uint64_t id, const std::string& host, const std::string& addr,
                                             const std::string& alias, const std::string& tag, const std::string& notes) {
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        bool wasFav = false;
        std::string existingAlias = alias;
        std::string existingTag = tag;
        std::string existingNotes = notes;

        for (const auto& e : recentSessions_) {
            bool match = (id > 0 && e.deskId == id) || (!addr.empty() && e.address == addr);
            if (match) {
                if (e.isFavorite) wasFav = true;
                if (existingAlias.empty()) existingAlias = e.alias;
                if (existingTag.empty()) existingTag = e.tag;
                if (existingNotes.empty()) existingNotes = e.notes;
                break;
            }
        }

        recentSessions_.erase(
            std::remove_if(recentSessions_.begin(), recentSessions_.end(), [&](const RecentSessionEntry& e) {
                return (id > 0 && e.deskId == id) || (!addr.empty() && e.address == addr);
            }),
            recentSessions_.end()
        );
        RecentSessionEntry entry;
        entry.deskId = id;
        entry.hostname = host.empty() ? CryptoUtils::formatDeskId(id) : host;
        entry.address = addr;
        entry.isFavorite = wasFav;
        entry.alias = existingAlias;
        entry.tag = existingTag;
        entry.notes = existingNotes;
        recentSessions_.insert(recentSessions_.begin(), entry);

        std::stable_sort(recentSessions_.begin(), recentSessions_.end(), [](const RecentSessionEntry& a, const RecentSessionEntry& b) {
            return a.isFavorite && !b.isFavorite;
        });

        if (recentSessions_.size() > 20) {
            recentSessions_.resize(20);
        }
    }
    save();
}

void IdentityManager::updateRecentSessionMetadata(uint64_t id, const std::string& alias, const std::string& tag, const std::string& notes) {
    bool found = false;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        for (auto& e : recentSessions_) {
            if (e.deskId == id) {
                e.alias = alias;
                e.tag = tag;
                e.notes = notes;
                found = true;
                break;
            }
        }
    }
    if (found) save();
}

void IdentityManager::toggleFavoriteSession(uint64_t id) {
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        for (auto& e : recentSessions_) {
            if (e.deskId == id) {
                e.isFavorite = !e.isFavorite;
                break;
            }
        }
        std::stable_sort(recentSessions_.begin(), recentSessions_.end(), [](const RecentSessionEntry& a, const RecentSessionEntry& b) {
            return a.isFavorite && !b.isFavorite;
        });
    }
    save();
}

void IdentityManager::removeRecentSession(uint64_t id) {
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        recentSessions_.erase(
            std::remove_if(recentSessions_.begin(), recentSessions_.end(), [&](const RecentSessionEntry& e) {
                return e.deskId == id;
            }),
            recentSessions_.end()
        );
    }
    save();
}

void IdentityManager::clearRecentSessions() {
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        recentSessions_.clear();
    }
    save();
}

void IdentityManager::updateSettings(const AppSettings& newSettings) {
    std::string totpSecretCopy;
    TotpAlgorithm totpAlg = TotpAlgorithm::Sha1;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        settings_ = newSettings;
        settings_.targetFps = clampTargetFps(settings_.targetFps);
        settings_.defaultPermissions &= PERM_ALL;
        relayServerAddr_ = settings_.relayServer;
        totpSecretCopy = settings_.totpSecret;
        totpAlg = (settings_.totpAlgorithm == "SHA256") ? TotpAlgorithm::Sha256 : TotpAlgorithm::Sha1;
    }
    if (!totpSecretCopy.empty()) {
        std::vector<uint8_t> decoded;
        std::string normalized = TotpManager::decodeBase32(totpSecretCopy, decoded)
            ? TotpManager::encodeBase32(decoded.data(), decoded.size())
            : "";
        if (!decoded.empty()) CryptoUtils::secureZero(decoded.data(), decoded.size());
        if (normalized != totpManager_.getSecretBase32() || totpAlg != totpManager_.getAlgorithm()) {
            totpManager_.setSecret(totpSecretCopy, totpAlg);
        }
    } else if (totpManager_.isConfigured()) {
        totpManager_.clear();
    }
    save();
}

void IdentityManager::resetSettingsToDefault() {
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        settings_ = AppSettings{};
        relayServerAddr_ = settings_.relayServer;
    }
    totpManager_.clear();
    save();
}

// ---------------- AntiReplayWindow (Option 4A) ----------------

bool AntiReplayWindow::check(uint64_t seq) const {
    if (seq == 0 && maxSeq_ == 0 && bitmap_ == 0) {
        return true;
    }
    if (seq > maxSeq_) {
        return true;
    }
    uint64_t diff = maxSeq_ - seq;
    if (diff >= 64) {
        return false;
    }
    if (bitmap_ & (1ULL << diff)) {
        return false;
    }
    return true;
}

void AntiReplayWindow::mark(uint64_t seq) {
    if (seq == 0 && maxSeq_ == 0 && bitmap_ == 0) {
        bitmap_ = 1ULL;
        return;
    }
    if (seq > maxSeq_) {
        uint64_t diff = seq - maxSeq_;
        if (diff < 64) {
            bitmap_ <<= diff;
            bitmap_ |= 1ULL;
        } else {
            bitmap_ = 1ULL;
        }
        maxSeq_ = seq;
        return;
    }
    uint64_t diff = maxSeq_ - seq;
    if (diff < 64) {
        bitmap_ |= (1ULL << diff);
    }
}

bool AntiReplayWindow::checkAndMark(uint64_t seq) {
    if (!check(seq)) return false;
    mark(seq);
    return true;
}

void AntiReplayWindow::reset() {
    maxSeq_ = 0;
    bitmap_ = 0;
}

// ---------------- EcdhKeyExchange (Option 1A) ----------------

EcdhKeyExchange::EcdhKeyExchange() = default;

EcdhKeyExchange::~EcdhKeyExchange() {
    reset();
}

PairingKey::~PairingKey() {
    CryptoUtils::secureZero(keyPair_.data(), keyPair_.size());
}

std::array<uint8_t, 32> CryptoUtils::pairingTranscript(
    char role,
    uint64_t hostId,
    uint64_t viewerId,
    const std::array<uint8_t, 32>& nonce,
    const std::vector<uint8_t>& hostEcdhPub,
    const std::vector<uint8_t>& viewerEcdhPub)
{
    std::vector<uint8_t> buf;
    auto putU = [&buf](uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto putLenAndBytes = [&](const std::vector<uint8_t>& b) {
        putU(b.size(), 2);
        buf.insert(buf.end(), b.begin(), b.end());
    };
    static const char kLabel[] = "CPPDESK-PAIR-V1";
    buf.insert(buf.end(), kLabel, kLabel + sizeof(kLabel) - 1);
    buf.push_back(static_cast<uint8_t>(role));
    putU(hostId, 8);
    putU(viewerId, 8);
    buf.insert(buf.end(), nonce.begin(), nonce.end());
    putLenAndBytes(hostEcdhPub);
    putLenAndBytes(viewerEcdhPub);
    return sha256(buf.data(), buf.size());
}

#if defined(_WIN32)
bool EcdhKeyExchange::initialize() {
    reset();

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0);
    if (status < 0) return false;
    hAlg_ = reinterpret_cast<uintptr_t>(hAlg);

    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptGenerateKeyPair(hAlg, &hKey, 256, 0);
    if (status < 0) {
        reset();
        return false;
    }
    status = BCryptFinalizeKeyPair(hKey, 0);
    if (status < 0) {
        reset();
        return false;
    }
    hKey_ = reinterpret_cast<uintptr_t>(hKey);

    ULONG pubLen = 0;
    BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &pubLen, 0);
    if (pubLen == 0) {
        reset();
        return false;
    }
    localPubBlob_.resize(pubLen);
    status = BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, localPubBlob_.data(), pubLen, &pubLen, 0);
    if (status < 0) {
        reset();
        return false;
    }
    localPubBlob_.resize(pubLen);
    initialized_ = true;
    return true;
}

bool EcdhKeyExchange::initializeForTest(const std::vector<uint8_t>& privateBlob) {
    reset();
    if (privateBlob.size() != sizeof(BCRYPT_ECCKEY_BLOB) + 3 * 32) return false;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0);
    if (status < 0) return false;
    hAlg_ = reinterpret_cast<uintptr_t>(hAlg);

    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptImportKeyPair(hAlg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &hKey, const_cast<PUCHAR>(privateBlob.data()),
                                 static_cast<ULONG>(privateBlob.size()), 0);
    if (status < 0) {
        reset();
        return false;
    }
    hKey_ = reinterpret_cast<uintptr_t>(hKey);

    ULONG pubLen = 0;
    BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &pubLen, 0);
    if (pubLen == 0) {
        reset();
        return false;
    }
    localPubBlob_.resize(pubLen);
    status = BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, localPubBlob_.data(), pubLen, &pubLen, 0);
    if (status < 0) {
        reset();
        return false;
    }
    localPubBlob_.resize(pubLen);
    initialized_ = true;
    return true;
}

bool EcdhKeyExchange::computeSharedSessionKey(
    const uint8_t* peerPubBlob,
    size_t peerPubLen,
    uint64_t hostId,
    uint64_t viewerId,
    const std::array<uint8_t, 32>& nonce,
    std::array<uint8_t, 32>& outSessionKey)
{
    outSessionKey.fill(0);
    if (!initialized_ || !peerPubBlob || peerPubLen == 0 || !hAlg_ || !hKey_) return false;

    BCRYPT_ALG_HANDLE hAlg = reinterpret_cast<BCRYPT_ALG_HANDLE>(hAlg_);
    BCRYPT_KEY_HANDLE hKey = reinterpret_cast<BCRYPT_KEY_HANDLE>(hKey_);

    BCRYPT_KEY_HANDLE hPeerKey = nullptr;
    NTSTATUS status = BCryptImportKeyPair(
        hAlg,
        nullptr,
        BCRYPT_ECCPUBLIC_BLOB,
        &hPeerKey,
        const_cast<PUCHAR>(peerPubBlob),
        static_cast<ULONG>(peerPubLen),
        0
    );
    if (status < 0) return false;

    BCRYPT_SECRET_HANDLE hSecret = nullptr;
    status = BCryptSecretAgreement(hKey, hPeerKey, &hSecret, 0);
    BCryptDestroyKey(hPeerKey);
    if (status < 0) return false;

    ULONG derivedLen = 0;
    BCryptDeriveKey(hSecret, BCRYPT_KDF_RAW_SECRET, nullptr, nullptr, 0, &derivedLen, 0);
    if (derivedLen == 0) {
        BCryptDestroySecret(hSecret);
        return false;
    }

    std::vector<uint8_t> secretBytes(derivedLen);
    status = BCryptDeriveKey(hSecret, BCRYPT_KDF_RAW_SECRET, nullptr, secretBytes.data(), derivedLen, &derivedLen, 0);
    BCryptDestroySecret(hSecret);
    if (status < 0) {
        CryptoUtils::secureZero(secretBytes.data(), secretBytes.size());
        return false;
    }
    secretBytes.resize(derivedLen);

    outSessionKey = CryptoUtils::deriveSessionKey(hostId, viewerId, nonce, secretBytes.data(), secretBytes.size());
    CryptoUtils::secureZero(secretBytes.data(), secretBytes.size());
    return true;
}

void EcdhKeyExchange::reset() {
    if (hKey_) {
        BCryptDestroyKey(reinterpret_cast<BCRYPT_KEY_HANDLE>(hKey_));
        hKey_ = 0;
    }
    if (hAlg_) {
        BCryptCloseAlgorithmProvider(reinterpret_cast<BCRYPT_ALG_HANDLE>(hAlg_), 0);
        hAlg_ = 0;
    }
    CryptoUtils::secureZero(localPubBlob_.data(), localPubBlob_.size());
    localPubBlob_.clear();
    initialized_ = false;
}

// ---------------- PairingKey (Windows CNG) ----------------

namespace {
// An ECDSA P-256 key handle from X || Y (public) or X || Y || d (private); null if CNG rejects it.
BCRYPT_KEY_HANDLE importEcdsaKey(BCRYPT_ALG_HANDLE hAlg, const uint8_t* keyBytes, bool priv) {
    const size_t bodyLen = priv ? 96 : 64;
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 96> blob{};
    BCRYPT_ECCKEY_BLOB header{priv ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), keyBytes, bodyLen);
    BCRYPT_KEY_HANDLE hKey = nullptr;
    NTSTATUS status = BCryptImportKeyPair(hAlg, nullptr, priv ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB, &hKey,
                                          blob.data(), static_cast<ULONG>(sizeof(header) + bodyLen), 0);
    CryptoUtils::secureZero(blob.data(), blob.size());
    return status >= 0 ? hKey : nullptr;
}
}

bool PairingKey::generate() {
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 96> blob{};
    ULONG blobLen = 0;
    bool ok = false;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) >= 0) {
        BCRYPT_KEY_HANDLE hKey = nullptr;
        ok = BCryptGenerateKeyPair(hAlg, &hKey, 256, 0) >= 0 && BCryptFinalizeKeyPair(hKey, 0) >= 0 &&
             BCryptExportKey(hKey, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), static_cast<ULONG>(blob.size()),
                             &blobLen, 0) >= 0 && blobLen == blob.size();
        if (hKey) BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    KeyPair kp{};
    if (ok) std::memcpy(kp.data(), blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), kp.size());
    CryptoUtils::secureZero(blob.data(), blob.size());
    ok = ok && importKeyPair(kp);
    CryptoUtils::secureZero(kp.data(), kp.size());
    return ok;
}

bool PairingKey::importKeyPair(const KeyPair& kp) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) return false;
    BCRYPT_KEY_HANDLE hKey = importEcdsaKey(hAlg, kp.data(), true);
    if (hKey) BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!hKey) return false;
    keyPair_ = kp;
    std::copy(kp.begin(), kp.begin() + pub_.size(), pub_.begin());
    valid_ = true;
    return true;
}

bool PairingKey::sign(const std::array<uint8_t, 32>& digest, Signature& out) const {
    if (!valid_) return false;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) return false;
    bool ok = false;
    if (BCRYPT_KEY_HANDLE hKey = importEcdsaKey(hAlg, keyPair_.data(), true)) {
        ULONG len = 0;
        ok = BCryptSignHash(hKey, nullptr, const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                            out.data(), static_cast<ULONG>(out.size()), &len, 0) >= 0 && len == out.size();
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool PairingKey::verify(const PublicKey& pub, const std::array<uint8_t, 32>& digest, const Signature& sig) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) return false;
    bool ok = false;
    if (BCRYPT_KEY_HANDLE hKey = importEcdsaKey(hAlg, pub.data(), false)) {
        ok = BCryptVerifySignature(hKey, nullptr, const_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
                                   const_cast<PUCHAR>(sig.data()), static_cast<ULONG>(sig.size()), 0) >= 0;
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// ---------------- AesGcmSessionCipher (Option 1A & 4A) ----------------

AesGcmSessionCipher::AesGcmSessionCipher() = default;

AesGcmSessionCipher::~AesGcmSessionCipher() {
    reset();
}

bool AesGcmSessionCipher::initialize(const std::array<uint8_t, 32>& sessionKey, bool isHost) {
    reset();
    isHost_ = isHost;
    sessionKey_ = sessionKey;
    CryptoUtils::pinMemory(sessionKey_.data(), sessionKey_.size());

    BCRYPT_ALG_HANDLE hAesAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAesAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (status < 0) {
        reset();
        return false;
    }
    hAesAlg_ = reinterpret_cast<uintptr_t>(hAesAlg);

    status = BCryptSetProperty(
        hAesAlg,
        BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM),
        0
    );
    if (status < 0) {
        reset();
        return false;
    }

    DWORD keyObjSize = 0;
    DWORD cbData = 0;
    status = BCryptGetProperty(hAesAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&keyObjSize), sizeof(keyObjSize), &cbData, 0);
    if (status < 0) {
        reset();
        return false;
    }
    if (keyObjSize > 0) {
        keyObjBuf_.resize(keyObjSize);
        decryptKeyObjBuf_.resize(keyObjSize);
    }

    BCRYPT_KEY_HANDLE hAesKey = nullptr;
    status = BCryptGenerateSymmetricKey(
        hAesAlg,
        &hAesKey,
        keyObjBuf_.empty() ? nullptr : keyObjBuf_.data(),
        static_cast<ULONG>(keyObjBuf_.size()),
        const_cast<PUCHAR>(sessionKey_.data()),
        static_cast<ULONG>(sessionKey_.size()),
        0
    );
    if (status < 0) {
        reset();
        return false;
    }
    hAesKey_ = reinterpret_cast<uintptr_t>(hAesKey);

    BCRYPT_KEY_HANDLE hAesDecryptKey = nullptr;
    status = BCryptGenerateSymmetricKey(
        hAesAlg,
        &hAesDecryptKey,
        decryptKeyObjBuf_.empty() ? nullptr : decryptKeyObjBuf_.data(),
        static_cast<ULONG>(decryptKeyObjBuf_.size()),
        const_cast<PUCHAR>(sessionKey_.data()),
        static_cast<ULONG>(sessionKey_.size()),
        0
    );
    if (status < 0) {
        reset();
        return false;
    }
    hAesDecryptKey_ = reinterpret_cast<uintptr_t>(hAesDecryptKey);

    replayWindow_.reset();
    initialized_ = true;
    return true;
}

void AesGcmSessionCipher::reset() {
    if (hAesDecryptKey_) {
        BCryptDestroyKey(reinterpret_cast<BCRYPT_KEY_HANDLE>(hAesDecryptKey_));
        hAesDecryptKey_ = 0;
    }
    if (hAesKey_) {
        BCryptDestroyKey(reinterpret_cast<BCRYPT_KEY_HANDLE>(hAesKey_));
        hAesKey_ = 0;
    }
    if (hAesAlg_) {
        BCryptCloseAlgorithmProvider(reinterpret_cast<BCRYPT_ALG_HANDLE>(hAesAlg_), 0);
        hAesAlg_ = 0;
    }
    CryptoUtils::unpinMemory(sessionKey_.data(), sessionKey_.size());
    CryptoUtils::secureZero(sessionKey_.data(), sessionKey_.size());
    CryptoUtils::secureZero(keyObjBuf_.data(), keyObjBuf_.size());
    keyObjBuf_.clear();
    CryptoUtils::secureZero(decryptKeyObjBuf_.data(), decryptKeyObjBuf_.size());
    decryptKeyObjBuf_.clear();
    replayWindow_.reset();
    initialized_ = false;
}

bool AesGcmSessionCipher::encrypt(
    const void* plaintext,
    size_t plainLen,
    uint64_t seq,
    const void* aad,
    size_t aadLen,
    std::vector<uint8_t>& outEncryptedPayload)
{
    if (!initialized_ || !hAesKey_) return false;
    if (plainLen > 0 && !plaintext) return false;
    if (aadLen > 0 && !aad) return false;
    if (plainLen > static_cast<size_t>(INT_MAX) || aadLen > static_cast<size_t>(INT_MAX)) return false;
    BCRYPT_KEY_HANDLE hKey = reinterpret_cast<BCRYPT_KEY_HANDLE>(hAesKey_);

    uint8_t nonce[12] = {};
    if (isHost_) {
        nonce[0] = 'H'; nonce[1] = 'S'; nonce[2] = 'T'; nonce[3] = '1';
    } else {
        nonce[0] = 'V'; nonce[1] = 'I'; nonce[2] = 'W'; nonce[3] = '1';
    }
    std::memcpy(nonce + 4, &seq, 8);

    uint8_t tag[16] = {};
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = sizeof(nonce);
    authInfo.pbTag = tag;
    authInfo.cbTag = sizeof(tag);
    if (aad && aadLen > 0) {
        authInfo.pbAuthData = const_cast<PUCHAR>(static_cast<const uint8_t*>(aad));
        authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    }

    outEncryptedPayload.resize(12 + plainLen + 16);
    std::memcpy(outEncryptedPayload.data(), nonce, 12);

    ULONG ctLen = 0;
    NTSTATUS status = BCryptEncrypt(
        hKey,
        plainLen > 0 ? const_cast<PUCHAR>(static_cast<const uint8_t*>(plaintext)) : nullptr,
        static_cast<ULONG>(plainLen),
        &authInfo,
        nullptr, 0,
        plainLen > 0 ? outEncryptedPayload.data() + 12 : nullptr,
        static_cast<ULONG>(plainLen),
        &ctLen,
        0
    );

    if (status < 0) {
        outEncryptedPayload.clear();
        return false;
    }

    std::memcpy(outEncryptedPayload.data() + 12 + plainLen, tag, 16);
    return true;
}

bool AesGcmSessionCipher::decrypt(
    const uint8_t* encryptedPayload,
    size_t payloadLen,
    const void* aad,
    size_t aadLen,
    std::vector<uint8_t>& outPlaintext,
    uint64_t* outSeq)
{
    if (!initialized_ || !hAesDecryptKey_ || !encryptedPayload || payloadLen < 28) return false;
    if (aadLen > 0 && !aad) return false;
    if (payloadLen > static_cast<size_t>(INT_MAX) || aadLen > static_cast<size_t>(INT_MAX)) return false;
    BCRYPT_KEY_HANDLE hKey = reinterpret_cast<BCRYPT_KEY_HANDLE>(hAesDecryptKey_);

    const uint8_t* nonce = encryptedPayload;
    const uint8_t* ciphertext = encryptedPayload + 12;
    size_t cipherLen = payloadLen - 28;
    const uint8_t* tag = encryptedPayload + 12 + cipherLen;

    const char* expect = isHost_ ? "VIW1" : "HST1";
    if (std::memcmp(nonce, expect, 4) != 0) return false;

    uint64_t seq = 0;
    std::memcpy(&seq, nonce + 4, 8);

    if (!replayWindow_.check(seq)) {
        return false; // Replay / duplicate rejected
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(nonce);
    authInfo.cbNonce = 12;
    authInfo.pbTag = const_cast<PUCHAR>(tag);
    authInfo.cbTag = 16;
    if (aad && aadLen > 0) {
        authInfo.pbAuthData = const_cast<PUCHAR>(static_cast<const uint8_t*>(aad));
        authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    }

    outPlaintext.resize(cipherLen);
    ULONG ptLen = 0;
    NTSTATUS status = BCryptDecrypt(
        hKey,
        cipherLen > 0 ? const_cast<PUCHAR>(ciphertext) : nullptr,
        static_cast<ULONG>(cipherLen),
        &authInfo,
        nullptr, 0,
        cipherLen > 0 ? outPlaintext.data() : nullptr,
        static_cast<ULONG>(cipherLen),
        &ptLen,
        0
    );

    if (status < 0) {
        outPlaintext.clear();
        return false; // Auth tag mismatch or decryption failure
    }

    // Sequence mutation occurs strictly AFTER successful cryptographic authentication
    replayWindow_.mark(seq);

    outPlaintext.resize(ptLen);
    if (outSeq) *outSeq = seq;
    return true;
}

#else

// ---------------- EcdhKeyExchange (OpenSSL: Linux and other POSIX) ----------------
// Same wire format as the Windows CNG path: an ECCPUBLIC_BLOB of {magic 'ECK1', 32, X, Y} for NIST P-256.

namespace {
constexpr uint32_t kEccPublicP256Magic = 0x314B4345; // BCRYPT_ECDH_PUBLIC_P256_MAGIC
constexpr uint32_t kEccPrivateP256Magic = 0x324B4345; // BCRYPT_ECDH_PRIVATE_P256_MAGIC
constexpr size_t kEccBlobLen = 8 + 32 + 32;
}

bool EcdhKeyExchange::initialize() {
    reset();

    EVP_PKEY* key = EVP_EC_gen("P-256");
    if (!key) return false;

    // The encoded public key is the uncompressed point on every OpenSSL 3.x. The "pub" parameter is not:
    // OpenSSL 3.0.2 (Ubuntu 22.04) returns it compressed (33 bytes).
    uint8_t point[65];
    unsigned char* encoded = nullptr;
    const size_t pointLen = EVP_PKEY_get1_encoded_public_key(key, &encoded);
    const bool ok = encoded && pointLen == sizeof(point) && encoded[0] == 0x04;
    if (ok) std::memcpy(point, encoded, sizeof(point));
    OPENSSL_free(encoded);
    if (!ok) {
        EVP_PKEY_free(key);
        return false;
    }

    localPubBlob_.resize(kEccBlobLen);
    const uint32_t cbKey = 32;
    std::memcpy(localPubBlob_.data(), &kEccPublicP256Magic, 4);
    std::memcpy(localPubBlob_.data() + 4, &cbKey, 4);
    std::memcpy(localPubBlob_.data() + 8, point + 1, 64);

    hKey_ = reinterpret_cast<uintptr_t>(key);
    initialized_ = true;
    return true;
}

bool EcdhKeyExchange::initializeForTest(const std::vector<uint8_t>& privateBlob) {
    reset();
    if (privateBlob.size() != kEccBlobLen + 32) return false;
    uint32_t magic = 0, cbKey = 0;
    std::memcpy(&magic, privateBlob.data(), 4);
    std::memcpy(&cbKey, privateBlob.data() + 4, 4);
    if (magic != kEccPrivateP256Magic || cbKey != 32) return false;

    uint8_t point[65];
    point[0] = 0x04;
    std::memcpy(point + 1, privateBlob.data() + 8, 64);
    BIGNUM* d = BN_bin2bn(privateBlob.data() + kEccBlobLen, 32, nullptr);

    EVP_PKEY* key = nullptr;
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM* params = nullptr;
    EVP_PKEY_CTX* ctx = nullptr;
    bool ok = false;
    do {
        if (!d || !bld ||
            OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "P-256", 0) != 1 ||
            OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)) != 1 ||
            OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, d) != 1) break;
        params = OSSL_PARAM_BLD_to_param(bld);
        ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        if (!params || !ctx || EVP_PKEY_fromdata_init(ctx) != 1 ||
            EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_KEYPAIR, params) != 1) break;
        ok = true;
    } while (false);
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    BN_clear_free(d);
    if (!ok) {
        EVP_PKEY_free(key);
        return false;
    }

    localPubBlob_.resize(kEccBlobLen);
    std::memcpy(localPubBlob_.data(), &kEccPublicP256Magic, 4);
    std::memcpy(localPubBlob_.data() + 4, &cbKey, 4);
    std::memcpy(localPubBlob_.data() + 8, point + 1, 64);
    hKey_ = reinterpret_cast<uintptr_t>(key);
    initialized_ = true;
    return true;
}

bool EcdhKeyExchange::computeSharedSessionKey(
    const uint8_t* peerPubBlob,
    size_t peerPubLen,
    uint64_t hostId,
    uint64_t viewerId,
    const std::array<uint8_t, 32>& nonce,
    std::array<uint8_t, 32>& outSessionKey)
{
    outSessionKey.fill(0);
    if (!initialized_ || !hKey_ || !peerPubBlob || peerPubLen != kEccBlobLen) return false;

    uint32_t magic = 0, cbKey = 0;
    std::memcpy(&magic, peerPubBlob, 4);
    std::memcpy(&cbKey, peerPubBlob + 4, 4);
    if (magic != kEccPublicP256Magic || cbKey != 32) return false;

    uint8_t point[65];
    point[0] = 0x04;
    std::memcpy(point + 1, peerPubBlob + 8, 64);

    EVP_PKEY* local = reinterpret_cast<EVP_PKEY*>(hKey_);
    EVP_PKEY* peer = nullptr;
    bool ok = false;

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM* params = nullptr;
    EVP_PKEY_CTX* importCtx = nullptr;
    EVP_PKEY_CTX* checkCtx = nullptr;
    EVP_PKEY_CTX* deriveCtx = nullptr;
    std::array<uint8_t, 32> secret{};
    size_t secretLen = secret.size();

    do {
        if (!bld ||
            OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "P-256", 0) != 1 ||
            OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)) != 1) break;
        params = OSSL_PARAM_BLD_to_param(bld);
        importCtx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        if (!params || !importCtx || EVP_PKEY_fromdata_init(importCtx) != 1 ||
            EVP_PKEY_fromdata(importCtx, &peer, EVP_PKEY_PUBLIC_KEY, params) != 1) break;

        // Reject a point that is not on the curve.
        checkCtx = EVP_PKEY_CTX_new(peer, nullptr);
        if (!checkCtx || EVP_PKEY_public_check(checkCtx) != 1) break;

        deriveCtx = EVP_PKEY_CTX_new(local, nullptr);
        if (!deriveCtx || EVP_PKEY_derive_init(deriveCtx) != 1 ||
            EVP_PKEY_derive_set_peer(deriveCtx, peer) != 1 ||
            EVP_PKEY_derive(deriveCtx, secret.data(), &secretLen) != 1 || secretLen != secret.size()) break;
        ok = true;
    } while (false);

    EVP_PKEY_CTX_free(deriveCtx);
    EVP_PKEY_CTX_free(checkCtx);
    EVP_PKEY_CTX_free(importCtx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    EVP_PKEY_free(peer);
    if (!ok) {
        CryptoUtils::secureZero(secret.data(), secret.size());
        return false;
    }

    // CNG hands back the raw secret least-significant byte first; OpenSSL most-significant first.
    std::reverse(secret.begin(), secret.end());
    outSessionKey = CryptoUtils::deriveSessionKey(hostId, viewerId, nonce, secret.data(), secret.size());
    CryptoUtils::secureZero(secret.data(), secret.size());
    return true;
}

void EcdhKeyExchange::reset() {
    if (hKey_) {
        EVP_PKEY_free(reinterpret_cast<EVP_PKEY*>(hKey_));
        hKey_ = 0;
    }
    initialized_ = false;
    localPubBlob_.clear();
}

// ---------------- PairingKey (OpenSSL: Linux and other POSIX) ----------------

namespace {
// A P-256 EVP_PKEY from X || Y, plus the private scalar d when given. Null if OpenSSL rejects it.
EVP_PKEY* pairingEvpKey(const uint8_t* xy, const uint8_t* d) {
    uint8_t point[65];
    point[0] = 0x04;
    std::memcpy(point + 1, xy, 64);
    BIGNUM* bn = d ? BN_bin2bn(d, 32, nullptr) : nullptr;

    EVP_PKEY* key = nullptr;
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM* params = nullptr;
    EVP_PKEY_CTX* ctx = nullptr;
    bool ok = false;
    do {
        if ((d && !bn) || !bld ||
            OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "P-256", 0) != 1 ||
            OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)) != 1 ||
            (bn && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, bn) != 1)) break;
        params = OSSL_PARAM_BLD_to_param(bld);
        ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
        if (!params || !ctx || EVP_PKEY_fromdata_init(ctx) != 1 ||
            EVP_PKEY_fromdata(ctx, &key, d ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params) != 1) break;
        ok = true;
    } while (false);
    EVP_PKEY_CTX_free(ctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    BN_clear_free(bn);
    if (!ok) {
        EVP_PKEY_free(key);
        return nullptr;
    }
    return key;
}
}

bool PairingKey::generate() {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    if (!key) return false;

    // Uncompressed point on every OpenSSL 3.x; see EcdhKeyExchange::initialize.
    unsigned char* encoded = nullptr;
    const size_t pointLen = EVP_PKEY_get1_encoded_public_key(key, &encoded);
    BIGNUM* d = nullptr;
    KeyPair kp{};
    bool ok = encoded && pointLen == 65 && encoded[0] == 0x04 &&
              EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &d) == 1 && d &&
              BN_bn2binpad(d, kp.data() + 64, 32) == 32;
    if (ok) std::memcpy(kp.data(), encoded + 1, 64);
    OPENSSL_free(encoded);
    BN_clear_free(d);
    EVP_PKEY_free(key);
    ok = ok && importKeyPair(kp);
    CryptoUtils::secureZero(kp.data(), kp.size());
    return ok;
}

bool PairingKey::importKeyPair(const KeyPair& kp) {
    EVP_PKEY* key = pairingEvpKey(kp.data(), kp.data() + 64);
    if (!key) return false;
    EVP_PKEY_free(key);
    keyPair_ = kp;
    std::copy(kp.begin(), kp.begin() + pub_.size(), pub_.begin());
    valid_ = true;
    return true;
}

bool PairingKey::sign(const std::array<uint8_t, 32>& digest, Signature& out) const {
    if (!valid_) return false;
    EVP_PKEY* key = pairingEvpKey(keyPair_.data(), keyPair_.data() + 64);
    EVP_PKEY_CTX* ctx = key ? EVP_PKEY_CTX_new(key, nullptr) : nullptr;
    uint8_t der[80]; // an ECDSA P-256 signature is at most 72 bytes of DER
    size_t derLen = sizeof(der);
    bool ok = ctx && EVP_PKEY_sign_init(ctx) == 1 && EVP_PKEY_sign(ctx, der, &derLen, digest.data(), digest.size()) == 1;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (!ok) return false;

    // DER SEQUENCE { r, s } -> fixed-width r || s
    const unsigned char* p = der;
    ECDSA_SIG* es = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(derLen));
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    if (es) ECDSA_SIG_get0(es, &r, &s);
    ok = es && BN_bn2binpad(r, out.data(), 32) == 32 && BN_bn2binpad(s, out.data() + 32, 32) == 32;
    ECDSA_SIG_free(es);
    return ok;
}

bool PairingKey::verify(const PublicKey& pub, const std::array<uint8_t, 32>& digest, const Signature& sig) {
    // r || s -> DER SEQUENCE { r, s }
    BIGNUM* r = BN_bin2bn(sig.data(), 32, nullptr);
    BIGNUM* s = BN_bin2bn(sig.data() + 32, 32, nullptr);
    ECDSA_SIG* es = ECDSA_SIG_new();
    if (es && r && s && ECDSA_SIG_set0(es, r, s) == 1) r = s = nullptr; // es owns them now
    BN_free(r);
    BN_free(s);
    unsigned char* der = nullptr;
    const int derLen = es ? i2d_ECDSA_SIG(es, &der) : 0;
    ECDSA_SIG_free(es);

    EVP_PKEY* key = derLen > 0 ? pairingEvpKey(pub.data(), nullptr) : nullptr;
    EVP_PKEY_CTX* ctx = key ? EVP_PKEY_CTX_new(key, nullptr) : nullptr;
    // Reject a point that is not on the curve, as EcdhKeyExchange does for the peer key.
    const bool ok = ctx && EVP_PKEY_public_check(ctx) == 1 && EVP_PKEY_verify_init(ctx) == 1 &&
                    EVP_PKEY_verify(ctx, der, static_cast<size_t>(derLen), digest.data(), digest.size()) == 1;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(key);
    OPENSSL_free(der);
    return ok;
}

// ---------------- AesGcmSessionCipher (OpenSSL: Linux and other POSIX) ----------------
// AES-256-GCM, byte-for-byte the same payload layout as the Windows CNG path:
// [12-byte nonce: "HST1"/"VIW1" + 8-byte sequence][ciphertext][16-byte tag].

AesGcmSessionCipher::AesGcmSessionCipher() = default;

AesGcmSessionCipher::~AesGcmSessionCipher() {
    reset();
}

bool AesGcmSessionCipher::initialize(const std::array<uint8_t, 32>& sessionKey, bool isHost) {
    reset();
    sessionKey_ = sessionKey;
    isHost_ = isHost;
    CryptoUtils::pinMemory(sessionKey_.data(), sessionKey_.size());
    replayWindow_.reset();
    initialized_ = true;
    return true;
}

void AesGcmSessionCipher::reset() {
    CryptoUtils::unpinMemory(sessionKey_.data(), sessionKey_.size());
    CryptoUtils::secureZero(sessionKey_.data(), sessionKey_.size());
    replayWindow_.reset();
    initialized_ = false;
}

bool AesGcmSessionCipher::encrypt(
    const void* plaintext,
    size_t plainLen,
    uint64_t seq,
    const void* aad,
    size_t aadLen,
    std::vector<uint8_t>& outEncryptedPayload)
{
    if (!initialized_ || plainLen > static_cast<size_t>(INT_MAX) || aadLen > static_cast<size_t>(INT_MAX)) return false;
    if (plainLen > 0 && !plaintext) return false;
    if (aadLen > 0 && !aad) return false;

    uint8_t nonce[12] = {};
    std::memcpy(nonce, isHost_ ? "HST1" : "VIW1", 4);
    std::memcpy(nonce + 4, &seq, 8);

    outEncryptedPayload.resize(12 + plainLen + 16);
    std::memcpy(outEncryptedPayload.data(), nonce, 12);
    uint8_t* ct = outEncryptedPayload.data() + 12;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    bool ok = ctx &&
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, sessionKey_.data(), nonce) == 1 &&
        (!aad || aadLen == 0 || EVP_EncryptUpdate(ctx, nullptr, &len, static_cast<const uint8_t*>(aad), static_cast<int>(aadLen)) == 1) &&
        (plainLen == 0 || EVP_EncryptUpdate(ctx, ct, &len, static_cast<const uint8_t*>(plaintext), static_cast<int>(plainLen)) == 1) &&
        EVP_EncryptFinal_ex(ctx, ct + plainLen, &len) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, ct + plainLen) == 1;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) outEncryptedPayload.clear();
    return ok;
}

bool AesGcmSessionCipher::decrypt(
    const uint8_t* encryptedPayload,
    size_t payloadLen,
    const void* aad,
    size_t aadLen,
    std::vector<uint8_t>& outPlaintext,
    uint64_t* outSeq)
{
    if (!initialized_ || !encryptedPayload || payloadLen < 28 || payloadLen > static_cast<size_t>(INT_MAX) || aadLen > static_cast<size_t>(INT_MAX)) return false;
    if (aadLen > 0 && !aad) return false;

    const uint8_t* nonce = encryptedPayload;
    const size_t cipherLen = payloadLen - 28;
    const uint8_t* ct = encryptedPayload + 12;
    uint8_t tag[16];
    std::memcpy(tag, encryptedPayload + 12 + cipherLen, 16);

    if (std::memcmp(nonce, isHost_ ? "VIW1" : "HST1", 4) != 0) return false;

    uint64_t seq = 0;
    std::memcpy(&seq, nonce + 4, 8);
    if (!replayWindow_.check(seq)) return false;

    outPlaintext.resize(cipherLen);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    uint8_t finalBlock[16];
    bool ok = ctx &&
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, sessionKey_.data(), nonce) == 1 &&
        (!aad || aadLen == 0 || EVP_DecryptUpdate(ctx, nullptr, &len, static_cast<const uint8_t*>(aad), static_cast<int>(aadLen)) == 1) &&
        (cipherLen == 0 || EVP_DecryptUpdate(ctx, outPlaintext.data(), &len, ct, static_cast<int>(cipherLen)) == 1) &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1 &&
        EVP_DecryptFinal_ex(ctx, finalBlock, &len) == 1;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        outPlaintext.clear();
        return false;
    }

    // The sequence is recorded only after the tag has authenticated.
    replayWindow_.mark(seq);
    if (outSeq) *outSeq = seq;
    return true;
}

#endif

} // namespace cppdesk
