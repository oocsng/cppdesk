#pragma once

#include "protocol.hpp"
#include "totp_manager.hpp"
#include "crypto_compat.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <array>
#include <mutex>
#include <optional>

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
    int           audioInputDeviceId = -1;               // -1 = Default (WAVE_MAPPER)
    int           audioOutputDeviceId = -1;              // -1 = Default (WAVE_MAPPER)
    std::string   audioInputDeviceName = "System Default";
    std::string   audioOutputDeviceName = "System Default";

    // Virtual Multi-Display Driver & Headless Emulation
    // Remembered answer to the Wayland desktop's screen sharing question (a restore token), kept with the other settings
    std::string   waylandRestoreToken = "";
    bool          autoVirtualDisplay = false;            // Auto-provision virtual display for headless host

    // Privacy Mode Curtain Screen Custom Branding & Notice
    std::string   privacyCustomNotice = "Privacy screen is on. Move the mouse or press a key on this PC to end it.";
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

    // LAN Fleet: lets other PCs pair with this one (config key allow_pairing); off until the user turns it on
    bool          allowPairing = false;

    // Glide: desk ID of the paired PC placed past each screen edge (config keys glide_left/right/top/bottom); 0 = nothing placed
    uint64_t      glideLeft = 0, glideRight = 0, glideTop = 0, glideBottom = 0;
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

    static constexpr uint32_t kDefaultPbkdf2Iterations = 200000;
    static constexpr uint32_t kMinPbkdf2Iterations     = 10000;
    static constexpr uint32_t kMaxPbkdf2Iterations     = 2000000;
    static constexpr size_t   kPbkdf2SaltLen           = 16;

    // PBKDF2-HMAC-SHA256 key derivation
    static bool pbkdf2Sha256(
        const std::string& password,
        const uint8_t* salt,
        size_t saltLen,
        uint32_t iterations,
        std::array<uint8_t, 32>& outDerivedKey);

    // Salted PBKDF2-HMAC-SHA256 password verifier for storage ("pbkdf2_sha256$<iter>$<saltHex>$<dkHex>")
    static std::string hashPassword(
        const std::string& password,
        uint32_t iterations = kDefaultPbkdf2Iterations);

    static bool parsePasswordVerifier(
        const std::string& verifier,
        uint32_t& outIterations,
        std::array<uint8_t, 16>& outSalt,
        std::array<uint8_t, 32>& outDerivedKey);

    // Challenge-response digest from precomputed verifier token: SHA256(passToken + hostId + clientId + nonce)
    static std::array<uint8_t, 32> computeChallengeResponseFromToken(
        const std::string& passwordVerifierToken,
        uint64_t hostId,
        uint64_t clientId,
        const std::array<uint8_t, 32>& nonce);

    // Challenge-response digest from plaintext password and PBKDF2 parameters
    static std::array<uint8_t, 32> computeChallengeResponse(
        const std::string& passwordPlain,
        const std::array<uint8_t, 16>& salt,
        uint32_t iterations,
        uint64_t hostId,
        uint64_t clientId,
        const std::array<uint8_t, 32>& nonce);

    // Binds a challenge-response digest to the ECDH session key (HMAC-SHA256 keyed by it). The plain digest is
    // computable from public values, so anyone who sees it can test password guesses offline; the bound one
    // can only be tested by a party that holds an ephemeral ECDH private key.
    static std::array<uint8_t, 32> bindChallengeResponse(
        const std::array<uint8_t, 32>& sessionKey,
        const std::array<uint8_t, 32>& digest);

    // Digest a pairing signature covers: SHA-256 of "CPPDESK-PAIR-V1" | role ('H' host, 'V' viewer) | hostId u64 LE |
    // viewerId u64 LE | nonce | u16 LE len + hostEcdhPub | u16 LE len + viewerEcdhPub.
    static std::array<uint8_t, 32> pairingTranscript(
        char role,
        uint64_t hostId,
        uint64_t viewerId,
        const std::array<uint8_t, 32>& nonce,
        const std::vector<uint8_t>& hostEcdhPub,
        const std::vector<uint8_t>& viewerEcdhPub);

    // Derive a 256-bit session key from handshake parameters (supports ECDH shared secret)
    static std::array<uint8_t, 32> deriveSessionKey(
        uint64_t hostId,
        uint64_t clientId,
        const std::array<uint8_t, 32>& nonce,
        const uint8_t* ecdhSecret = nullptr,
        size_t ecdhSecretLen = 0);

    static bool isUsableKey(const std::array<uint8_t, 32>& key) {
        for (uint8_t b : key) {
            if (b != 0) return true;
        }
        return false;
    }

    // 8-character uppercase SAS fingerprint ("XXXX-XXXX") derived from sessionKey to verify zero MITM
    static std::string sessionFingerprintHex(const std::array<uint8_t, 32>& sessionKey);

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
    // Tests only: a fixed key pair instead of a random one, in the Windows CNG ECCPRIVATE blob layout (magic "ECK2",
    // key size 32, X, Y, d: 104 bytes), the same on every platform.
    bool initializeForTest(const std::vector<uint8_t>& privateBlob);
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

// ---------------- PairingKey ----------------
// A per-install ECDSA P-256 signing key. All byte strings are big-endian; signatures are raw r || s (not DER).
class PairingKey {
public:
    using PublicKey = std::array<uint8_t, 64>;  // X || Y
    using KeyPair   = std::array<uint8_t, 96>;  // X || Y || d
    using Signature = std::array<uint8_t, 64>;  // r || s

    PairingKey() = default;
    ~PairingKey();

    PairingKey(const PairingKey&) = delete;
    PairingKey& operator=(const PairingKey&) = delete;

    bool generate();
    bool importKeyPair(const KeyPair& kp);
    KeyPair exportKeyPair() const { return keyPair_; }
    const PublicKey& publicKey() const { return pub_; }
    bool isValid() const { return valid_; }

    bool sign(const std::array<uint8_t, 32>& digest, Signature& out) const;
    static bool verify(const PublicKey& pub, const std::array<uint8_t, 32>& digest, const Signature& sig);

private:
    bool      valid_ = false;
    KeyPair   keyPair_{};
    PublicKey pub_{};
};

// A PC this install has paired with: its desk ID, last known name and pinned signing key.
struct PairedPeer {
    uint64_t              deskId = 0;
    std::string           hostname;
    PairingKey::PublicKey publicKey{};
    uint64_t              pairedAtUnix = 0;
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

    const std::array<uint8_t, 32>& currentKey() const { return sessionKey_; }

private:
    bool                    initialized_ = false;
    bool                    isHost_ = false;
    std::array<uint8_t, 32> sessionKey_{};
    uintptr_t               hAesAlg_ = 0;
    uintptr_t               hAesKey_ = 0;
    uintptr_t               hAesDecryptKey_ = 0;
    std::vector<uint8_t>    keyObjBuf_;
    std::vector<uint8_t>    decryptKeyObjBuf_;
    AntiReplayWindow        replayWindow_;
};

class IdentityManager {
public:
    explicit IdentityManager(int instanceId = 1);

    bool loadOrCreate();
    bool save() const;
    bool saveConfig() const { return save(); }

    int instanceId() const { return instanceId_; }
    uint64_t deskId() const { std::lock_guard<std::mutex> lk(stateMutex_); return deskId_; }
    std::string formattedDeskId() const { return CryptoUtils::formatDeskId(deskId()); }
    std::string hostname() const { std::lock_guard<std::mutex> lk(stateMutex_); return hostname_; }
    uint16_t listenPort() const { std::lock_guard<std::mutex> lk(stateMutex_); return listenPort_; }
    void setListenPort(uint16_t port) { std::lock_guard<std::mutex> lk(stateMutex_); listenPort_ = port; }

    bool unattendedEnabled() const { std::lock_guard<std::mutex> lk(stateMutex_); return unattendedEnabled_; }
    void setUnattendedEnabled(bool enabled);

    // Returns active dynamic session code (or in-memory unattended password if set during this run)
    std::string unattendedPassword() const { std::lock_guard<std::mutex> lk(stateMutex_); return unattendedPassword_.empty() ? sessionCode_ : unattendedPassword_; }
    void setUnattendedPassword(const std::string& newPassword);
    bool hasStoredPasswordVerifier() const { std::lock_guard<std::mutex> lk(stateMutex_); return !unattendedVerifier_.empty(); }
    std::string passwordVerifier() const { std::lock_guard<std::mutex> lk(stateMutex_); return unattendedVerifier_; }

    // Dynamic 6-character One-Time Session Password (in-memory only, regenerable anytime)
    std::string sessionCode() const { std::lock_guard<std::mutex> lk(stateMutex_); return sessionCode_; }
    std::string regenerateSessionCode();
    // Sets the session code directly; used by the UI snapshot harness for repeatable output.
    void setSessionCode(const std::string& code) { std::lock_guard<std::mutex> lk(stateMutex_); sessionCode_ = code; }

    // Returns the active PBKDF2 salt and iteration count for AUTH_CHALLENGE.
    void getAuthKdfParams(std::array<uint8_t, 16>& outSalt, uint32_t& outIterations) const;

    // Verifies a bound challenge-response digest (unbound password mode is removed).
    bool verifyChallengeResponse(
        uint64_t clientId,
        const std::array<uint8_t, 32>& nonce,
        const std::array<uint8_t, 32>& clientDigest,
        const std::array<uint8_t, 32>& boundSessionKey,
        const std::array<uint8_t, 16>& challengeSalt,
        uint32_t challengeIterations) const;

    std::string relayServerAddress() const { std::lock_guard<std::mutex> lk(stateMutex_); return relayServerAddr_; }
    void setRelayServerAddress(const std::string& addr);

    std::vector<RecentSessionEntry> recentSessions() const { std::lock_guard<std::mutex> lk(stateMutex_); return recentSessions_; }
    void addOrUpdateRecentSession(uint64_t id, const std::string& host, const std::string& addr,
                                 const std::string& alias = "", const std::string& tag = "", const std::string& notes = "");
    void updateRecentSessionMetadata(uint64_t id, const std::string& alias, const std::string& tag, const std::string& notes);
    void toggleFavoriteSession(uint64_t id);
    void removeRecentSession(uint64_t id);
    void clearRecentSessions();

    AppSettings settings() const { std::lock_guard<std::mutex> lk(stateMutex_); return settings_; }
    void updateSettings(const AppSettings& newSettings);
    void resetSettingsToDefault();

    TotpManager& totpManager() { return totpManager_; }
    const TotpManager& totpManager() const { return totpManager_; }

    std::string configFilePath() const;
    void setConfigPathForTest(const std::filesystem::path& path);

    // Pairing (thread-safe; getters return copies). The add/remove calls save the config.
    const PairingKey& pairingKey() const { return pairingKey_; }       // created on first run
    std::vector<PairedPeer> pairedViewers() const;                     // may enter this PC without a password
    std::vector<PairedPeer> pairedHosts() const;                       // this PC may enter them; their key is pinned
    std::optional<PairedPeer> findPairedViewer(uint64_t deskId) const;
    std::optional<PairedPeer> findPairedHost(uint64_t deskId) const;
    void addPairedViewer(const PairedPeer& p);                         // replaces an entry with the same deskId
    void addPairedHost(const PairedPeer& p);
    bool removePairedViewer(uint64_t deskId);
    bool removePairedHost(uint64_t deskId);

private:
    int                             instanceId_ = 1;
    uint64_t                        deskId_ = 0;
    std::string                     hostname_;
    uint16_t                        listenPort_ = 50990;
    bool                            unattendedEnabled_ = true;
    std::string                     unattendedPassword_; // In-memory only (never written in plaintext to disk)
    std::string                     unattendedVerifier_; // PBKDF2-HMAC-SHA256 verifier persisted in config_*.ini
    std::array<uint8_t, 16>         sessionSalt_{};      // In-memory random salt for sessionCode_ when no unattended verifier is set
    uint32_t                        sessionIterations_ = CryptoUtils::kDefaultPbkdf2Iterations;
    std::string                     sessionCode_;        // Dynamic 6-char One-Time Session Password
    std::string                     relayServerAddr_ = "127.0.0.1:50999";
    std::vector<RecentSessionEntry> recentSessions_;
    AppSettings                     settings_{};
    TotpManager                     totpManager_;
    std::filesystem::path           configPath_;
    PairingKey                      pairingKey_;         // Hex in the config file as pair_key
    mutable std::mutex              saveMutex_;          // Lock order: saveMutex_ -> stateMutex_ -> pairMutex_
    mutable std::mutex              stateMutex_;         // Guards mutable identity/settings/recent state
    mutable std::mutex              pairMutex_;          // Guards the two paired lists
    std::vector<PairedPeer>         pairedViewers_;
    std::vector<PairedPeer>         pairedHosts_;
};

} // namespace cppdesk
