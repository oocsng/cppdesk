#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <sstream>
#include <cctype>
#include <compare>

namespace cppdesk {

constexpr uint32_t PROTOCOL_MAGIC   = 0x43505044; // "CPPD" (CppDesk packet magic)
constexpr uint32_t RELAY_MAGIC      = 0x4344534B; // "CDSK" (CppDesk relay magic)
constexpr uint16_t PROTOCOL_VERSION = 3;
constexpr const char* CPP_DESK_VERSION = "3.5.3";
constexpr uint32_t CPP_DESK_VERSION_NUM = 0x030503;

struct SemanticVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    static SemanticVersion parse(const std::string& verStr) {
        SemanticVersion v;
        std::string s = verStr;
        size_t start = 0;
        while (start < s.size() && (std::isspace(static_cast<unsigned char>(s[start])) || s[start] == 'v' || s[start] == 'V')) {
            start++;
        }
        s = s.substr(start);
        if (s.empty()) return v;

        std::stringstream ss(s);
        std::string seg;
        std::vector<int> parts;
        while (std::getline(ss, seg, '.')) {
            try {
                size_t endDigit = 0;
                while (endDigit < seg.size() && std::isdigit(static_cast<unsigned char>(seg[endDigit]))) {
                    endDigit++;
                }
                parts.push_back(endDigit > 0 ? std::stoi(seg.substr(0, endDigit)) : 0);
            } catch (...) {
                parts.push_back(0);
            }
        }
        while (parts.size() < 3) parts.push_back(0);
        v.major = parts[0];
        v.minor = parts[1];
        v.patch = parts[2];
        return v;
    }

    std::string toString() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    auto operator<=>(const SemanticVersion&) const = default;
};

constexpr uint16_t DEFAULT_HOST_PORT      = 50990;
constexpr uint16_t DEFAULT_DISCOVERY_PORT = 50998;
constexpr uint16_t DEFAULT_RELAY_PORT     = 50999;

constexpr size_t MAX_PACKET_PAYLOAD_SIZE = 32 * 1024 * 1024; // 32 MB safety cap
constexpr size_t FILE_CHUNK_SIZE         = 64 * 1024;        // 64 KB per file chunk
constexpr int    TILE_SIZE               = 64;               // 64x64 dirty-tile grid

enum class PacketType : uint8_t {
    // Handshake & Authentication
    HELLO               = 0x01,
    AUTH_CHALLENGE      = 0x02,
    AUTH_RESPONSE       = 0x03,
    AUTH_WAITING        = 0x04, // Waiting for host user to click Accept/Reject
    AUTH_RESULT         = 0x05,
    PERMISSION_UPDATE   = 0x06,
    PING                = 0x07,
    PONG                = 0x08,
    DISCONNECT          = 0x09,
    TOTP_VERIFY         = 0x0A, // Viewer -> Host: 6-digit TOTP verification code

    // Video & Monitor Control
    VIDEO_CONFIG        = 0x10,
    VIDEO_FRAME_TILES   = 0x11,
    CURSOR_UPDATE       = 0x12,
    VIDEO_CONTROL_REQ   = 0x13, // Quality preset, keyframe request, or monitor switch
    MONITOR_LIST        = 0x14, // Host -> Viewer: List of active remote monitors
    MONITOR_SELECT      = 0x15, // Viewer -> Host: Request active monitor switch
    QUALITY_UPDATE      = 0x16, // Viewer -> Host: Live quality preset & FPS update
    AUDIO_STREAM_CHUNK  = 0x17, // Host -> Viewer: 48kHz 16-bit stereo PCM audio samples
    PRIVACY_MODE_TOGGLE = 0x18, // Viewer <-> Host: Enable/Disable Host Privacy Screen & input lock
    TUNNEL_OPEN         = 0x19, // Viewer -> Host: Request proxy connection to target port
    TUNNEL_DATA         = 0x1A, // Bidirectional: Multiplexed TCP payload chunk
    TUNNEL_CLOSE        = 0x1B, // Bidirectional: Terminate tunnel connection
    TERMINAL_DATA       = 0x1C, // Bidirectional: Interactive command shell I/O
    WHITEBOARD_PACKET   = 0x1D, // Bidirectional: Whiteboard strokes & laser pointer updates
    VIRTUAL_DISPLAY_CMD    = 0x1E, // Viewer -> Host: Create, Destroy, SetMode virtual displays
    VIRTUAL_DISPLAY_STATUS = 0x1F, // Host -> Viewer: Status, geometry, and backend report

    // Remote Input Injection
    INPUT_MOUSE_MOVE    = 0x20,
    INPUT_MOUSE_BUTTON  = 0x21,
    INPUT_MOUSE_WHEEL   = 0x22,
    INPUT_KEY_EVENT     = 0x23,
    INPUT_RELEASE_ALL   = 0x24, // Release all pressed modifier keys on focus loss / tab switch
    SYSTEM_ACTION       = 0x25, // Trigger remote system action (Task Manager, Show Desktop, Lock PC, SAS, Reboot)
    DIAGNOSTICS_REQ     = 0x26, // Viewer -> Host: Enable/Disable active diagnostics streaming
    SYSTEM_DIAGNOSTICS  = 0x27, // Host -> Viewer: Hardware telemetry & top processes
    PROCESS_KILL        = 0x28, // Viewer -> Host: Request process termination
    VOICE_INTERCOM_CHUNK= 0x29, // Bidirectional: VoIP microphone audio stream chunk
    RESOLUTION_CHANGE_REQ=0x2A, // Viewer -> Host: Request host resolution change or aspect fit
    PERFORMANCE_HUD_METRICS=0x2B, // Host -> Viewer: Real-time capture & encode latency telemetry
    AUDIO_CONTROL          = 0x2C, // Viewer -> Host: Mute state & volume percentage sync

    // Clipboard, File Transfer & Live Chat
    CLIPBOARD_TEXT      = 0x30,
    FILE_OFFER          = 0x31,
    FILE_ACCEPT         = 0x32,
    FILE_CHUNK          = 0x33,
    FILE_COMPLETE       = 0x34,
    FILE_CANCEL         = 0x35,
    CHAT_MESSAGE        = 0x36,
    CLIPBOARD_FILE_LIST    = 0x37, // Host <-> Viewer: Virtual files metadata list on copy
    CLIPBOARD_FILE_REQUEST = 0x38, // Target -> Source: On-demand request for specific file chunk
    CLIPBOARD_FILE_CHUNK   = 0x39, // Source -> Target: Chunk payload with SHA-256 block hash
    CLIPBOARD_FILE_CANCEL  = 0x3A, // Bidirectional: Cancel active clipboard transfer & purge staging
    CHAT_MEDIA_MESSAGE     = 0x3B, // Bidirectional: Rich chat image/media attachment chunk
    REMOTE_REBOOT_REQUEST  = 0x3C, // Viewer -> Host: Request reboot (Normal or Safe Mode)
    REMOTE_REBOOT_CONFIRM  = 0x3D, // Host -> Viewer: Reboot ack with resume token & countdown
    REMOTE_REBOOT_RECONNECT= 0x3E, // Viewer -> Host: Reconnect handshake with resume token

    // Remote File Synchronization & Folder Mirroring
    SYNC_SCAN_REQ           = 0x40, // Requester -> Responder: Scan directory tree
    SYNC_SCAN_RESP          = 0x41, // Responder -> Requester: Directory tree file entries
    SYNC_HASH_REQ           = 0x42, // Source -> Target: Request 64KB block hashes for file
    SYNC_HASH_RESP          = 0x43, // Target -> Source: Array of 32-byte block SHA-256 hashes
    SYNC_DELTA_BLOCK        = 0x44, // Source -> Target: 64KB block payload with offset and hash
    SYNC_STATUS_UPDATE      = 0x45, // Bidirectional: Sync progress and state telemetry
    SYNC_ACTION_REQ         = 0x46, // Requester -> Responder: CreateDir, DeleteFile, DeleteDir, Abort
    SYNC_ACTION_RESP        = 0x47, // Responder -> Requester: Action result acknowledgment

    // Peer Version Handshake & Upgrade Notice
    VERSION_UPGRADE_NOTICE  = 0x48, // Bidirectional: Notify peer that an upgrade is recommended

    // Breakthrough Power Features
    SCRATCHPAD_UPDATE       = 0x49, // Bidirectional: Live collaborative notepad synchronization
    COMMAND_PALETTE_EXEC    = 0x4A, // Viewer -> Host: Execute remote administrative omnibar action
    COMMAND_PALETTE_OUTPUT  = 0x4B, // Host -> Viewer: Piped output stream from omnibar action

    // Relay & Rendezvous Protocol
    RELAY_REGISTER      = 0x50,
    RELAY_REGISTER_ACK  = 0x51,
    RELAY_LOOKUP        = 0x52,
    RELAY_LOOKUP_RESP   = 0x53,
    RELAY_CONNECT_REQ   = 0x54,
    RELAY_INCOMING_REQ  = 0x55,
    RELAY_BRIDGE_ACCEPT = 0x56,
    RELAY_BRIDGE_READY  = 0x57
};

enum class SystemActionType : uint8_t {
    TaskManager     = 1,
    ShowDesktop     = 2,
    LockWorkstation = 3,
    SendCtrlAltDel  = 4,
    EmergencyReboot = 5
};

enum class FileOfferTarget : uint8_t {
    DefaultDownloads = 0,
    Desktop          = 1,
    Custom           = 2
};

enum FrameFlags : uint8_t {
    FLAG_NONE      = 0x00,
    FLAG_ENCRYPTED = 0x01,
    FLAG_KEYFRAME  = 0x02
};

enum PermissionFlags : uint8_t {
    PERM_NONE          = 0x00,
    PERM_INPUT         = 0x01, // Mouse & Keyboard control
    PERM_CLIPBOARD     = 0x02, // Bidirectional clipboard sync
    PERM_FILE_TRANSFER = 0x04, // Send/receive files
    PERM_ALL           = PERM_INPUT | PERM_CLIPBOARD | PERM_FILE_TRANSFER
};

enum class QualityPreset : uint8_t {
    Ultra        = 0, // Lossless Zstd for UI + JPEG Q90, 60 FPS target
    Balanced     = 1, // Hybrid Zstd / JPEG Q75, 30 FPS target
    LowBandwidth = 2  // JPEG Q50, 15-20 FPS target
};

inline uint8_t clampTargetFps(uint8_t fps) {
    if (fps <= 20) return 15;
    if (fps <= 37) return 30;
    if (fps <= 52) return 45;
    return 60;
}

// One-Click Connection Quality Profiles
enum class ConnectionProfile : uint8_t {
    LowBandwidth      = 0, // 15 FPS, LowBandwidth preset (JPEG Q50), Adaptive FPS ON
    BalancedBandwidth = 1, // 30 FPS, Balanced preset (JPEG Q75/Zstd), Adaptive FPS ON
    HighBandwidth     = 2, // 45 FPS, Balanced preset (JPEG Q75/Zstd), Adaptive FPS ON
    VeryHighBandwidth = 3, // 60 FPS, Ultra preset (JPEG Q90/lossless UI), Adaptive FPS OFF
    Custom            = 4, // User-defined combination of FPS and Quality
    Balanced          = BalancedBandwidth,
    UltraLAN          = VeryHighBandwidth
};

inline const char* connectionProfileName(ConnectionProfile profile) {
    switch (profile) {
        case ConnectionProfile::LowBandwidth:      return "Low Bandwidth";
        case ConnectionProfile::BalancedBandwidth: return "Balanced Bandwidth";
        case ConnectionProfile::HighBandwidth:     return "High Bandwidth";
        case ConnectionProfile::VeryHighBandwidth: return "Very High Bandwidth";
        case ConnectionProfile::Custom:            return "Custom";
        default:                                   return "Balanced Bandwidth";
    }
}

inline void getProfileSettings(ConnectionProfile profile, QualityPreset& outQp, uint8_t& outFps, bool& outAdaptive) {
    switch (profile) {
        case ConnectionProfile::LowBandwidth:
            outQp = QualityPreset::LowBandwidth;
            outFps = 15;
            outAdaptive = true;
            break;
        case ConnectionProfile::HighBandwidth:
            outQp = QualityPreset::Balanced;
            outFps = 45;
            outAdaptive = true;
            break;
        case ConnectionProfile::VeryHighBandwidth:
            outQp = QualityPreset::Ultra;
            outFps = 60;
            outAdaptive = false;
            break;
        case ConnectionProfile::BalancedBandwidth:
        default:
            outQp = QualityPreset::Balanced;
            outFps = 30;
            outAdaptive = true;
            break;
    }
}

inline ConnectionProfile inferConnectionProfile(QualityPreset qp, uint8_t fps, bool adaptive) {
    if (qp == QualityPreset::LowBandwidth && fps <= 20 && adaptive) {
        return ConnectionProfile::LowBandwidth;
    }
    if (qp == QualityPreset::Balanced && fps > 20 && fps <= 37 && adaptive) {
        return ConnectionProfile::BalancedBandwidth;
    }
    if (qp == QualityPreset::Balanced && fps > 37 && fps <= 52 && adaptive) {
        return ConnectionProfile::HighBandwidth;
    }
    if (qp == QualityPreset::Ultra && fps > 52 && !adaptive) {
        return ConnectionProfile::VeryHighBandwidth;
    }
    return ConnectionProfile::Custom;
}


enum class ScaleMode : uint8_t {
    FitAspect  = 0,
    Stretch    = 1,
    Original   = 2,
    FillAspect = 3
};

enum class ViewerConnectionState : uint8_t {
    Disconnected    = 0,
    ResolvingId     = 1,
    ConnectingTcp   = 2,
    Authenticating  = 3,
    WaitingApproval = 4,
    Connected       = 5,
    Error           = 6,
    Reconnecting    = 7,
    WaitingTotp     = 8  // Pending user entry of 6-digit TOTP code
};

// Automatically drops effective FPS (60 -> 30 -> 15) when network RTT or TCP send duration indicates poor connection
inline uint8_t computeAdaptiveFpsCap(uint8_t userTargetFps, bool adaptiveEnabled, uint32_t rttMs, float avgSendMs) {
    uint8_t target = clampTargetFps(userTargetFps);
    if (!adaptiveEnabled) {
        return target;
    }
    if (rttMs >= 180 || avgSendMs >= 75.0f) {
        return 15;
    }
    if (rttMs >= 95 || avgSendMs >= 38.0f) {
        return std::min<uint8_t>(target, 30);
    }
    return target;
}

enum class TileEncoding : uint8_t {
    RawBGRA = 0,
    Zstd    = 1,
    Jpeg    = 2
};

enum class AuthResultCode : uint8_t {
    Accepted         = 0,
    RejectedByUser   = 1,
    InvalidPassword  = 2,
    HostBusy         = 3,
    ProtocolError    = 4,
    RateLimited      = 5,
    TotpRequired     = 0x20, // 32: Host unattended password accepted, 6-digit TOTP code required
    TotpInvalid      = 0x21  // 33: Submitted TOTP code was incorrect or replayed
};

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;       // PROTOCOL_MAGIC
    uint8_t  type;        // PacketType
    uint8_t  flags;       // FrameFlags
    uint32_t payloadSize; // Payload byte count
};

using CppDeskHeader = FrameHeader;

struct DiscoveryBeaconPacket {
    uint32_t magic;       // PROTOCOL_MAGIC
    uint8_t  packetKind;  // 1 = Announce, 2 = QueryById
    uint64_t deskId;      // 9-digit numeric ID
    uint64_t queryId;     // Target ID when packetKind == 2
    uint16_t tcpPort;     // Host listening port
    char     hostname[64];
};

struct TileHeader {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t  encoding;    // TileEncoding
    uint32_t dataSize;
};

struct AudioChunkHeader {
    uint32_t sampleRate;     // Target sample rate (e.g. 48000)
    uint8_t  channels;       // Channel count (e.g. 2 for stereo)
    uint8_t  bitsPerSample;  // e.g. 16
    uint8_t  isSilent;       // 1 = silent/zero payload, 0 = active PCM
    uint32_t sampleFrames;   // number of sample frames in this chunk
};

#pragma pack(push, 1)
struct AudioControlPayload {
    uint8_t isMuted;       // 1 = muted, 0 = unmuted
    uint8_t volumePercent; // 0 - 100
    uint8_t reserved[2];
};
#pragma pack(pop)

struct VoiceChunkHeader {
    uint32_t sampleRate;     // Target sample rate (e.g. 48000 or 16000)
    uint8_t  channels;       // 1 (mono) or 2 (stereo)
    uint8_t  bitsPerSample;  // 16
    uint8_t  flags;          // 0x01: isSilent/muted, 0x02: pushToTalk
    uint32_t sampleFrames;   // number of sample frames in this chunk
};

enum class ScreenBlankMode : uint8_t {
    CurtainOnly = 0, // Software privacy curtain window only
    DpmsOnly    = 1, // Hardware DPMS monitor standby only
    Unified     = 2  // Curtain window + Hardware DPMS monitor standby
};

struct PrivacyModePayload {
    uint8_t enable;          // 1 = engage, 0 = disengage
    uint8_t acknowledge;     // 0 = request, 1 = ACK confirmation
};

struct PrivacyModeConfigPayload {
    uint8_t enable;          // 1 = engage, 0 = disengage
    uint8_t acknowledge;     // 0 = request, 1 = ACK confirmation
    uint8_t showDeskId;      // 1 = show, 0 = hide
    uint8_t blankMode;       // ScreenBlankMode: CurtainOnly(0), DpmsOnly(1), Unified(2)
    char    customNotice[128]; // null-terminated UTF-8 notice text
    char    brandName[64];     // null-terminated UTF-8 branding name
};

struct CursorUpdatePacket {
    float   normX;
    float   normY;
    uint8_t visible;
};

struct PingPacket {
    uint64_t timestamp;
    uint32_t rttMs;
};

struct ResolutionChangePayload {
    uint32_t targetWidth;   // Desired width or 0 to restore original
    uint32_t targetHeight;  // Desired height or 0 to restore original
    uint8_t  mode;          // 0 = RestoreDefault, 1 = ExactMatch, 2 = BestFitAspect
};

struct TunnelOpenHeader {
    uint32_t tunnelId;
    uint16_t targetPort;
    uint8_t  flags;
};

struct TunnelDataHeader {
    uint32_t tunnelId;
    uint32_t dataLen;
};

struct TunnelCloseHeader {
    uint32_t tunnelId;
    uint8_t  reasonCode;     // 0 = Normal, 1 = Refused, 2 = Timeout/Reset
};

enum class TerminalStreamKind : uint8_t {
    StdoutChunk = 1,
    StdinInput  = 2,
    ResetShell  = 3,
    SwitchShell = 4
};

struct TerminalDataHeader {
    uint8_t  streamKind;     // TerminalStreamKind
    uint32_t textLen;
};

enum class WhiteboardTool : uint8_t {
    Pen          = 0,
    Highlighter  = 1,
    Arrow        = 2,
    Laser        = 3,
    LaserPointer = 3,
    ClearAll     = 4
};

enum class WhiteboardAction : uint8_t {
    BeginStroke  = 0,
    AppendPoints = 1,
    EndStroke    = 2,
    LaserUpdate  = 3,
    ClearCanvas  = 4
};

struct WhiteboardPoint {
    float x; // Normalized 0.0 to 1.0
    float y; // Normalized 0.0 to 1.0
};

struct ClipboardFileListHeader {
    uint32_t transferId;
    uint32_t fileCount;
    uint64_t totalBytes;
};

struct ClipboardFileDescHeader {
    uint32_t fileIndex;
    uint64_t fileSize;
    uint32_t fileAttributes;
    uint16_t nameLengthChars; // Number of wchar_t
};

struct ClipboardFileRequestHeader {
    uint32_t transferId;
    uint32_t fileIndex;
    uint64_t offset;
    uint32_t length;
};

struct ClipboardFileChunkHeader {
    uint32_t transferId;
    uint32_t fileIndex;
    uint64_t offset;
    uint32_t dataLength;
    uint8_t  sha256[32];
};

struct ClipboardFileCancelHeader {
    uint32_t transferId;
    uint32_t reasonCode; // 1 = user cancelled, 2 = size limit exceeded, 3 = IO error
};
#pragma pack(pop)

struct VirtualFileEntry {
    uint32_t     fileIndex = 0;
    std::wstring fileName;
    uint64_t     fileSize = 0;
    uint32_t     fileAttributes = 0x80; // FILE_ATTRIBUTE_NORMAL
};

struct WhiteboardStroke {
    uint32_t strokeId = 0;
    WhiteboardTool tool = WhiteboardTool::Pen;
    uint32_t colorRgba = 0xE50914FF; // Default Crimson Red
    float strokeWidth = 3.0f;
    std::vector<WhiteboardPoint> points;
};

struct MonitorDesc {
    int32_t     index = 0;
    int32_t     x = 0;
    int32_t     y = 0;
    int32_t     width = 1920;
    int32_t     height = 1080;
    bool        isPrimary = true;
    std::string name;
    bool        isVirtual = false;
    uint32_t    virtualId = 0;
};

struct EncodedTile {
    uint16_t             x = 0;
    uint16_t             y = 0;
    uint16_t             width = 0;
    uint16_t             height = 0;
    TileEncoding         encoding = TileEncoding::Zstd;
    std::vector<uint8_t> data;
};

// Binary serialization helper
class ByteWriter {
public:
    ByteWriter() : bufRef_(ownedBuf_) {}
    explicit ByteWriter(std::vector<uint8_t>& externalBuf) : bufRef_(externalBuf) {}
    explicit ByteWriter(std::vector<uint8_t>&& movableBuf) : ownedBuf_(std::move(movableBuf)), bufRef_(ownedBuf_) {}

    void writeU8(uint8_t v) { bufRef_.push_back(v); }
    void writeU16(uint16_t v) { writeBytes(&v, sizeof(v)); }
    void writeU32(uint32_t v) { writeBytes(&v, sizeof(v)); }
    void writeI32(int32_t v) { writeBytes(&v, sizeof(v)); }
    void writeU64(uint64_t v) { writeBytes(&v, sizeof(v)); }
    void writeF32(float v) { writeBytes(&v, sizeof(v)); }

    void writeString(const std::string& s) {
        uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.size(), 65535));
        writeU16(len);
        if (len > 0) {
            writeBytes(s.data(), len);
        }
    }

    void writeBytes(const void* ptr, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(ptr);
        bufRef_.insert(bufRef_.end(), p, p + len);
    }

    void reserve(size_t cap) { bufRef_.reserve(cap); }
    const std::vector<uint8_t>& buffer() const { return bufRef_; }
    std::vector<uint8_t> takeBuffer() { return std::move(bufRef_); }
    std::string toString() const {
        return std::string(reinterpret_cast<const char*>(bufRef_.data()), bufRef_.size());
    }

private:
    std::vector<uint8_t> ownedBuf_;
    std::vector<uint8_t>& bufRef_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size), pos_(0) {}
    explicit ByteReader(const std::vector<uint8_t>& vec) : data_(vec.data()), size_(vec.size()), pos_(0) {}

    bool hasRemaining(size_t n) const { return pos_ + n <= size_; }
    size_t remaining() const { return size_ - pos_; }

    std::vector<uint8_t> readBytesVector(size_t n) {
        require(n);
        std::vector<uint8_t> v(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return v;
    }

    uint8_t readU8() {
        require(1);
        return data_[pos_++];
    }

    uint16_t readU16() {
        uint16_t v = 0;
        readBytes(&v, sizeof(v));
        return v;
    }

    uint32_t readU32() {
        uint32_t v = 0;
        readBytes(&v, sizeof(v));
        return v;
    }

    int32_t readI32() {
        int32_t v = 0;
        readBytes(&v, sizeof(v));
        return v;
    }

    uint64_t readU64() {
        uint64_t v = 0;
        readBytes(&v, sizeof(v));
        return v;
    }

    float readF32() {
        float v = 0.0f;
        readBytes(&v, sizeof(v));
        return v;
    }

    std::string readString() {
        uint16_t len = readU16();
        require(len);
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return s;
    }

    void readBytes(void* out, size_t len) {
        require(len);
        if (len > 0) {
            std::memcpy(out, data_ + pos_, len);
            pos_ += len;
        }
    }

    const uint8_t* currentPtr() const { return data_ + pos_; }
    void skip(size_t len) {
        require(len);
        pos_ += len;
    }

private:
    void require(size_t n) const {
        if (n > size_ || pos_ > size_ - n) {
            throw std::runtime_error("Packet buffer underflow");
        }
    }

    const uint8_t* data_;
    size_t         size_;
    size_t         pos_;
};

struct ProcessTelemetryItem {
    uint32_t pid = 0;
    uint64_t workingSetBytes = 0;
    std::string name;
};

struct SystemDiagnosticsPayload {
    float cpuUsagePercent = 0.0f;
    uint64_t ramUsedBytes = 0;
    uint64_t ramTotalBytes = 0;
    uint64_t diskUsedBytes = 0;
    uint64_t diskTotalBytes = 0;
    std::vector<ProcessTelemetryItem> processes;

    // Extended Host Hardware Specs & Health Info
    std::string cpuModel;          // e.g. "Intel(R) Core(TM) i7-12700H" or "AMD Ryzen 7 5800X"
    std::string gpuModel;          // e.g. "NVIDIA GeForce RTX 4070 Laptop GPU"
    std::string osVersion;         // e.g. "Windows 11 (Build 22631)"
    uint32_t    cpuCores = 0;      // Number of logical processor cores
    uint64_t    uptimeSeconds = 0;  // System uptime in seconds
};

inline void serializeSystemDiagnostics(const SystemDiagnosticsPayload& payload, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeF32(payload.cpuUsagePercent);
    w.writeU64(payload.ramUsedBytes);
    w.writeU64(payload.ramTotalBytes);
    w.writeU64(payload.diskUsedBytes);
    w.writeU64(payload.diskTotalBytes);
    uint16_t count = static_cast<uint16_t>(std::min<size_t>(payload.processes.size(), 65535));
    w.writeU16(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& p = payload.processes[i];
        w.writeU32(p.pid);
        w.writeU64(p.workingSetBytes);
        w.writeString(p.name);
    }
    // Extended Hardware Specs
    w.writeString(payload.cpuModel);
    w.writeString(payload.gpuModel);
    w.writeString(payload.osVersion);
    w.writeU32(payload.cpuCores);
    w.writeU64(payload.uptimeSeconds);
    out = w.takeBuffer();
}

inline bool deserializeSystemDiagnostics(const uint8_t* data, size_t size, SystemDiagnosticsPayload& out) {
    if (!data || size < (4 + 8 * 4 + 2)) return false;
    try {
        ByteReader r(data, size);
        out.cpuUsagePercent = r.readF32();
        out.ramUsedBytes = r.readU64();
        out.ramTotalBytes = r.readU64();
        out.diskUsedBytes = r.readU64();
        out.diskTotalBytes = r.readU64();
        uint16_t count = r.readU16();
        out.processes.clear();
        out.processes.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            ProcessTelemetryItem item;
            item.pid = r.readU32();
            item.workingSetBytes = r.readU64();
            item.name = r.readString();
            out.processes.push_back(std::move(item));
        }
        // Extended Hardware Specs (backward-compatible check)
        if (r.remaining() > 0) {
            out.cpuModel = r.readString();
            out.gpuModel = r.readString();
            out.osVersion = r.readString();
            out.cpuCores = r.readU32();
            out.uptimeSeconds = r.readU64();
        } else {
            out.cpuModel.clear();
            out.gpuModel.clear();
            out.osVersion.clear();
            out.cpuCores = 0;
            out.uptimeSeconds = 0;
        }
        return true;
    } catch (...) {
        return false;
    }
}

struct PerformanceHudPayload {
    float    captureLatencyMs = 0.0f;
    float    encodeLatencyMs = 0.0f;
    uint32_t dirtyTilesCount = 0;
    float    compressionRatio = 1.0f;
    float    hostFps = 0.0f;
};

inline void serializePerformanceHud(const PerformanceHudPayload& payload, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeF32(payload.captureLatencyMs);
    w.writeF32(payload.encodeLatencyMs);
    w.writeU32(payload.dirtyTilesCount);
    w.writeF32(payload.compressionRatio);
    w.writeF32(payload.hostFps);
    out = w.takeBuffer();
}

inline bool deserializePerformanceHud(const uint8_t* data, size_t size, PerformanceHudPayload& out) {
    if (!data || size < (sizeof(float) * 4 + sizeof(uint32_t))) return false;
    try {
        ByteReader r(data, size);
        out.captureLatencyMs = r.readF32();
        out.encodeLatencyMs = r.readF32();
        out.dirtyTilesCount = r.readU32();
        out.compressionRatio = r.readF32();
        if (r.hasRemaining(sizeof(float))) {
            out.hostFps = r.readF32();
        } else {
            out.hostFps = 0.0f;
        }
        return true;
    } catch (...) {
        return false;
    }
}

struct ChatMediaPayload {
    std::string senderName;
    std::string captionText;
    uint32_t    imgWidth = 0;
    uint32_t    imgHeight = 0;
    uint64_t    timestampMs = 0;
    std::vector<uint8_t> jpegData;
};

inline void serializeChatMedia(const ChatMediaPayload& payload, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeString(payload.senderName);
    w.writeString(payload.captionText);
    w.writeU32(payload.imgWidth);
    w.writeU32(payload.imgHeight);
    w.writeU64(payload.timestampMs);
    w.writeU32(static_cast<uint32_t>(payload.jpegData.size()));
    if (!payload.jpegData.empty()) {
        w.writeBytes(payload.jpegData.data(), payload.jpegData.size());
    }
    out = w.takeBuffer();
}

inline bool deserializeChatMedia(const uint8_t* data, size_t size, ChatMediaPayload& out) {
    if (!data || size < (2 + 2 + 4 + 4 + 8 + 4)) return false;
    try {
        ByteReader r(data, size);
        out.senderName = r.readString();
        out.captionText = r.readString();
        out.imgWidth = r.readU32();
        out.imgHeight = r.readU32();
        out.timestampMs = r.readU64();
        uint32_t len = r.readU32();
        if (len > 0) {
            if (len > 10 * 1024 * 1024 || len > r.remaining()) return false;
            out.jpegData.resize(len);
            r.readBytes(out.jpegData.data(), len);
        } else {
            out.jpegData.clear();
        }
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeClipboardFileList(uint32_t transferId, const std::vector<VirtualFileEntry>& files, std::vector<uint8_t>& out) {
    ByteWriter w;
    ClipboardFileListHeader hdr{};
    hdr.transferId = transferId;
    hdr.fileCount = static_cast<uint32_t>(files.size());
    hdr.totalBytes = 0;
    for (const auto& f : files) hdr.totalBytes += f.fileSize;
    w.writeBytes(&hdr, sizeof(hdr));

    for (const auto& f : files) {
        ClipboardFileDescHeader desc{};
        desc.fileIndex = f.fileIndex;
        desc.fileSize = f.fileSize;
        desc.fileAttributes = f.fileAttributes;
        desc.nameLengthChars = static_cast<uint16_t>(f.fileName.size());
        w.writeBytes(&desc, sizeof(desc));
        if (desc.nameLengthChars > 0) {
            w.writeBytes(reinterpret_cast<const uint8_t*>(f.fileName.data()), desc.nameLengthChars * sizeof(wchar_t));
        }
    }
    out = w.takeBuffer();
}

inline bool deserializeClipboardFileList(const uint8_t* data, size_t size, uint32_t& outTransferId, std::vector<VirtualFileEntry>& outFiles) {
    if (!data || size < sizeof(ClipboardFileListHeader)) return false;
    try {
        ByteReader r(data, size);
        ClipboardFileListHeader hdr{};
        r.readBytes(&hdr, sizeof(hdr));
        outTransferId = hdr.transferId;
        outFiles.clear();
        outFiles.reserve(hdr.fileCount);

        for (uint32_t i = 0; i < hdr.fileCount; ++i) {
            if (!r.hasRemaining(sizeof(ClipboardFileDescHeader))) return false;
            ClipboardFileDescHeader desc{};
            r.readBytes(&desc, sizeof(desc));
            size_t nameBytes = static_cast<size_t>(desc.nameLengthChars) * sizeof(wchar_t);
            if (!r.hasRemaining(nameBytes)) return false;

            VirtualFileEntry entry{};
            entry.fileIndex = desc.fileIndex;
            entry.fileSize = desc.fileSize;
            entry.fileAttributes = desc.fileAttributes;
            if (desc.nameLengthChars > 0) {
                entry.fileName.resize(desc.nameLengthChars);
                std::memcpy(entry.fileName.data(), r.currentPtr(), nameBytes);
                r.skip(nameBytes);
            }
            outFiles.push_back(std::move(entry));
        }
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------- Remote Reboot & Auto-Reconnect Payloads ----------------

struct RemoteRebootRequestPayload {
    uint8_t  rebootMode = 0; // 0 = Normal, 1 = Safe Mode with Networking
    uint32_t countdownSeconds = 5;
};

struct RemoteRebootConfirmPayload {
    bool        accepted = true;
    uint32_t    countdownSeconds = 5;
    std::string resumeTokenHex;
    std::string message;
};

struct RemoteRebootReconnectPayload {
    uint64_t    callerDeskId = 0;
    std::string resumeTokenHex;
};

inline void serializeRemoteRebootRequest(const RemoteRebootRequestPayload& payload, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU8(payload.rebootMode);
    w.writeU32(payload.countdownSeconds);
    out = w.takeBuffer();
}

inline bool deserializeRemoteRebootRequest(const uint8_t* data, size_t size, RemoteRebootRequestPayload& out) {
    if (!data || size < (1 + 4)) return false;
    try {
        ByteReader r(data, size);
        out.rebootMode = r.readU8();
        out.countdownSeconds = r.readU32();
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeRemoteRebootConfirm(const RemoteRebootConfirmPayload& payload, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU8(payload.accepted ? 1 : 0);
    w.writeU32(payload.countdownSeconds);
    w.writeString(payload.resumeTokenHex);
    w.writeString(payload.message);
    out = w.takeBuffer();
}

inline bool deserializeRemoteRebootConfirm(const uint8_t* data, size_t size, RemoteRebootConfirmPayload& out) {
    if (!data || size < (1 + 4 + 2 + 2)) return false;
    try {
        ByteReader r(data, size);
        out.accepted = (r.readU8() != 0);
        out.countdownSeconds = r.readU32();
        out.resumeTokenHex = r.readString();
        out.message = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeRemoteRebootReconnect(const RemoteRebootReconnectPayload& payload, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU64(payload.callerDeskId);
    w.writeString(payload.resumeTokenHex);
    out = w.takeBuffer();
}

inline bool deserializeRemoteRebootReconnect(const uint8_t* data, size_t size, RemoteRebootReconnectPayload& out) {
    if (!data || size < (8 + 2)) return false;
    try {
        ByteReader r(data, size);
        out.callerDeskId = r.readU64();
        out.resumeTokenHex = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------- Virtual Multi-Display & Headless Emulation Protocol ----------------

enum class VirtualDisplayCmdType : uint8_t {
    Create  = 1,
    Destroy = 2,
    SetMode = 3
};

enum class VirtualDisplayStatusCode : uint8_t {
    Success          = 0,
    FailedGeneric    = 1,
    LimitReached     = 2,
    NotFound         = 3,
    PermissionDenied = 4,
    DriverError      = 5
};

struct VirtualDisplayCmdPayload {
    uint8_t  cmd = static_cast<uint8_t>(VirtualDisplayCmdType::Create);
    uint32_t displayId = 0;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t refreshRate = 60;
    uint8_t  flags = 0; // 0x01 = Prefer IddCx, 0x02 = Force Software
};

struct VirtualDisplayStatusPayload {
    uint8_t     statusCode = static_cast<uint8_t>(VirtualDisplayStatusCode::Success);
    uint32_t    displayId = 0;
    uint32_t    width = 1920;
    uint32_t    height = 1080;
    uint32_t    refreshRate = 60;
    uint8_t     isVirtual = 1;
    uint8_t     isHeadlessFallback = 0;
    uint8_t     driverBackend = 0; // 0 = Software, 1 = IddCx
    std::string message;
};

inline void serializeVirtualDisplayCmd(const VirtualDisplayCmdPayload& payload, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU8(payload.cmd);
    w.writeU32(payload.displayId);
    w.writeU32(payload.width);
    w.writeU32(payload.height);
    w.writeU32(payload.refreshRate);
    w.writeU8(payload.flags);
    out = w.takeBuffer();
}

inline bool deserializeVirtualDisplayCmd(const uint8_t* data, size_t size, VirtualDisplayCmdPayload& out) {
    if (!data || size < 18) return false;
    try {
        ByteReader r(data, size);
        out.cmd = r.readU8();
        out.displayId = r.readU32();
        out.width = r.readU32();
        out.height = r.readU32();
        out.refreshRate = r.readU32();
        out.flags = r.readU8();
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeVirtualDisplayStatus(const VirtualDisplayStatusPayload& payload, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU8(payload.statusCode);
    w.writeU32(payload.displayId);
    w.writeU32(payload.width);
    w.writeU32(payload.height);
    w.writeU32(payload.refreshRate);
    w.writeU8(payload.isVirtual);
    w.writeU8(payload.isHeadlessFallback);
    w.writeU8(payload.driverBackend);
    w.writeString(payload.message);
    out = w.takeBuffer();
}

inline bool deserializeVirtualDisplayStatus(const uint8_t* data, size_t size, VirtualDisplayStatusPayload& out) {
    if (!data || size < 20) return false;
    try {
        ByteReader r(data, size);
        out.statusCode = r.readU8();
        out.displayId = r.readU32();
        out.width = r.readU32();
        out.height = r.readU32();
        out.refreshRate = r.readU32();
        out.isVirtual = r.readU8();
        out.isHeadlessFallback = r.readU8();
        out.driverBackend = r.readU8();
        if (r.hasRemaining(2)) {
            out.message = r.readString();
        } else {
            out.message.clear();
        }
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializePrivacyModeConfig(const PrivacyModeConfigPayload& payload, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeBytes(&payload, sizeof(payload));
    out = w.takeBuffer();
}

inline bool deserializePrivacyModeConfig(const uint8_t* data, size_t size, PrivacyModeConfigPayload& out) {
    if (!data || size < sizeof(PrivacyModeConfigPayload)) return false;
    std::memcpy(&out, data, sizeof(PrivacyModeConfigPayload));
    out.customNotice[sizeof(out.customNotice) - 1] = '\0';
    out.brandName[sizeof(out.brandName) - 1] = '\0';
    return true;
}

inline void serializeMonitorList(const std::vector<MonitorDesc>& monitors, std::vector<uint8_t>& out) {
    ByteWriter w;
    uint16_t mcount = static_cast<uint16_t>(std::min<size_t>(monitors.size(), 65535));
    w.writeU16(mcount);
    for (size_t i = 0; i < mcount; ++i) {
        const auto& m = monitors[i];
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
    out = w.takeBuffer();
}

inline bool deserializeMonitorList(const uint8_t* data, size_t size, std::vector<MonitorDesc>& out) {
    if (!data || size < 2) return false;
    try {
        ByteReader r(data, size);
        uint16_t mcount = r.readU16();
        out.clear();
        out.reserve(mcount);
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
            } else {
                m.isVirtual = false;
                m.virtualId = 0;
            }
            out.push_back(std::move(m));
        }
        return true;
    } catch (...) {
        return false;
    }
}

struct TotpVerifyPacket {
    std::string totpCode; // 6-digit decimal string
};

inline void serializeTotpVerify(const TotpVerifyPacket& packet, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeString(packet.totpCode);
    out = w.takeBuffer();
}

inline bool deserializeTotpVerify(const uint8_t* data, size_t size, TotpVerifyPacket& out) {
    if (!data || size < 2) return false;
    try {
        ByteReader r(data, size);
        out.totpCode = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

// Remote File Synchronization and Folder Mirroring Structs & Serializers

struct SyncFileEntry {
    std::string relativePath;  // Normalized canonical path with forward slashes (e.g. "bin/app.exe")
    uint64_t sizeBytes = 0;    // File size in bytes (0 for directories)
    uint64_t mtimeSec = 0;     // Unix epoch modification timestamp
    uint8_t isDirectory = 0;   // 1 = directory, 0 = regular file
};

struct SyncScanReqPayload {
    uint32_t scanId = 0;
    std::string rootPath;
    uint8_t flags = 0;         // 0x01 = Recursive, 0x02 = IncludeHidden
};

struct SyncScanRespPayload {
    uint32_t scanId = 0;
    uint8_t statusCode = 0;    // 0 = Success, 1 = DirectoryNotFound, 2 = AccessDenied, 3 = Error
    std::string rootPath;
    std::vector<SyncFileEntry> entries;
};

struct SyncHashReqPayload {
    uint32_t syncId = 0;
    uint32_t fileId = 0;
    std::string relativePath;
};

struct SyncHashRespPayload {
    uint32_t syncId = 0;
    uint32_t fileId = 0;
    uint64_t targetFileSize = 0;
    std::vector<std::array<uint8_t, 32>> blockHashes;
};

struct SyncDeltaBlockPayload {
    uint32_t syncId = 0;
    uint32_t fileId = 0;
    uint32_t blockIndex = 0;
    uint64_t blockOffset = 0;
    uint64_t totalFileSize = 0;
    uint32_t totalBlocks = 0;
    std::array<uint8_t, 32> blockHash{};
    std::vector<uint8_t> blockData;
};

struct SyncStatusUpdatePayload {
    uint32_t syncId = 0;
    uint8_t syncState = 0;       // 0=Idle, 1=Scanning, 2=DiffReady, 3=Syncing, 4=Completed, 5=Failed, 6=Cancelled
    uint64_t transferredBytes = 0;
    uint64_t totalBytes = 0;
    uint32_t currentFileIndex = 0;
    uint32_t totalFiles = 0;
    std::string currentFileName;
    std::string errorMessage;
};

#ifdef DeleteFile
#undef DeleteFile
#endif

enum class SyncActionType : uint8_t {
    CreateDir   = 1,
    DeleteFile  = 2,
    DeleteDir   = 3,
    AbortSync   = 4,
    RequestFile = 5
};

#ifdef DeleteFile
#undef DeleteFile
#endif

struct SyncActionReqPayload {
    uint32_t actionId = 0;
    uint8_t actionType = 0;
    std::string targetPath;
};

struct SyncActionRespPayload {
    uint32_t actionId = 0;
    uint8_t actionType = 0;
    uint8_t statusCode = 0;     // 0 = Success, 1 = Failed
    std::string message;
};

inline void serializeSyncScanReq(const SyncScanReqPayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.scanId);
    w.writeString(p.rootPath);
    w.writeU8(p.flags);
    out = w.takeBuffer();
}

inline bool deserializeSyncScanReq(const uint8_t* data, size_t size, SyncScanReqPayload& out) {
    if (!data || size < 5) return false;
    try {
        ByteReader r(data, size);
        out.scanId = r.readU32();
        out.rootPath = r.readString();
        out.flags = r.readU8();
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeSyncScanResp(const SyncScanRespPayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.scanId);
    w.writeU8(p.statusCode);
    w.writeString(p.rootPath);
    w.writeU32(static_cast<uint32_t>(p.entries.size()));
    for (const auto& entry : p.entries) {
        w.writeString(entry.relativePath);
        w.writeU64(entry.sizeBytes);
        w.writeU64(entry.mtimeSec);
        w.writeU8(entry.isDirectory);
    }
    out = w.takeBuffer();
}

inline bool deserializeSyncScanResp(const uint8_t* data, size_t size, SyncScanRespPayload& out) {
    if (!data || size < 9) return false;
    try {
        ByteReader r(data, size);
        out.scanId = r.readU32();
        out.statusCode = r.readU8();
        out.rootPath = r.readString();
        uint32_t count = r.readU32();
        out.entries.clear();
        out.entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            SyncFileEntry e;
            e.relativePath = r.readString();
            e.sizeBytes = r.readU64();
            e.mtimeSec = r.readU64();
            e.isDirectory = r.readU8();
            out.entries.push_back(std::move(e));
        }
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeSyncHashReq(const SyncHashReqPayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.syncId);
    w.writeU32(p.fileId);
    w.writeString(p.relativePath);
    out = w.takeBuffer();
}

inline bool deserializeSyncHashReq(const uint8_t* data, size_t size, SyncHashReqPayload& out) {
    if (!data || size < 8) return false;
    try {
        ByteReader r(data, size);
        out.syncId = r.readU32();
        out.fileId = r.readU32();
        out.relativePath = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeSyncHashResp(const SyncHashRespPayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.syncId);
    w.writeU32(p.fileId);
    w.writeU64(p.targetFileSize);
    w.writeU32(static_cast<uint32_t>(p.blockHashes.size()));
    for (const auto& h : p.blockHashes) {
        w.writeBytes(h.data(), 32);
    }
    out = w.takeBuffer();
}

inline bool deserializeSyncHashResp(const uint8_t* data, size_t size, SyncHashRespPayload& out) {
    if (!data || size < 20) return false;
    try {
        ByteReader r(data, size);
        out.syncId = r.readU32();
        out.fileId = r.readU32();
        out.targetFileSize = r.readU64();
        uint32_t count = r.readU32();
        out.blockHashes.clear();
        out.blockHashes.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            r.readBytes(out.blockHashes[i].data(), 32);
        }
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeSyncDeltaBlock(const SyncDeltaBlockPayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.syncId);
    w.writeU32(p.fileId);
    w.writeU32(p.blockIndex);
    w.writeU64(p.blockOffset);
    w.writeU64(p.totalFileSize);
    w.writeU32(p.totalBlocks);
    w.writeBytes(p.blockHash.data(), 32);
    w.writeU32(static_cast<uint32_t>(p.blockData.size()));
    if (!p.blockData.empty()) {
        w.writeBytes(p.blockData.data(), p.blockData.size());
    }
    out = w.takeBuffer();
}

inline bool deserializeSyncDeltaBlock(const uint8_t* data, size_t size, SyncDeltaBlockPayload& out) {
    if (!data || size < (4 + 4 + 4 + 8 + 8 + 4 + 32 + 4)) return false;
    try {
        ByteReader r(data, size);
        out.syncId = r.readU32();
        out.fileId = r.readU32();
        out.blockIndex = r.readU32();
        out.blockOffset = r.readU64();
        out.totalFileSize = r.readU64();
        out.totalBlocks = r.readU32();
        r.readBytes(out.blockHash.data(), 32);
        uint32_t dataLen = r.readU32();
        out.blockData = r.readBytesVector(dataLen);
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeSyncStatusUpdate(const SyncStatusUpdatePayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.syncId);
    w.writeU8(p.syncState);
    w.writeU64(p.transferredBytes);
    w.writeU64(p.totalBytes);
    w.writeU32(p.currentFileIndex);
    w.writeU32(p.totalFiles);
    w.writeString(p.currentFileName);
    w.writeString(p.errorMessage);
    out = w.takeBuffer();
}

inline bool deserializeSyncStatusUpdate(const uint8_t* data, size_t size, SyncStatusUpdatePayload& out) {
    if (!data || size < (4 + 1 + 8 + 8 + 4 + 4 + 2 + 2)) return false;
    try {
        ByteReader r(data, size);
        out.syncId = r.readU32();
        out.syncState = r.readU8();
        out.transferredBytes = r.readU64();
        out.totalBytes = r.readU64();
        out.currentFileIndex = r.readU32();
        out.totalFiles = r.readU32();
        out.currentFileName = r.readString();
        out.errorMessage = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeSyncActionReq(const SyncActionReqPayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.actionId);
    w.writeU8(p.actionType);
    w.writeString(p.targetPath);
    out = w.takeBuffer();
}

inline bool deserializeSyncActionReq(const uint8_t* data, size_t size, SyncActionReqPayload& out) {
    if (!data || size < 5) return false;
    try {
        ByteReader r(data, size);
        out.actionId = r.readU32();
        out.actionType = r.readU8();
        out.targetPath = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

inline void serializeSyncActionResp(const SyncActionRespPayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.actionId);
    w.writeU8(p.actionType);
    w.writeU8(p.statusCode);
    w.writeString(p.message);
    out = w.takeBuffer();
}

inline bool deserializeSyncActionResp(const uint8_t* data, size_t size, SyncActionRespPayload& out) {
    if (!data || size < 6) return false;
    try {
        ByteReader r(data, size);
        out.actionId = r.readU32();
        out.actionType = r.readU8();
        out.statusCode = r.readU8();
        out.message = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

struct VersionUpgradeNoticePayload {
    std::string senderVersion;      // Version of connecting peer (e.g. "3.4.1")
    std::string minimumVersion;     // Recommended minimum version (e.g. "3.4.1")
    std::string downloadUrl;        // Release or asset download URL
    std::string message;            // Human-readable upgrade notice
};

inline void serializeVersionUpgradeNotice(const VersionUpgradeNoticePayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeString(p.senderVersion);
    w.writeString(p.minimumVersion);
    w.writeString(p.downloadUrl);
    w.writeString(p.message);
    out = w.takeBuffer();
}

inline bool deserializeVersionUpgradeNotice(const uint8_t* data, size_t size, VersionUpgradeNoticePayload& out) {
    if (!data || size < 8) return false;
    try {
        ByteReader r(data, size);
        out.senderVersion = r.readString();
        out.minimumVersion = r.readString();
        out.downloadUrl = r.readString();
        out.message = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------- Breakthrough Power Features Payloads ----------------

struct ScratchpadUpdatePayload {
    uint32_t revision = 0;        // Monotonically increasing revision counter
    uint64_t authorDeskId = 0;    // Desk ID of peer modifying text
    uint32_t cursorPosition = 0;  // Cursor offset
    uint8_t flags = 0;            // 0x01 = FullText, 0x02 = Snippet, 0x04 = Clear
    std::string text;             // Content string
};

inline void serializeScratchpadUpdate(const ScratchpadUpdatePayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.revision);
    w.writeU64(p.authorDeskId);
    w.writeU32(p.cursorPosition);
    w.writeU8(p.flags);
    w.writeString(p.text);
    out = w.takeBuffer();
}

inline bool deserializeScratchpadUpdate(const uint8_t* data, size_t size, ScratchpadUpdatePayload& out) {
    if (!data || size < 19) return false;
    try {
        ByteReader r(data, size);
        out.revision = r.readU32();
        out.authorDeskId = r.readU64();
        out.cursorPosition = r.readU32();
        out.flags = r.readU8();
        out.text = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

struct CommandPaletteExecPayload {
    uint32_t commandId = 0;       // Unique command execution identifier
    std::string commandKey;       // Catalog identifier (e.g. "net_flush_dns")
    std::string customCommand;    // Optional custom argument or command override
    uint8_t flags = 0;            // 0x01 = RequireElevated
};

inline void serializeCommandPaletteExec(const CommandPaletteExecPayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.commandId);
    w.writeString(p.commandKey);
    w.writeString(p.customCommand);
    w.writeU8(p.flags);
    out = w.takeBuffer();
}

inline bool deserializeCommandPaletteExec(const uint8_t* data, size_t size, CommandPaletteExecPayload& out) {
    if (!data || size < 9) return false;
    try {
        ByteReader r(data, size);
        out.commandId = r.readU32();
        out.commandKey = r.readString();
        out.customCommand = r.readString();
        out.flags = r.readU8();
        return true;
    } catch (...) {
        return false;
    }
}

struct CommandPaletteOutputPayload {
    uint32_t commandId = 0;
    uint32_t exitCode = 0;
    bool isCompleted = false;
    std::string stdoutData;
    std::string errorMessage;
};

inline void serializeCommandPaletteOutput(const CommandPaletteOutputPayload& p, std::vector<uint8_t>& out) {
    ByteWriter w;
    w.writeU32(p.commandId);
    w.writeU32(p.exitCode);
    w.writeU8(p.isCompleted ? 1 : 0);
    w.writeString(p.stdoutData);
    w.writeString(p.errorMessage);
    out = w.takeBuffer();
}

inline bool deserializeCommandPaletteOutput(const uint8_t* data, size_t size, CommandPaletteOutputPayload& out) {
    if (!data || size < 13) return false;
    try {
        ByteReader r(data, size);
        out.commandId = r.readU32();
        out.exitCode = r.readU32();
        out.isCompleted = (r.readU8() != 0);
        out.stdoutData = r.readString();
        out.errorMessage = r.readString();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace cppdesk
