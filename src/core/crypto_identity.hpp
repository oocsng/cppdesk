#pragma once

#include "protocol.hpp"
#include "totp_manager.hpp"
#include "crypto_compat.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace cppdesk {

struct RecentSessionEntry {
    uint64_t    deskId = 0;
    std::string hostname;
    std::string address;
    bool        isFavorite = false;
    std::string alias;
    std::string tag;
    std::string notes;
};

struct AppSettings {
    bool          darkTheme = false;                     // false = Light (White & Blue), true = Dark (Black & Blue)
    uint8_t       targetFps = 30;                        // 15, 30, 45, or 60 FPS
    bool          adaptiveFps = true;                    // Auto-drop FPS when network connection is poor
    QualityPreset defaultQuality = QualityPreset::Balanced;
    ConnectionProfile connectionProfile = ConnectionProfile::BalancedBandwidth; // One-Click Connection Quality Profile
    uint8_t       defaultScaleMode = 0;                  // 0 = FitAspect, 1 = Stretch, 2 = Original 1:1
    bool          showRemoteCursor = true;               // Render remote cursor ring on viewer canvas
    bool          showSessionHud = true;                 // Show live FPS / RTT / Bitrate telemetry in session bar
    bool          autoAcceptIncoming = false;            // Automatically accept incoming requests without modal
    uint8_t       defaultPermissions = PERM_ALL;         // Default permissions granted to incoming viewers
    bool          lockWorkstationOnDisconnect = false;   // Lock Windows workstation when host session ends
    bool          enablePushNotifications = true;        // Windows Action Center & Tray Push Toasts
    bool          enableTaskbarFlash = true;             // Pulse taskbar orange when background alerts arrive
    bool          enableNotificationSounds = true;       // Audio chime on incoming alerts
    bool          minimizeToTray = false;                // Minimize window to Windows Notification Area

    // Self-Hosted Relay & STUN Configuration
    std::string   relayServer = "127.0.0.1:50999";
    std::string   relayAuthKey = "";
    std::string   stunServer = "stun.l.google.com:19302";
    uint8_t       relayMode = 0;                         // 0 = Auto, 1 = Self-Hosted, 2 = Direct LAN

    // Hardware Accelerated Rendering Toggle
    bool          hardwareAcceleration = true;           // true = GPU (Direct3D 11 / Direct2D default), false = Software (WARP / CPU)

    // Audio Controls & Persistence
    uint8_t       defaultAudioVolume = 100;              // 0 - 100%
    bool          audioMutedDefault = false;             // false = audible, true = muted

    // Virtual Multi-Display Driver & Headless Emulation
    bool          autoVirtualDisplay = true;             // Auto-provision virtual display for headless host

    // Privacy Mode Curtain Screen Custom Branding & Notice
    std::string   privacyCustomNotice = "Screen output hidden and local physical inputs secured for authorized administration.";
    std::string   privacyBrandName = "CppDesk Enterprise Security";
    bool          privacyShowDeskId = true;
    bool          hardwareDpmsBlanking = true;           // Hardware DPMS monitor standby coupled with curtain mode (default true)

    // Two-Factor Authentication (TOTP RFC 6238)
    bool          totpEnabled = false;
    std::string   totpSecret = "";        // Base32 encoded 160-bit secret
    std::string   totpAlgorithm = "SHA1"; // Default SHA1

    // Privacy Cloak: Sensitive Window & PII Auto-Redaction Mask
    bool          privacyCloakEnabled = true;
    std::string   privacyCloakKeywords = "KeePass, Bitwarden, 1Password, Authenticator, Credential, Wallet, .env, Banking";
    uint8_t       privacyCloakStyle = 0; // 0 = Solid Charcoal, 1 = Mosaic
};

class CryptoUtils {
public:
    static bool randomBytes(void* buffer, size_t len);
    static uint64_t generateNineDigitId();

    // Memory protection & zeroization (Option 4A)
    static void secureZero(void* ptr, size_t len);
    static bool pinMemory(void* ptr, size_t len);
    static void unpinMemory(void* ptr, size_t len);

    static std::array<uint8_t, 32> sha256(const void* data, size_t len);
    static std::array<uint8_t, 32> sha256(const std::string& text);
    static std::string toHex(const uint8_t* data, size_t len);
    static std::vector<uint8_t> fromHex(const std::string& hex);
    static std::string sha256Hex(const std::string& text);

    // Cryptographic PRF / KDF helpers (RFC 5869 HKDF-SHA256)
    static std::array<uint8_t, 32> hmacSha256(const void* key, size_t keyLen, const void* msg, size_t msgLen);
    static std::array<uint8_t, 32> hkdfSha256(
        const void* ikm, size_t ikmLen,
        const void* salt, size_t saltLen,
        const std::string& info);

    // Salted password hash for storage & verification (never stores plaintext on disk)
    static std::string hashPassword(const std::string& password, const std::string& salt);

    // Challenge-response digest from precomputed verifier token: SHA256(passToken + hostId + clientId + nonce)
    static std::array<uint8_t, 32> computeChallengeResponseFromToken(
        const std::string& passwordVerifierToken,
        uint64_t hostId,
        uint64_t clientId,
        const std::array<uint8_t, 32>& nonce);

    // Challenge-response digest from plaintext password
    static std::array<uint8_t, 32> computeChallengeResponse(
        const std::string& passwordPlain,
        uint64_t hostId,
        uint64_t clientId,
        const std::array<uint8_t, 32>& nonce);

    // Derive a 256-bit session key from handshake parameters (supports ECDH shared secret)
    static std::array<uint8_t, 32> deriveSessionKey(
        uint64_t hostId,
        uint64_t clientId,
        const std::array<uint8_t, 32>& nonce,
        const uint8_t* ecdhSecret = nullptr,
        size_t ecdhSecretLen = 0);

    // 8-character uppercase SAS fingerprint ("XXXX-XXXX") derived from sessionKey to verify zero MITM
    static std::string sessionFingerprintHex(const std::array<uint8_t, 32>& sessionKey);

    // Fast stream cipher for payload confidentiality (fallback / test)
    static void transformPayload(
        uint8_t* data,
        size_t len,
        const std::array<uint8_t, 32>& sessionKey,
        uint64_t frameSequence);

    // Format 9-digit ID as "123 456 789"
    static std::string formatDeskId(uint64_t id);

    // Parse "123 456 789" or "123456789" -> 123456789 (returns 0 if not a 9-digit ID)
    static uint64_t parseDeskId(const std::string& input);
};

// ---------------- AntiReplayWindow (Option 4A) ----------------
class AntiReplayWindow {
public:
    AntiReplayWindow() = default;

    // Pure read-only check: returns true if sequence number is valid and un-replayed
    bool check(uint64_t seq) const;
    // Mutates window state to record sequence number (call ONLY after authentication succeeds)
    void mark(uint64_t seq);
    // Combined helper for test backward compatibility
    bool checkAndMark(uint64_t seq);
    void reset();

private:
    uint64_t maxSeq_ = 0;
    uint64_t bitmap_ = 0; // 64-packet sliding window bitmask
};

// ---------------- EcdhKeyExchange (Option 1A) ----------------
// Ephemeral NIST P-256 Elliptic-Curve Diffie-Hellman via Windows CNG
class EcdhKeyExchange {
public:
    EcdhKeyExchange();
    ~EcdhKeyExchange();

    EcdhKeyExchange(const EcdhKeyExchange&) = delete;
    EcdhKeyExchange& operator=(const EcdhKeyExchange&) = delete;

    bool initialize();
    bool isInitialized() const { return initialized_; }
    const std::vector<uint8_t>& localPublicKey() const { return localPubBlob_; }

    // Computes shared secret with peer's public key, mixes with nonce & IDs via HKDF-SHA256
    bool computeSharedSessionKey(
        const uint8_t* peerPubBlob,
        size_t peerPubLen,
        uint64_t hostId,
        uint64_t viewerId,
        const std::array<uint8_t, 32>& nonce,
        std::array<uint8_t, 32>& outSessionKey);

    void reset();

private:
    bool                 initialized_ = false;
    uintptr_t            hAlg_ = 0;
    uintptr_t            hKey_ = 0;
    std::vector<uint8_t> localPubBlob_;
};

// ---------------- AesGcmSessionCipher (Option 1A & 4A) ----------------
// Hardware AES-NI accelerated AES-256-GCM AEAD cipher with 16-byte auth tags and anti-replay protection
class AesGcmSessionCipher {
public:
    AesGcmSessionCipher();
    ~AesGcmSessionCipher();

    AesGcmSessionCipher(const AesGcmSessionCipher&) = delete;
    AesGcmSessionCipher& operator=(const AesGcmSessionCipher&) = delete;

    bool initialize(const std::array<uint8_t, 32>& sessionKey, bool isHost);
    void reset();
    bool isInitialized() const { return initialized_; }

    // Wire format: [12-byte Nonce][Ciphertext][16-byte GCM Tag]
    // AAD: typically 10-byte FrameHeader with final payloadSize = 12 + plainLen + 16
    bool encrypt(
        const void* plaintext,
        size_t plainLen,
        uint64_t seq,
        const void* aad,
        size_t aadLen,
        std::vector<uint8_t>& outEncryptedPayload);

    // Decrypts and authenticates payload with GCM tag and anti-replay check
    bool decrypt(
        const uint8_t* encryptedPayload,
        size_t payloadLen,
        const void* aad,
        size_t aadLen,
        std::vector<uint8_t>& outPlaintext,
        uint64_t* outSeq = nullptr);

    // Periodic key ratcheting (Option 4A)
    bool ratchetKey();

    const std::array<uint8_t, 32>& currentKey() const { return sessionKey_; }

private:
    bool                    initialized_ = false;
    bool                    isHost_ = false;
    std::array<uint8_t, 32> sessionKey_{};
    uintptr_t               hAesAlg_ = 0;
    uintptr_t               hAesKey_ = 0;
    std::vector<uint8_t>    keyObjBuf_;
    AntiReplayWindow        replayWindow_;
};

class IdentityManager {
public:
    explicit IdentityManager(int instanceId = 1);

    bool loadOrCreate();
    bool save() const;

    int instanceId() const { return instanceId_; }
    uint64_t deskId() const { return deskId_; }
    std::string formattedDeskId() const { return CryptoUtils::formatDeskId(deskId_); }
    const std::string& hostname() const { return hostname_; }
    uint16_t listenPort() const { return listenPort_; }
    void setListenPort(uint16_t port) { listenPort_ = port; }

    bool unattendedEnabled() const { return unattendedEnabled_; }
    void setUnattendedEnabled(bool enabled);

    // Returns active dynamic session code (or in-memory unattended password if set during this run)
    const std::string& unattendedPassword() const { return unattendedPassword_.empty() ? sessionCode_ : unattendedPassword_; }
    void setUnattendedPassword(const std::string& newPassword);
    bool hasStoredPasswordVerifier() const { return !unattendedVerifier_.empty(); }
    const std::string& passwordVerifier() const { return unattendedVerifier_; }

    // Dynamic 6-character One-Time Session Password (in-memory only, regenerable anytime)
    const std::string& sessionCode() const { return sessionCode_; }
    std::string regenerateSessionCode();

    bool verifyChallengeResponse(
        uint64_t clientId,
        const std::array<uint8_t, 32>& nonce,
        const std::array<uint8_t, 32>& clientDigest) const;

    const std::string& relayServerAddress() const { return relayServerAddr_; }
    void setRelayServerAddress(const std::string& addr);

    const std::vector<RecentSessionEntry>& recentSessions() const { return recentSessions_; }
    void addOrUpdateRecentSession(uint64_t id, const std::string& host, const std::string& addr,
                                 const std::string& alias = "", const std::string& tag = "", const std::string& notes = "");
    void updateRecentSessionMetadata(uint64_t id, const std::string& alias, const std::string& tag, const std::string& notes);
    void toggleFavoriteSession(uint64_t id);
    void removeRecentSession(uint64_t id);
    void clearRecentSessions();

    const AppSettings& settings() const { return settings_; }
    void updateSettings(const AppSettings& newSettings);
    void resetSettingsToDefault();

    TotpManager& totpManager() { return totpManager_; }
    const TotpManager& totpManager() const { return totpManager_; }

    std::string configFilePath() const { return configPath_; }

private:
    int                             instanceId_ = 1;
    uint64_t                        deskId_ = 0;
    std::string                     hostname_;
    uint16_t                        listenPort_ = 50990;
    bool                            unattendedEnabled_ = true;
    std::string                     unattendedPassword_; // In-memory only (never written in plaintext to disk)
    std::string                     unattendedVerifier_; // Salted SHA-256 verifier persisted in config_*.ini
    std::string                     sessionCode_;        // Dynamic 6-char One-Time Session Password
    std::string                     relayServerAddr_ = "127.0.0.1:50999";
    std::vector<RecentSessionEntry> recentSessions_;
    AppSettings                     settings_{};
    TotpManager                     totpManager_;
    std::string                     configPath_;
};

} // namespace cppdesk
