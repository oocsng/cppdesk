#pragma once

// Desk-ID ownership on a relay. A host's first registration of an ID enrolls a key for it (trust on first
// use); every later registration must answer a fresh challenge with HMAC(key, deskId, nonce), so nobody who
// merely knows the ID can take it over. The key is sent in the clear only at enrollment, never again.
// No platform headers.

#include "../core/crypto_identity.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cppdesk::relayowner {

using Key = std::array<uint8_t, 32>;
using Nonce = std::array<uint8_t, 16>;

constexpr uint8_t OWNER_CAP = 1;       // trailing byte on RELAY_REGISTER: "I can prove ownership"
constexpr uint8_t MODE_PROVE = 0;      // challenge: answer with computeProof()
constexpr uint8_t MODE_ENROLL = 1;     // challenge: answer with the key itself
constexpr size_t NONCE_LEN = 16;
constexpr uint64_t TTL_MS = 7ULL * 24 * 3600 * 1000;   // an ID nobody has used for this long is free again
constexpr size_t MAX_RECORDS = 100000;

// HMAC-SHA256(key, label || deskId || nonce), the same bytes the .NET relay computes.
inline Key computeProof(const Key& key, uint64_t deskId, const Nonce& nonce) {
    static const char kLabel[] = "CppDesk-relay-owner-v1";
    std::string msg(kLabel, sizeof(kLabel) - 1);
    msg.append(reinterpret_cast<const char*>(&deskId), sizeof(deskId));
    msg.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    return CryptoUtils::hmacSha256(key.data(), key.size(), msg.data(), msg.size());
}

// The key a host presents to one relay, so a relay that learns it at enrollment cannot use it elsewhere.
inline Key deriveRelayKey(const Key& installKey, const std::string& relayHostPort) {
    static const char kLabel[] = "CppDesk-relay-owner-key-v1|";
    std::string msg(kLabel, sizeof(kLabel) - 1);
    msg += relayHostPort;
    return CryptoUtils::hmacSha256(installKey.data(), installKey.size(), msg.data(), msg.size());
}

class OwnerRegistry {
public:
    bool hasOwner(uint64_t deskId, uint64_t nowMs) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return liveLocked(deskId, nowMs) != nullptr;
    }

    // False when the ID already has a live owner. When the table is full the ID is accepted but left
    // unprotected: refusing would lock real hosts out.
    bool enroll(uint64_t deskId, const Key& key, uint64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (liveLocked(deskId, nowMs)) return false;
        if (records_.size() >= MAX_RECORDS && records_.find(deskId) == records_.end()) return true;
        records_[deskId] = Record{key, nowMs};
        return true;
    }

    bool verify(uint64_t deskId, const Nonce& nonce, const Key& proof, uint64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        Record* rec = liveLocked(deskId, nowMs);
        if (!rec) return false;
        Key expected = computeProof(rec->key, deskId, nonce);
        uint8_t diff = 0;
        for (size_t i = 0; i < expected.size(); ++i) diff |= expected[i] ^ proof[i];
        if (diff != 0) return false;
        rec->lastSeenMs = nowMs;
        return true;
    }

    // Activity from the owning host keeps the record from lapsing.
    void touch(uint64_t deskId, uint64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (Record* rec = liveLocked(deskId, nowMs)) rec->lastSeenMs = nowMs;
    }

private:
    struct Record {
        Key key{};
        uint64_t lastSeenMs = 0;
    };

    Record* liveLocked(uint64_t deskId, uint64_t nowMs) const {
        auto it = records_.find(deskId);
        if (it == records_.end()) return nullptr;
        if (nowMs > it->second.lastSeenMs && nowMs - it->second.lastSeenMs > TTL_MS) {
            records_.erase(it);
            return nullptr;
        }
        return &it->second;
    }

    mutable std::mutex mutex_;
    // shortcut: lives in memory, so a relay restart forgets every owner and the first registrant wins again; persist when relays run for months.
    mutable std::unordered_map<uint64_t, Record> records_;
};

} // namespace cppdesk::relayowner
