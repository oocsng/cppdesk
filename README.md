<p align="center">
  <img src="assets/icon.png" width="96" height="96" alt="CppDesk App Icon"><br>
  <b>CppDesk</b><br>
  <span>Portable remote desktop client and host for Windows and macOS</span><br><br>
  <a href="#features">Features</a> •
  <a href="#quick-start">Quick start</a> •
  <a href="#building-from-source">Build</a> •
  <a href="#project-structure">Structure</a> •
  <a href="assets/CppDeskShowcase.mp4">Video demo</a> •
  <a href="https://github.com/oocsng/cppdesk/releases">Releases</a> •
  <a href="#issues-and-support">Issues</a>
</p>

<p align="center">
  <a href="https://github.com/oocsng/cppdesk/releases/latest">
    <img src="https://img.shields.io/github/v/release/oocsng/cppdesk?style=for-the-badge&color=D97757&label=DOWNLOAD%20CPPDESK&sort=semver&labelColor=262320" alt="Download CppDesk">
  </a>
  <a href="https://github.com/oocsng/cppdesk/blob/main/LICENSE">
    <img src="https://img.shields.io/badge/License-MIT-D97757?style=for-the-badge&labelColor=262320" alt="License: MIT">
  </a>
  <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20macOS-D97757?style=for-the-badge&logo=windows&logoColor=white&labelColor=262320" alt="Platform: Windows | macOS">
</p>

<p align="center">
  <a href="assets/CppDeskShowcase.mp4">
    <img src="assets/showcase.gif" alt="CppDesk Product Showcase Demo" width="860" style="max-width: 100%; border-radius: 12px; box-shadow: 0 10px 30px rgba(0,0,0,0.15);">
  </a>
  <br>
  <sub>
    <a href="assets/CppDeskShowcase.mp4">
      <b>Watch demo video (1080p 60 FPS with audio)</b>
    </a>
  </sub>
</p>

CppDesk is a portable remote desktop application built with modern C++ and hardware-accelerated rendering (Direct2D on Windows, Cocoa and Metal on macOS). It runs standalone with no installer, registry clutter, or external runtime requirements. Sessions authenticate via ephemeral or permanent keys and run end-to-end encrypted over direct LAN, NAT hole punching (STUN), or an optional relay server.

Hosts and clients identify each other with a 9-digit desk ID (`XXX XXX XXX`). Access can be granted with a password or through interactive on-screen approval.

## Features

- Screen streaming: Low-latency desktop capture using DXGI Desktop Duplication with GDI fallback on Windows (and AVFoundation / CoreGraphics on macOS), multi-monitor selection, dynamic resolution matching, and 4-tier quality presets (Low Bandwidth 15 FPS, Balanced 30 FPS, High Bandwidth 45 FPS, Ultra LAN 60 FPS).
- Multi-session management: Tabbed interface for controlling multiple remote machines at once, with live ping indicators and unread chat counters.
- File transfer and directory sync: Drag-and-drop file delivery with chunked progress tracking, SHA-256 integrity checks, and background bidirectional folder mirroring.
- Audio and voice intercom: Full-duplex microphone chat with live VU meters, device selection dropdowns, and remote system loopback sound streaming.
- Session recording and instant rewind: Record sessions directly into self-contained video files with an integrated in-app player, or scrub backwards through a rolling frame cache without interrupting the connection.
- Terminal, process control, and command palette: Built-in remote command prompt, PowerShell execution, live task manager with process killing, and an omnibar command palette (`Ctrl+K`).
- Security and privacy: ECDH (NIST P-256) key exchange, AES-256-GCM AEAD authenticated encryption, optional TOTP 2FA (RFC 6238) with QR setup, brute-force rate limiting, curtain screen privacy mode, and real-time privacy cloak redaction zones.
- Collaboration tools: In-session screen drawing tools (pens, highlighters, pointers), shared clipboard sync, and a real-time collaborative scratchpad.
- Built-in networking tools: Port forwarding presets (RDP, SSH, VNC, HTTP), STUN diagnostics, and address book with quick tags.

## Quick start

### 1. Download and run
Grab `CppDesk.exe` from the latest release. No installation needed.

```powershell
.\CppDesk.exe
```

To test connections locally on a single machine, open two isolated instances:

```powershell
.\CppDesk.exe --instance 1
.\CppDesk.exe --instance 2
```

### 2. Connect
1. On the host: Note your 9-digit ID and one-time session password (or set a permanent password in settings).
2. On the viewer: Enter the host's 9-digit ID, type the password (or leave blank to prompt the host user for approval), and click Connect.

## Building from source

### Requirements
- Windows 10 or 11 (x64) or macOS 12+
- C++20 compiler (such as MinGW-w64 from MSYS2, MSVC, or Apple Clang)
- CMake 3.20+ (for cross-platform builds)

### Build commands

```powershell
# Build standalone CppDesk.exe via PowerShell (Windows)
powershell -ExecutionPolicy Bypass -File .\build.ps1

# Run the automated test suite (Windows)
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Test

# Or build via CMake (Windows & macOS)
cmake -B build
cmake --build build --config Release
```

## Project structure

```text
├── CppDesk.exe                   # Standalone portable Windows executable
├── build.ps1                     # Windows build and test automation script
├── CMakeLists.txt                # Cross-platform CMake build configuration
├── src/
│   ├── capture/                  # Screen capture, display management, and rewind buffer
│   ├── control/                  # Input injection, session tabs, whiteboard, scratchpad, command palette
│   ├── core/                     # Protocol definitions, crypto identity, and TOTP 2FA
│   ├── media/                    # Voice intercom, session recorder, and player
│   ├── net/                      # Socket engine, NAT traversal, and auto-updater
│   ├── simd/                     # Vectorized image conversion and diffing (AVX2 & scalar fallback)
│   └── ui/                       # Direct2D (Windows) and Cocoa/Metal (macOS) interface
├── relay-dotnet/                 # Optional self-hosted relay server (.NET 10)
├── scripts/                      # Verification and benchmark utilities
└── tests/                        # Automated unit and integration test suite (51 suites)
```

## Issues and support

If you hit a bug, connection failure, or want to suggest an improvement:

- [Open an issue](https://github.com/oocsng/cppdesk/issues/new)
- [Browse open reports](https://github.com/oocsng/cppdesk/issues)

Include the following in your report to help diagnose the issue:
- Windows build version (for example, Windows 11 23H2 x64)
- CppDesk version (for example, v3.6.0)
- Connection type (Direct LAN, STUN NAT traversal, or Relay)
- Steps to reproduce, along with relevant log lines or status messages

## License

CppDesk is licensed under the [MIT License](LICENSE). Copyright (c) 2026 CppDesk.
