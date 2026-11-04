#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cppdesk {

enum class TotpAlgorithm : uint8_t {
    Sha1   = 0,
    Sha256 = 1
};

class TotpManager {
public:
    TotpManager();
    ~TotpManager();

    // Configuration
    bool isConfigured() const;
    void setSecret(const std::string& base32Secret, TotpAlgorithm alg = TotpAlgorithm::Sha1);
    void setSecretRaw(const uint8_t* key, size_t keyLen, TotpAlgorithm alg = TotpAlgorithm::Sha1);
    void clear();

    std::string getSecretBase32() const { return secretBase32_; }
    TotpAlgorithm getAlgorithm() const { return algorithm_; }
    void setAlgorithm(TotpAlgorithm alg) { algorithm_ = alg; }

    // Generates a new cryptographically random 20-byte (160-bit) secret
    void generateNewSecret(TotpAlgorithm alg = TotpAlgorithm::Sha1);

    // Code generation
    // If unixTime == 0, current system time std::time(nullptr) is used.
    std::string generateCode(uint64_t unixTime = 0, int digits = 6) const;

    // Code verification with drift window and single-use replay protection
    // driftSteps: +/- drift steps (1 step = 30s; driftSteps = 1 checks T-1, T, T+1)
    bool verifyCode(const std::string& code, uint64_t unixTime = 0, int driftSteps = 1);

    // URI & formatting
    std::string buildOtpAuthUri(uint64_t deskId, const std::string& issuer = "CppDesk") const;
    static std::string formatBase32Secret(const std::string& rawBase32);

    // Base32 RFC 4648 encoding & decoding
    static std::string encodeBase32(const uint8_t* data, size_t length);
    static bool decodeBase32(const std::string& input, std::vector<uint8_t>& out);

    // Dynamic truncation & HOTP calculation per RFC 4226 / RFC 6238
    static bool computeHotpCode(
        TotpAlgorithm alg,
        const uint8_t* key,
        size_t keyLen,
        uint64_t timeStep,
        int digits,
        uint32_t& outCode);

    // Big-endian 64-bit HMAC computation using Windows CNG BCrypt
    static bool computeHmac(
        TotpAlgorithm alg,
        const uint8_t* key,
        size_t keyLen,
        const uint8_t* data,
        size_t dataLen,
        std::vector<uint8_t>& digestOut);

    uint64_t lastConsumedStep() const { return lastConsumedStep_; }
    void resetLastConsumedStep() { lastConsumedStep_ = 0; }

private:
    std::vector<uint8_t> secretBytes_;
    std::string          secretBase32_;
    TotpAlgorithm        algorithm_ = TotpAlgorithm::Sha1;
    uint64_t             lastConsumedStep_ = 0;
};

} // namespace cppdesk
