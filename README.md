<p align="center">
  <img src="assets/icon.png" width="96" height="96" alt="CppDesk App Icon"><br>
  <b>CppDesk</b><br>
  <span>Portable remote desktop client and host for Windows</span><br><br>
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
  <img src="https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011-D97757?style=for-the-badge&logo=windows&logoColor=white&labelColor=262320" alt="Platform: Windows">
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

CppDesk is a portable remote desktop application for Windows built with modern C++ and Direct2D. It ships as a single standalone executable (`CppDesk.exe`) with no installer, registry clutter, or external runtime requirements. Sessions authenticate via ephemeral or permanent keys and run end-to-end encrypted over direct LAN, NAT hole punching (STUN), or an optional relay server.

Hosts and clients identify each other with a 9-digit desk ID (`XXX XXX XXX`). Access can be granted with a password or through interactive on-screen approval.

## Features

- Screen streaming: Low-latency desktop capture using DXGI Desktop Duplication with GDI fallback, multi-monitor selection, resolution matching, and manual or adaptive FPS targets (15 to 60 FPS).
- Multi-session management: Tabbed interface for controlling multiple remote machines at once, with live ping indicators and unread chat counters.
- File transfer: Drag and drop files or directories straight onto the viewer window, with chunked progress tracking and SHA-256 integrity checks.
- Two-way voice intercom and system audio: Full-duplex microphone chat with WASAPI/WinMM device selection, VU meters, and remote system sound streaming.
- Session recording and instant rewind: Record sessions directly into self-contained video files, or scrub backwards through a rolling frame cache without interrupting the live connection.
- Terminal and process control: Built-in remote command prompt, PowerShell execution, and a live task manager with process killing and resource monitoring.
- Security: X25519 key exchange, ChaCha20-Poly1305 authenticated encryption, optional TOTP 2FA, brute-force rate limiting, and privacy mode (blank screen with remote input lock).
- Whiteboard and collaboration: In-session screen drawing tools with pens, highlighters, pointers, and shared clipboard sync.
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
- Windows 10 or 11 (x64)
- C++20 compiler (such as MinGW-w64 from MSYS2 or LLVM/Clang)

### Build commands

```powershell
# Build standalone CppDesk.exe
powershell -ExecutionPolicy Bypass -File .\build.ps1

# Run the test suite
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Test
```

## Project structure

```text
├── CppDesk.exe                   # Standalone portable Windows executable
├── build.ps1                     # Build and test automation script
├── src/
│   ├── audio/                    # WASAPI audio capture and playback
│   ├── capture/                  # DXGI/GDI screen capture and display management
│   ├── control/                  # Input injection, session tabs, and whiteboard
│   ├── core/                     # Protocol definitions, crypto identity, and settings
│   ├── media/                    # Voice intercom, frame playback, and recording
│   ├── net/                      # Socket engine, NAT traversal, and auto-updater
│   ├── simd/                     # Vectorized image conversion and diffing
│   └── ui/                       # Direct2D rendering and window management
├── relay-dotnet/                 # Optional self-hosted relay server (.NET 8)
├── scripts/                      # Verification and benchmark utilities
└── tests/                        # Automated unit and integration test suite
```

## Issues and support

If you hit a bug, connection failure, or want to suggest an improvement:

- [Open an issue](https://github.com/oocsng/cppdesk/issues/new)
- [Browse open reports](https://github.com/oocsng/cppdesk/issues)

Include the following in your report to help diagnose the issue:
- Windows build version (for example, Windows 11 23H2 x64)
- CppDesk version (for example, v3.5.3)
- Connection type (Direct LAN, STUN NAT traversal, or Relay)
- Steps to reproduce, along with relevant log lines or status messages

## License

CppDesk is licensed under the [MIT License](LICENSE). Copyright (c) 2026 CppDesk.
