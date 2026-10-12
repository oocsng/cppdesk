<p align="center">
  <img src="assets/icon.png" width="96" height="96" alt="CppDesk App Icon"><br>
  <b>CppDesk</b><br>
  <span>Fast, lightweight, standalone Remote Desktop for Windows</span><br><br>
  <a href="#features">Features</a> •
  <a href="#quick-start">Quick Start</a> •
  <a href="#how-to-build-from-source">Build</a> •
  <a href="#project-structure">Structure</a> •
  <a href="assets/CppDeskShowcase.mp4">Video Demo</a> •
  <a href="https://github.com/nmnghia2527/cppdesk/releases">Releases</a> •
  <a href="#issues--support">Issues</a>
</p>

<p align="center">
  <a href="https://github.com/nmnghia2527/cppdesk/releases/latest">
    <img src="https://img.shields.io/github/v/release/nmnghia2527/cppdesk?style=for-the-badge&color=D97757&label=DOWNLOAD%20CPPDESK&sort=semver&labelColor=262320" alt="Download CppDesk">
  </a>
  <a href="https://github.com/nmnghia2527/cppdesk/blob/main/LICENSE">
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
      <b>Watch Full Video (1080p 60 FPS • Audio & Voiceover)</b>
    </a>
  </sub>
</p>

---

CppDesk is a zero-install remote desktop application for Windows distributed as a single portable executable (**`CppDesk.exe`**). It works out of the box with no configuration or external dependencies required. You have full control of your connection and data, with end-to-end encrypted sessions.

Connect across networks using a **9-Digit Desk ID** (`XXX XXX XXX`) with a permanent password or interactive approval.

---

## Features

- **Screen Sharing**: Smooth, low-latency remote desktop streaming with multi-monitor switching, borderless fullscreen, and adjustable quality settings.
- **Display Matching**: Fit remote displays to eliminate black bars, or dynamically adapt the remote computer's resolution to match your local window.
- **Multi-Session Tabs**: Manage multiple simultaneous connections in clean browser-style tabs with live status indicators, unread chat counters, and fast keyboard shortcuts.
- **File & Folder Transfer**: Drag and drop files and complete folders directly onto the remote screen with real-time transfer tracking and integrity verification.
- **Voice Intercom & Audio**: Stream host system audio and talk back with real-time two-way voice communication and instant mute controls.
- **Session Recording**: Record remote sessions directly to seekable video files with one click.
- **Privacy Mode**: Blank out the remote screen and block local keyboard and mouse input while you work securely.
- **TCP Port Forwarding**: Access remote services securely through local ports with quick presets for Remote Desktop (RDP), SSH, Web, and VNC.
- **Remote Terminal**: Built-in command prompt and PowerShell console accessible directly from the side drawer for quick administrative tasks.
- **Screen Whiteboard**: Draw and annotate together in real time using pens, highlighters, arrows, and laser pointers.
- **Address Book**: Save and organize frequent computers with custom names, category tags, and instant search.
- **End-to-End Security**: Protected with encrypted handshakes, authenticated stream encryption, brute-force lockout protection, and security code verification.
- **Automatic Updates**: Built-in update notifications keeping your client secure and synchronized with new releases.

---

## Quick Start

### 1. Download & Run
Download **[`CppDesk.exe`](https://github.com/nmnghia2527/cppdesk/releases/latest)** (no installation required). Double-click to launch.

```powershell
.\CppDesk.exe
```

> **Testing locally?** You can launch multiple isolated instances on one PC:
> ```powershell
> .\CppDesk.exe --instance 1
> .\CppDesk.exe --instance 2
> ```

### 2. Connect
1. **On the Host PC**: Share your **9-Digit ID** (`XXX XXX XXX`) and one-time session code (or set a permanent password in Settings).
2. **On the Viewer PC**: Enter the Host's 9-digit ID, enter the password (or leave blank for interactive approval), and click **Connect**.

---

## How to Build from Source

### Prerequisites
- **Windows 10 / 11 (x64)**
- **C++20 compiler** (such as MSYS2 MinGW-w64)

### Build Commands
```powershell
# Compile standalone CppDesk.exe
powershell -ExecutionPolicy Bypass -File .\build.ps1

# Run the automated verification test suite
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Test
```

---

## Project Structure

```text
├── CppDesk.exe                   # Standalone portable Windows executable
├── build.ps1                     # Build and test automation script
├── src/
│   ├── capture/                  # Screen capture and display management
│   ├── control/                  # Input handling, session tabs, and whiteboard
│   ├── core/                     # Networking protocol, security, and address book
│   ├── media/                    # Voice intercom and session recording
│   ├── net/                      # Connection engine, audio streaming, and updater
│   ├── simd/                     # Performance acceleration
│   └── ui/                       # Direct2D user interface
├── relay-dotnet/                 # Optional self-hosted relay server
├── scripts/                      # Verification and benchmark scripts
└── tests/                        # Automated test suite
```

---

## Issues & Support

If you run into any bugs, unexpected behavior, connection issues, or have feature ideas:

- **Report a Bug or Request a Feature**: [Open a New Issue](https://github.com/nmnghia2527/cppdesk/issues/new)
- **Browse Existing Reports**: [GitHub Issues](https://github.com/nmnghia2527/cppdesk/issues)

When opening an issue, include:
- Windows version (e.g. Windows 11 23H2 x64)
- CppDesk version (e.g. v3.5.1)
- Connection topology (Direct LAN, STUN NAT traversal, or Relay)
- Steps to reproduce the issue along with any displayed status text or error messages

---

## License

Licensed under the [MIT License](LICENSE). Copyright (c) 2026 CppDesk.
