#include "crypto_identity.hpp"
#include "../simd/simd_kernels.hpp"
#include "protocol.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <unistd.h>
#endif

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <algorithm>

namespace cppdesk {

namespace {

std::string generateRandomSixCharCode() {
    const char* chars = "23456789abcdefghjkmnpqrstuvwxyz";
    uint8_t rnd[6] = {};
    CryptoUtils::randomBytes(rnd, sizeof(rnd));
    std::string code;
    code.reserve(6);
    for (int i = 0; i < 6; ++i) {
        code.push_back(chars[rnd[i] % 31]);
    }
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
    randomBytes(&rnd, sizeof(rnd));
    return 100000000ULL + (rnd % 900000000ULL);
}

std::array<uint8_t, 32> CryptoUtils::sha256(const void* data, size_t len) {
    std::array<uint8_t, 32> digest{};
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
        if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) >= 0) {
            if (len > 0 && data != nullptr) {
                BCryptHashData(
                    hHash,
                    const_cast<PUCHAR>(static_cast<const uint8_t*>(data)),
                    static_cast<ULONG>(len),
                    0
                );
            }
            BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0);
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
#elif defined(__APPLE__)
    CC_SHA256(data, static_cast<CC_LONG>(len), digest.data());
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

std::string CryptoUtils::hashPassword(const std::string& password, const std::string& salt) {
    std::string combined = "CppDesk-v1:" + salt + ":" + password;
    return sha256Hex(combined);
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
    uint64_t hostId,
    uint64_t clientId,
    const std::array<uint8_t, 32>& nonce)
{
    std::string passToken = hashPassword(passwordPlain, std::to_string(hostId));
    return computeChallengeResponseFromToken(passToken, hostId, clientId, nonce);
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
    if (status >= 0) {
        BCRYPT_HASH_HANDLE hHash = nullptr;
        status = BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                                  const_cast<PUCHAR>(static_cast<const uint8_t*>(key)),
                                  static_cast<ULONG>(keyLen), 0);
        if (status >= 0) {
            if (msg && msgLen > 0) {
                BCryptHashData(hHash,
                               const_cast<PUCHAR>(static_cast<const uint8_t*>(msg)),
                               static_cast<ULONG>(msgLen), 0);
            }
            BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0);
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    return digest;
#elif defined(__APPLE__)
    CCHmac(kCCHmacAlgSHA256, key, keyLen, msg, msgLen, digest.data());
    return digest;
#else
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

    ByteWriter w;
    w.writeString("CppDesk-SessionKey-v1");
    w.writeU64(hostId);
    w.writeU64(clientId);
    w.writeBytes(nonce.data(), nonce.size());
    return sha256(w.buffer().data(), w.buffer().size());
}

std::string CryptoUtils::sessionFingerprintHex(const std::array<uint8_t, 32>& sessionKey) {
    auto digest = sha256(sessionKey.data(), sessionKey.size());
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02X%02X-%02X%02X",
                  digest[0], digest[1], digest[2], digest[3]);
    return std::string(buf);
}

void CryptoUtils::transformPayload(
    uint8_t* data,
    size_t len,
    const std::array<uint8_t, 32>& sessionKey,
    uint64_t frameSequence)
{
    if (!data || len == 0) return;

    // Fast 64-bit SplitMix64 CTR stream cipher seeded from 256-bit sessionKey + frameSequence
    uint64_t k0 = 0, k1 = 0, k2 = 0, k3 = 0;
    std::memcpy(&k0, sessionKey.data() + 0, 8);
    std::memcpy(&k1, sessionKey.data() + 8, 8);
    std::memcpy(&k2, sessionKey.data() + 16, 8);
    std::memcpy(&k3, sessionKey.data() + 24, 8);

    uint64_t state = k0 ^ (k1 + 0x9E3779B97F4A7C15ULL) ^ (frameSequence * 0xBF58476D1CE4E5B9ULL);
    auto nextWord = [&]() -> uint64_t {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = (state ^ k2);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27) ^ k3) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };

    size_t i = 0;
    if (SimdKernels::hasAvx2() && len >= 32) {
        alignas(32) uint64_t ksBatch[32]; // Up to 8 x 32-byte (256-bit) AVX2 blocks per call
        while (i + 32 <= len) {
            size_t blocks = std::min<size_t>((len - i) / 32, 8);
            size_t words = blocks * 4;
            for (size_t w = 0; w < words; ++w) {
                ksBatch[w] = nextWord();
            }
            cppdesk_avx2_xor_blocks32(data + i, reinterpret_cast<const uint8_t*>(ksBatch), blocks);
            i += blocks * 32;
        }
    }

    while (i + 8 <= len) {
        uint64_t ks = nextWord();
        uint64_t chunk = 0;
        std::memcpy(&chunk, data + i, 8);
        chunk ^= ks;
        std::memcpy(data + i, &chunk, 8);
        i += 8;
    }
    if (i < len) {
        uint64_t ks = nextWord();
        const uint8_t* ksBytes = reinterpret_cast<const uint8_t*>(&ks);
        for (size_t j = 0; i < len; ++i, ++j) {
            data[i] ^= ksBytes[j];
        }
    }
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
    char appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        std::filesystem::path dir = std::filesystem::path(appData) / "CppDesk";
        std::filesystem::path oldDir = std::filesystem::path(appData) / "AeroDesk";
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

        configPath_ = (dir / ("config_" + std::to_string(instanceId_) + ".ini")).string();
    } else {
        std::filesystem::path oldLocal = "aerodesk_" + std::to_string(instanceId_) + ".ini";
        std::filesystem::path newLocal = "cppdesk_" + std::to_string(instanceId_) + ".ini";
        std::error_code ec;
        if (std::filesystem::exists(oldLocal, ec) && !std::filesystem::exists(newLocal, ec)) {
            std::filesystem::copy_file(oldLocal, newLocal, std::filesystem::copy_options::overwrite_existing, ec);
        }
        configPath_ = newLocal.string();
    }
#else
    const char* home = std::getenv("HOME");
    std::filesystem::path dir = home ? (std::filesystem::path(home) / ".cppdesk") : std::filesystem::path(".cppdesk");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    configPath_ = (dir / ("config_" + std::to_string(instanceId_) + ".ini")).string();
#endif
}

bool IdentityManager::loadOrCreate() {
    std::string legacyPlainPassword;
    std::ifstream in(configPath_);
    if (in.is_open()) {
        std::string line;
        recentSessions_.clear();
        while (std::getline(in, line)) {
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
                if (val.size() == 64) unattendedVerifier_ = val;
            } else if (key == "unattended_password") {
                legacyPlainPassword = val;
            } else if (key == "relay_server") {
                if (!val.empty()) {
                    relayServerAddr_ = val;
                    settings_.relayServer = val;
                }
            } else if (key == "relay_auth_key") {
                settings_.relayAuthKey = val;
            } else if (key == "stun_server") {
                if (!val.empty()) settings_.stunServer = val;
            } else if (key == "relay_mode") {
                try { settings_.relayMode = static_cast<uint8_t>(std::clamp(std::stoi(val), 0, 2)); } catch (...) {}
            } else if (key == "dark_theme") {
                settings_.darkTheme = (val == "1" || val == "true");
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

    bool modified = false;
    if (deskId_ < 100000000ULL || deskId_ > 999999999ULL) {
        deskId_ = CryptoUtils::generateNineDigitId();
        modified = true;
    }

    if (sessionCode_.empty()) {
        sessionCode_ = generateRandomSixCharCode();
    }

    // Transparently migrate legacy plaintext password to salted SHA-256 verifier
    if (!legacyPlainPassword.empty() && unattendedVerifier_.empty()) {
        unattendedVerifier_ = CryptoUtils::hashPassword(legacyPlainPassword, std::to_string(deskId_));
        modified = true;
    }

    if (unattendedVerifier_.empty()) {
        unattendedVerifier_ = CryptoUtils::hashPassword(sessionCode_, std::to_string(deskId_));
        modified = true;
    }

    if (modified) {
        save();
    }
    return true;
}

bool IdentityManager::save() const {
    std::ofstream out(configPath_, std::ios::trunc);
    if (!out.is_open()) return false;

    out << "# CppDesk Configuration (Salted SHA-256 Verifier Protected)\n";
    out << "desk_id=" << deskId_ << "\n";
    out << "listen_port=" << listenPort_ << "\n";
    out << "unattended_enabled=" << (unattendedEnabled_ ? "1" : "0") << "\n";
    out << "unattended_verifier=" << unattendedVerifier_ << "\n";
    out << "relay_server=" << (settings_.relayServer.empty() ? relayServerAddr_ : settings_.relayServer) << "\n";
    out << "relay_auth_key=" << settings_.relayAuthKey << "\n";
    out << "stun_server=" << settings_.stunServer << "\n";
    out << "relay_mode=" << static_cast<int>(settings_.relayMode) << "\n";
    out << "dark_theme=" << (settings_.darkTheme ? "1" : "0") << "\n";
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
    out << "auto_virtual_display=" << (settings_.autoVirtualDisplay ? "1" : "0") << "\n";
    auto sanitizeIniValue = [](const std::string& in) -> std::string {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            if (c == '|' || c == '\r' || c == '\n') {
                out.push_back(' ');
            } else {
                out.push_back(c);
            }
        }
        return out;
    };

    out << "privacy_notice=" << sanitizeIniValue(settings_.privacyCustomNotice) << "\n";
    out << "privacy_brand=" << sanitizeIniValue(settings_.privacyBrandName) << "\n";
    out << "privacy_show_id=" << (settings_.privacyShowDeskId ? "1" : "0") << "\n";
    out << "hardware_dpms_blanking=" << (settings_.hardwareDpmsBlanking ? "1" : "0") << "\n";
    out << "totp_enabled=" << (settings_.totpEnabled ? "1" : "0") << "\n";
    out << "totp_secret=" << settings_.totpSecret << "\n";
    out << "totp_algorithm=" << settings_.totpAlgorithm << "\n";
    out << "privacy_cloak_enabled=" << (settings_.privacyCloakEnabled ? "1" : "0") << "\n";
    out << "privacy_cloak_keywords=" << sanitizeIniValue(settings_.privacyCloakKeywords) << "\n";
    out << "privacy_cloak_style=" << static_cast<int>(settings_.privacyCloakStyle) << "\n";

    for (const auto& r : recentSessions_) {
        out << "recent=" << r.deskId << "|" << sanitizeIniValue(r.hostname) << "|" << sanitizeIniValue(r.address) << "|" << (r.isFavorite ? "1" : "0")
            << "|" << sanitizeIniValue(r.alias) << "|" << sanitizeIniValue(r.tag) << "|" << sanitizeIniValue(r.notes) << "\n";
    }
    return true;
}

void IdentityManager::setUnattendedEnabled(bool enabled) {
    unattendedEnabled_ = enabled;
    save();
}

void IdentityManager::setUnattendedPassword(const std::string& newPassword) {
    if (newPassword.empty()) return;
    unattendedPassword_ = newPassword;
    unattendedVerifier_ = CryptoUtils::hashPassword(newPassword, std::to_string(deskId_));
    save();
}

std::string IdentityManager::regenerateSessionCode() {
    sessionCode_ = generateRandomSixCharCode();
    if (unattendedPassword_.empty()) {
        unattendedVerifier_ = CryptoUtils::hashPassword(sessionCode_, std::to_string(deskId_));
        save();
    }
    return sessionCode_;
}

bool IdentityManager::verifyChallengeResponse(
    uint64_t clientId,
    const std::array<uint8_t, 32>& nonce,
    const std::array<uint8_t, 32>& clientDigest) const
{
    if (!unattendedEnabled_) {
        return false;
    }

    // 1. Check against stored salted SHA-256 unattended password verifier
    if (!unattendedVerifier_.empty()) {
        auto expected = CryptoUtils::computeChallengeResponseFromToken(
            unattendedVerifier_, deskId_, clientId, nonce
        );
        if (constantTimeEquals32(expected, clientDigest)) {
            return true;
        }
    }

    // 2. Check against active One-Time Session Code
    if (!sessionCode_.empty()) {
        auto expectedCode = CryptoUtils::computeChallengeResponse(
            sessionCode_, deskId_, clientId, nonce
        );
        if (constantTimeEquals32(expectedCode, clientDigest)) {
            return true;
        }
    }

    return false;
}

void IdentityManager::setRelayServerAddress(const std::string& addr) {
    relayServerAddr_ = addr;
    settings_.relayServer = addr;
    save();
}

void IdentityManager::addOrUpdateRecentSession(uint64_t id, const std::string& host, const std::string& addr,
                                             const std::string& alias, const std::string& tag, const std::string& notes) {
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
    save();
}

void IdentityManager::updateRecentSessionMetadata(uint64_t id, const std::string& alias, const std::string& tag, const std::string& notes) {
    for (auto& e : recentSessions_) {
        if (e.deskId == id) {
            e.alias = alias;
            e.tag = tag;
            e.notes = notes;
            save();
            break;
        }
    }
}

void IdentityManager::toggleFavoriteSession(uint64_t id) {
    for (auto& e : recentSessions_) {
        if (e.deskId == id) {
            e.isFavorite = !e.isFavorite;
            break;
        }
    }
    std::stable_sort(recentSessions_.begin(), recentSessions_.end(), [](const RecentSessionEntry& a, const RecentSessionEntry& b) {
        return a.isFavorite && !b.isFavorite;
    });
    save();
}

void IdentityManager::removeRecentSession(uint64_t id) {
    recentSessions_.erase(
        std::remove_if(recentSessions_.begin(), recentSessions_.end(), [&](const RecentSessionEntry& e) {
            return e.deskId == id;
        }),
        recentSessions_.end()
    );
    save();
}

void IdentityManager::clearRecentSessions() {
    recentSessions_.clear();
    save();
}

void IdentityManager::updateSettings(const AppSettings& newSettings) {
    settings_ = newSettings;
    settings_.targetFps = clampTargetFps(settings_.targetFps);
    settings_.defaultPermissions &= PERM_ALL;
    relayServerAddr_ = settings_.relayServer;
    if (!settings_.totpSecret.empty()) {
        TotpAlgorithm alg = (settings_.totpAlgorithm == "SHA256") ? TotpAlgorithm::Sha256 : TotpAlgorithm::Sha1;
        totpManager_.setSecret(settings_.totpSecret, alg);
    } else {
        totpManager_.clear();
    }
    save();
}

void IdentityManager::resetSettingsToDefault() {
    settings_ = AppSettings{};
    totpManager_.clear();
    relayServerAddr_ = settings_.relayServer;
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

bool EcdhKeyExchange::computeSharedSessionKey(
    const uint8_t* peerPubBlob,
    size_t peerPubLen,
    uint64_t hostId,
    uint64_t viewerId,
    const std::array<uint8_t, 32>& nonce,
    std::array<uint8_t, 32>& outSessionKey)
{
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
    if (status < 0) return false;

    status = BCryptSetProperty(
        hAesAlg,
        BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        sizeof(BCRYPT_CHAIN_MODE_GCM),
        0
    );
    if (status < 0) {
        BCryptCloseAlgorithmProvider(hAesAlg, 0);
        return false;
    }
    hAesAlg_ = reinterpret_cast<uintptr_t>(hAesAlg);

    DWORD keyObjSize = 0;
    DWORD cbData = 0;
    BCryptGetProperty(hAesAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&keyObjSize), sizeof(keyObjSize), &cbData, 0);
    if (keyObjSize > 0) {
        keyObjBuf_.resize(keyObjSize);
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

    replayWindow_.reset();
    initialized_ = true;
    return true;
}

void AesGcmSessionCipher::reset() {
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
    if (!initialized_ || !hAesKey_ || payloadLen < 28) return false;
    BCRYPT_KEY_HANDLE hKey = reinterpret_cast<BCRYPT_KEY_HANDLE>(hAesKey_);

    const uint8_t* nonce = encryptedPayload;
    const uint8_t* ciphertext = encryptedPayload + 12;
    size_t cipherLen = payloadLen - 28;
    const uint8_t* tag = encryptedPayload + 12 + cipherLen;

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

#elif defined(__APPLE__)

// ---------------- EcdhKeyExchange (macOS Security Framework) ----------------

bool EcdhKeyExchange::initialize() {
    reset();

    CFMutableDictionaryRef parameters = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(parameters, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    int keySize = 256;
    CFNumberRef numSize = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keySize);
    CFDictionarySetValue(parameters, kSecAttrKeySizeInBits, numSize);
    CFRelease(numSize);

    CFErrorRef error = nullptr;
    SecKeyRef privateKey = SecKeyCreateRandomKey(parameters, &error);
    CFRelease(parameters);

    if (!privateKey) {
        if (error) CFRelease(error);
        return false;
    }
    hKey_ = reinterpret_cast<uintptr_t>(privateKey);

    SecKeyRef publicKey = SecKeyCopyPublicKey(privateKey);
    if (!publicKey) {
        reset();
        return false;
    }

    CFDataRef pubData = SecKeyCopyExternalRepresentation(publicKey, &error);
    CFRelease(publicKey);
    if (!pubData) {
        if (error) CFRelease(error);
        reset();
        return false;
    }

    const uint8_t* bytes = CFDataGetBytePtr(pubData);
    CFIndex len = CFDataGetLength(pubData);
    // Apple uncompressed EC point is 65 bytes: 0x04 || X (32 bytes) || Y (32 bytes)
    if (len == 65 && bytes[0] == 0x04) {
        // Wrap with standard 8-byte CNG header: dwMagic (0x314B4345) || cbKey (32)
        localPubBlob_.resize(72);
        uint32_t magic = 0x314B4345;
        uint32_t cbKey = 32;
        std::memcpy(localPubBlob_.data(), &magic, 4);
        std::memcpy(localPubBlob_.data() + 4, &cbKey, 4);
        std::memcpy(localPubBlob_.data() + 8, bytes + 1, 64);
        initialized_ = true;
    }
    CFRelease(pubData);
    return initialized_;
}

bool EcdhKeyExchange::computeSharedSessionKey(
    const uint8_t* peerPubBlob,
    size_t peerPubLen,
    uint64_t hostId,
    uint64_t viewerId,
    const std::array<uint8_t, 32>& nonce,
    std::array<uint8_t, 32>& outSessionKey)
{
    if (!initialized_ || !peerPubBlob || peerPubLen < 72 || !hKey_) return false;

    // Verify 8-byte CNG header
    uint32_t magic = 0, cbKey = 0;
    std::memcpy(&magic, peerPubBlob, 4);
    std::memcpy(&cbKey, peerPubBlob + 4, 4);
    if (magic != 0x314B4345 || cbKey != 32) return false;

    // Format ANSI X9.63 uncompressed point: 0x04 || X (32) || Y (32)
    std::vector<uint8_t> x963(65);
    x963[0] = 0x04;
    std::memcpy(x963.data() + 1, peerPubBlob + 8, 64);

    CFDataRef peerData = CFDataCreate(kCFAllocatorDefault, x963.data(), x963.size());
    CryptoUtils::secureZero(x963.data(), x963.size());
    if (!peerData) return false;

    CFMutableDictionaryRef keyAttrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(keyAttrs, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(keyAttrs, kSecAttrKeyClass, kSecAttrKeyClassPublic);

    CFErrorRef error = nullptr;
    SecKeyRef peerKey = SecKeyCreateWithData(peerData, keyAttrs, &error);
    CFRelease(keyAttrs);
    CFRelease(peerData);

    if (!peerKey) {
        if (error) CFRelease(error);
        return false;
    }

    SecKeyRef privateKey = reinterpret_cast<SecKeyRef>(hKey_);
    CFMutableDictionaryRef exchangeParams = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFDataRef sharedSecret = SecKeyCopyKeyExchangeResult(
        privateKey, kSecKeyAlgorithmECDHKeyExchangeStandard, peerKey, exchangeParams, &error);
    CFRelease(exchangeParams);
    CFRelease(peerKey);

    if (!sharedSecret) {
        if (error) CFRelease(error);
        return false;
    }

    const uint8_t* secretPtr = CFDataGetBytePtr(sharedSecret);
    CFIndex secretLen = CFDataGetLength(sharedSecret);

    outSessionKey = CryptoUtils::deriveSessionKey(hostId, viewerId, nonce, secretPtr, static_cast<size_t>(secretLen));
    CFRelease(sharedSecret);
    return true;
}

void EcdhKeyExchange::reset() {
    if (hKey_) {
        CFRelease(reinterpret_cast<SecKeyRef>(hKey_));
        hKey_ = 0;
    }
    hAlg_ = 0;
    CryptoUtils::secureZero(localPubBlob_.data(), localPubBlob_.size());
    localPubBlob_.clear();
    initialized_ = false;
}

// ---------------- AesGcmSessionCipher (macOS CommonCrypto CCCryptorGCM) ----------------

bool AesGcmSessionCipher::initialize(const std::array<uint8_t, 32>& sessionKey, bool isHost) {
    reset();
    isHost_ = isHost;
    sessionKey_ = sessionKey;
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
    if (!initialized_) return false;

    uint8_t nonce[12] = {};
    if (isHost_) {
        nonce[0] = 'H'; nonce[1] = 'S'; nonce[2] = 'T'; nonce[3] = '1';
    } else {
        nonce[0] = 'V'; nonce[1] = 'I'; nonce[2] = 'W'; nonce[3] = '1';
    }
    std::memcpy(nonce + 4, &seq, 8);

    uint8_t tag[16] = {};
    size_t tagLen = sizeof(tag);

    outEncryptedPayload.resize(12 + plainLen + 16);
    std::memcpy(outEncryptedPayload.data(), nonce, 12);

    CCCryptorStatus status = CCCryptorGCM(
        kCCEncrypt,
        kCCAlgorithmAES,
        sessionKey_.data(),
        sessionKey_.size(),
        nonce,
        sizeof(nonce),
        aad,
        aadLen,
        plaintext,
        plainLen,
        plainLen > 0 ? outEncryptedPayload.data() + 12 : nullptr,
        tag,
        &tagLen
    );

    if (status != kCCSuccess) {
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
    if (!initialized_ || payloadLen < 28) return false;

    const uint8_t* nonce = encryptedPayload;
    size_t cipherLen = payloadLen - 28;
    const uint8_t* ciphertext = encryptedPayload + 12;
    const uint8_t* tag = encryptedPayload + 12 + cipherLen;

    uint64_t seq = 0;
    std::memcpy(&seq, nonce + 4, 8);

    if (!replayWindow_.check(seq)) {
        return false;
    }

    outPlaintext.resize(cipherLen);
    uint8_t tagCopy[16];
    std::memcpy(tagCopy, tag, 16);
    size_t tagLen = 16;

    CCCryptorStatus status = CCCryptorGCM(
        kCCDecrypt,
        kCCAlgorithmAES,
        sessionKey_.data(),
        sessionKey_.size(),
        nonce,
        12,
        aad,
        aadLen,
        ciphertext,
        cipherLen,
        cipherLen > 0 ? outPlaintext.data() : nullptr,
        tagCopy,
        &tagLen
    );

    if (status != kCCSuccess) {
        outPlaintext.clear();
        return false;
    }

    replayWindow_.mark(seq);
    if (outSeq) *outSeq = seq;
    return true;
}

#endif

bool AesGcmSessionCipher::ratchetKey() {
    if (!initialized_) return false;
    std::array<uint8_t, 32> nextKey = CryptoUtils::hkdfSha256(
        sessionKey_.data(), sessionKey_.size(),
        sessionKey_.data(), sessionKey_.size(),
        "CppDesk-Ratchet-NextKey"
    );
    return initialize(nextKey, isHost_);
}

} // namespace cppdesk
