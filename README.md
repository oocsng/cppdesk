<p align="center">
  <img src="assets/icon.png" width="96" height="96" alt="CppDesk App Icon"><br>
  <b>CppDesk</b><br>
  <span>Portable remote desktop client and host for Windows, macOS, and Linux</span><br><br>
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
  <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20macOS%20%7C%20Linux-D97757?style=for-the-badge&logo=linux&logoColor=white&labelColor=262320" alt="Platform: Windows | macOS | Linux">
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

CppDesk is a portable remote desktop application built with modern C++ and hardware-accelerated rendering (Direct2D on Windows, Cocoa and Metal on macOS, X11 and MIT-SHM on Linux). It runs standalone with no installer, registry clutter, or external runtime requirements. Sessions authenticate via ephemeral or permanent keys and run end-to-end encrypted over direct LAN, NAT hole punching (STUN), or an optional relay server.

Hosts and clients identify each other with a 9-digit desk ID (`XXX XXX XXX`). Access can be granted with a password or through interactive on-screen approval.

## Features

- Screen streaming: Low-latency desktop capture using DXGI Desktop Duplication with GDI fallback on Windows, AVFoundation / CoreGraphics on macOS, and MIT-SHM / X11 on Linux; multi-monitor selection, dynamic resolution matching, and 4-tier quality presets (Low Bandwidth 15 FPS, Balanced 30 FPS, High Bandwidth 45 FPS, Ultra LAN 60 FPS).
- Multi-session management: Tabbed interface for controlling multiple remote machines at once, with live ping indicators and unread chat counters.
- File transfer and directory sync: Drag-and-drop file delivery with chunked progress tracking, SHA-256 integrity checks, and background bidirectional folder mirroring.
- Audio and voice intercom: Full-duplex microphone chat with live VU meters, device selection dropdowns, and remote system loopback sound streaming.
- Session recording and instant rewind: Record sessions directly into self-contained video files with an integrated in-app player, or scrub backwards through a rolling frame cache without interrupting the connection.
- Terminal, process control, and command palette: Built-in remote command prompt, PowerShell / bash execution, live task manager with process killing, and an omnibar command palette (`Ctrl+K`).
- Security and privacy: ECDH (NIST P-256) key exchange, AES-256-GCM AEAD authenticated encryption, optional TOTP 2FA (RFC 6238) with QR setup, brute-force rate limiting, curtain screen privacy mode, and real-time privacy cloak redaction zones.
- Collaboration tools: In-session screen drawing tools (pens, highlighters, pointers), shared clipboard sync, and a real-time collaborative scratchpad.
- Built-in networking tools: Port forwarding presets (RDP, SSH, VNC, HTTP), STUN diagnostics, and address book with quick tags.

## Quick start

### 1. Download and run
From the latest release:
- **Windows:** `CppDesk.exe`. No installation needed; double-click it.
- **Linux, any distro:** `CppDesk-x86_64.AppImage`. Allow it to run once (file properties → "Allow executing file as
  program", or `chmod +x CppDesk-x86_64.AppImage`), then double-click it.
- **Ubuntu / Debian:** `cppdesk_<version>_amd64.deb`. Double-click to install, or `sudo apt install ./cppdesk_*.deb`;
  CppDesk then appears in the app menu.

```powershell
# Windows
.\CppDesk.exe

# Linux
./CppDesk-x86_64.AppImage
```

To test connections locally on a single machine, open two isolated instances:

```powershell
.\CppDesk.exe --instance 1
.\CppDesk.exe --instance 2
```

Headless Linux host / service mode:
```bash
./CppDesk --daemon              # Headless background host
./CppDesk --host-only           # Run host engine only without UI
```

### 2. Connect
1. On the host: Note your 9-digit ID and one-time session password (or set a permanent password in settings).
2. On the viewer: Enter the host's 9-digit ID, type the password (or leave blank to prompt the host user for approval), and click Connect.

## Building from source

### Requirements
- Windows 10 or 11 (x64), macOS 12+, or Linux (X11 desktop)
- C++20 compiler (such as MinGW-w64 from MSYS2, MSVC, Apple Clang, or GCC 11+)
- CMake 3.20+ (for cross-platform builds)

On Linux (Debian / Ubuntu / derivatives):
```bash
sudo apt install build-essential cmake libx11-dev libxext-dev libxtst-dev libxrandr-dev libzstd-dev libjpeg-turbo8-dev libssl-dev libcairo2-dev libpango1.0-dev pkg-config
```

The Linux app is the full interface on X11 (XWayland sessions included), drawn with Cairo and Pango. It uses these
programs when they are installed and works without them:
- `zenity` or `kdialog` for open-file and choose-folder dialogs
- `xdg-open` for links and "show in folder"
- `notify-send` for desktop notifications
- `fonts-noto-cjk` and `fonts-noto-color-emoji` to show Chinese, Japanese, Korean and emoji text

The tray icon needs a panel with a system tray. GNOME has none without the AppIndicator extension; there the
"minimize to tray" option is hidden and closing the window quits.

### Build commands

```powershell
# Build standalone CppDesk.exe via PowerShell (Windows)
powershell -ExecutionPolicy Bypass -File .\build.ps1

# Run the automated test suite (Windows)
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Test

# Or build via CMake (Windows, macOS, Linux)
cmake -B build
cmake --build build --config Release

# Run automated tests (52 test suites)
./build/CppDeskTests
```

## Project structure

```text
├── CppDesk.exe                   # Standalone portable Windows executable
├── build.ps1                     # Windows build and test automation script
├── CMakeLists.txt                # Cross-platform CMake build configuration
├── src/
│   ├── capture/                  # Screen capture (DXGI, GDI, AVFoundation, X11 SHM), display management, rewind buffer
│   ├── control/                  # Input injection (SendInput, CGEvent, XTest), session tabs, whiteboard, scratchpad
│   ├── core/                     # Protocol definitions, crypto identity, and TOTP 2FA
│   ├── media/                    # Voice intercom, session recorder, and player
│   ├── net/                      # Socket engine, NAT traversal, and auto-updater
│   ├── simd/                     # Vectorized image conversion and diffing (AVX2 & scalar fallback)
│   └── ui/                       # Shared screens with Direct2D (Windows) and Cairo/X11 (Linux) backends; Cocoa (macOS)
├── relay-dotnet/                 # Optional self-hosted relay server (.NET 10)
├── scripts/                      # Verification and benchmark utilities
└── tests/                        # Automated unit and integration test suite (52 suites)
```

## Issues and support

If you hit a bug, connection failure, or want to suggest an improvement:

- [Open an issue](https://github.com/oocsng/cppdesk/issues/new)
- [Browse open reports](https://github.com/oocsng/cppdesk/issues)

Include the following in your report to help diagnose the issue:
- Operating system and version (for example, Windows 11 23H2 x64, macOS 14.5, Ubuntu 24.04 X11)
- CppDesk version (for example, v3.9.2)
- Connection type (Direct LAN, STUN NAT traversal, or Relay)
- Steps to reproduce, along with relevant log lines or status messages

## License

CppDesk is licensed under the [MIT License](LICENSE). Copyright (c) 2026 CppDesk.
