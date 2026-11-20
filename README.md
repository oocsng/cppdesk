<p align="center">
  <img src="assets/icon.png" width="96" height="96" alt="CppDesk App Icon"><br>
  <b>CppDesk</b><br>
  <span>Portable remote desktop for Windows, Linux and macOS. No server, no account, no installer.</span><br><br>
  <a href="https://oocsng.github.io/cppdesk/">Website</a> •
  <a href="#download">Download</a> •
  <a href="#features">Features</a> •
  <a href="#quick-start">Quick start</a> •
  <a href="#building-from-source">Build</a> •
  <a href="#documentation">Docs</a> •
  <a href="assets/CppDeskShowcase.mp4">Video demo</a> •
  <a href="https://github.com/oocsng/cppdesk/issues">Issues</a>
</p>

<p align="center">
  <a href="https://github.com/oocsng/cppdesk/releases/latest">
    <img src="https://img.shields.io/github/v/release/oocsng/cppdesk?style=for-the-badge&color=D97757&label=DOWNLOAD%20CPPDESK&sort=semver&labelColor=262320" alt="Download CppDesk">
  </a>
  <a href="https://github.com/oocsng/cppdesk/actions/workflows/ci.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/oocsng/cppdesk/ci.yml?branch=main&style=for-the-badge&label=CI&labelColor=262320" alt="CI status">
  </a>
  <a href="https://github.com/oocsng/cppdesk/blob/main/LICENSE">
    <img src="https://img.shields.io/badge/License-Apache%202.0-D97757?style=for-the-badge&labelColor=262320" alt="License: Apache 2.0">
  </a>
  <img src="https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-D97757?style=for-the-badge&logo=linux&logoColor=white&labelColor=262320" alt="Platform: Windows | Linux | macOS">
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

CppDesk is a remote desktop client and host built with modern C++ and native rendering (Direct2D on Windows, CoreGraphics on macOS, Cairo/Pango on X11 on Linux). It runs standalone, with no installer, registry clutter or external runtime. Sessions authenticate with ephemeral or permanent keys and are end-to-end encrypted.

It is built for devices on the same network: two machines on one LAN connect directly, with no server and no account. NAT hole punching (STUN) and a self-hosted relay exist for connecting across networks, but they are optional.

Each machine has a 9-digit desk ID (`XXX XXX XXX`). Access is granted with a password or by interactive approval on the host.

> [!CAUTION]
> Only connect to computers you own or have permission to control. The developers do not condone or support unauthorised access, and are not responsible for misuse.

## Download

Get the latest build from [Releases](https://github.com/oocsng/cppdesk/releases/latest).

| Platform | File | Notes |
| --- | --- | --- |
| Windows 10/11 (x64) | `CppDesk.exe` | Portable. Double-click it. |
| Linux, any distro | `CppDesk-x86_64.AppImage` | Allow it to run once (`chmod +x`), then double-click. |
| Ubuntu / Debian | `cppdesk_<version>_amd64.deb` | `sudo apt install ./cppdesk_*.deb`; CppDesk then appears in the app menu. |
| macOS 12.3+ | none yet | [Build from source](#building-from-source). |

The Windows and Linux files are not code-signed, so Windows SmartScreen may warn about an unknown publisher. See [SECURITY.md](SECURITY.md) for how updates are verified.

## Platform status

| Platform | As host | As viewer | State |
| --- | --- | --- | --- |
| Windows | Yes | Yes | The reference platform. Curtain screen privacy and privacy cloak are Windows-host only. |
| Linux, X11 (XWayland too) | Yes | Yes | Full interface. |
| Linux, Wayland | Experimental | Yes | Tested against a stand-in for the desktop service, **not on a real GNOME or KDE yet**. |
| macOS | Yes | Yes | Built and tested in CI, **not run on a real Mac yet**; no download. |

## Features

- Screen streaming: Low-latency desktop capture using DXGI Desktop Duplication with GDI fallback on Windows, ScreenCaptureKit / CoreGraphics on macOS, and MIT-SHM / X11 on Linux; multi-monitor selection, dynamic resolution matching, and 4 quality presets (Low Bandwidth 15 FPS, Balanced 30 FPS, High Bandwidth 45 FPS, Very High 60 FPS).
- Multi-session management: Tabbed interface for controlling multiple remote machines at once, with live ping indicators and unread chat counters.
- File transfer and directory sync: Drag-and-drop file delivery with chunked progress tracking, SHA-256 integrity checks, and background bidirectional folder mirroring.
- Audio and voice intercom: Full-duplex microphone chat with live VU meters, device selection dropdowns, and remote system loopback sound streaming.
- Session recording and instant rewind: Record a session to an uncompressed AVI file with an integrated in-app player (the files grow fast and recording stops at 4 GiB, so it suits short clips), or scrub backwards through a rolling frame cache without interrupting the connection.
- Terminal, process control, and command palette: Built-in remote command prompt, PowerShell / bash execution, live task manager with process killing, and an omnibar command palette (`Ctrl+Shift+P`).
- Security and privacy: ECDH (NIST P-256) key exchange, AES-256-GCM AEAD authenticated encryption, optional TOTP 2FA (RFC 6238) with QR setup, brute-force rate limiting, and, on Windows hosts only, curtain screen privacy mode and privacy cloak redaction zones.
- Collaboration tools: In-session screen drawing tools (pens, highlighters, pointers), shared clipboard sync (text, and files: copy them on one computer and paste them on the other, where they are fetched when you paste), and a real-time collaborative scratchpad.
- Built-in networking tools: Port forwarding presets (RDP, SSH, VNC, web), STUN and relay diagnostics (for connecting across networks), and address book with quick tags.

## Quick start

### 1. Run it
Start the file you downloaded:

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

### 3. Across networks (optional)
Two machines on one LAN need nothing more. To connect across networks you can run your own relay server (needs the .NET 10 SDK):

```bash
dotnet run --project relay-dotnet -c Release -- --port 50999 --auth-key <secret>
```

The key can also come from the `CPPDESK_RELAY_AUTH_KEY` environment variable. On each machine, open Settings > Network & Relay and enter the relay's address and the same key. There is no public relay.

## Building from source

### Requirements
- Windows 10 or 11 (x64), macOS 12.3+, or Linux (X11 desktop; Wayland screen sharing is experimental)
- C++20 compiler (such as MinGW-w64 from MSYS2, MSVC, Apple Clang, or GCC 11+)
- CMake 3.20+ (for cross-platform builds)

On Linux (Debian / Ubuntu / derivatives):
```bash
sudo apt install build-essential cmake libx11-dev libxext-dev libxtst-dev libxrandr-dev libzstd-dev libjpeg-dev libssl-dev libcairo2-dev libpango1.0-dev pkg-config
```

On macOS (Homebrew):
```bash
brew install cmake zstd openssl@3
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build
```

### Build commands

```powershell
# Build standalone CppDesk.exe via PowerShell (Windows)
powershell -ExecutionPolicy Bypass -File .\build.ps1

# Run the automated test suite (Windows)
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Test

# Or build via CMake (Windows, macOS, Linux)
cmake -B build
cmake --build build --config Release

# Run the automated tests
./build/CppDeskTests
```

## Platform notes

<details>
<summary><b>Linux (X11)</b>: optional helper programs and the tray icon</summary>

The Linux app is the full interface on X11 (XWayland sessions included), drawn with Cairo and Pango. It uses these
programs when they are installed and works without them:
- `zenity` or `kdialog` for open-file and choose-folder dialogs
- `xdg-open` for links and "show in folder"
- `notify-send` for desktop notifications
- `fonts-noto-cjk` and `fonts-noto-color-emoji` to show Chinese, Japanese, Korean and emoji text

The tray icon needs a panel with a system tray. GNOME has none without the AppIndicator extension; there the
"minimize to tray" option is hidden and closing the window quits.

</details>

<details>
<summary><b>Sharing a Wayland desktop</b> (experimental)</summary>

Ubuntu and Fedora log in to Wayland by default, where an X11 program cannot see or control the real desktop. On a
Wayland session the host therefore uses the desktop's own screen-sharing permission, with the video coming through
PipeWire. When a viewer connects, the desktop asks once whether to share the screen and allow remote control; nothing is
shared until you say yes, and saying no ends the session: the viewer is disconnected with the reason, and you get a
message and a note in Settings. The same happens when the desktop stops the sharing later. Untick remote control in that question and the viewer can see the screen but not control it. A desktop with a
newer portal remembers the answer, so later sessions do not ask again (the desktop lists remembered permissions in its
own privacy settings, where they can be revoked; a viewer still has to pass CppDesk's own password or approval).

This has **not been run on a real Wayland desktop yet**: it is tested against a stand-in for the desktop's service, not
on GNOME or KDE. What to check by hand is in [docs/wayland-host-test-checklist.md](docs/wayland-host-test-checklist.md).
If the desktop has no screen-sharing service, or PipeWire is missing, the host falls back to the old X11 way, which shows
only X applications (log in with an Xorg session to share everything). Viewing other computers is not affected, and
clipboard sync on Wayland reaches only X applications. To build the video part you need the PipeWire headers
(`sudo apt install libpipewire-0.3-dev`; without them it is left out); at run time the app loads the system's PipeWire
only when a Wayland share starts, so it still starts on machines without it.

</details>

<details>
<summary><b>Sound</b>: voice intercom and host sound</summary>

The voice intercom (microphone and speaker) and the host's system sound work on Windows, Linux and macOS. The host's
sound is sent as plain 48 kHz stereo with no compression, which costs about 1.5 Mbit/s and suits a LAN. On Linux sound
goes through PulseAudio, or PipeWire's PulseAudio support, loaded only when needed, so the app still starts without it;
to build the sound part you need the PulseAudio headers (`sudo apt install libpulse-dev`; without them sound is left
out). A Mac host sends its sound on macOS 13 or later (macOS 12 shares the screen without sound). A Linux host sends
everything its speakers play, CppDesk's own playback included, so a viewer who talks to a Linux host hears their own
voice back about a quarter second late; a Mac host leaves its own sound out. When a viewer talks, the host's
microphone opens by itself, and the dashboard shows "Microphone is on: the viewer can hear this room" while it is open.
A viewer's mute and volume apply at once and again after a reconnect. Not done: compression, choosing a sound device on
Linux (the system default is used) and echo cancellation, which would also remove that echo. The Linux sound is tested
against a test sound server and the Mac sound has **not been run on a real Mac**; what to check by hand is in
[docs/audio-test-checklist.md](docs/audio-test-checklist.md).

</details>

<details>
<summary><b>macOS</b>: trying the untested build</summary>

The macOS app is built and tested on every push, its screens are drawn offscreen and checked, and a Mac can now be
viewed and controlled as well as view others: it captures the screen with ScreenCaptureKit and takes mouse and keyboard
through CGEvent. It has **not been run on a real Mac yet** and there is no Mac download. To try it, open the latest
successful run of the CI workflow on GitHub, download the `CppDesk-mac-untested` artifact, unzip it, run
`xattr -dr com.apple.quarantine CppDesk.app`, open the app, and turn on Screen Recording and Accessibility when the
card on the dashboard asks. macOS asks again after every new build, because the build is only ad-hoc signed. A remote Ctrl
key acts as Command on the Mac (so Ctrl+C from a Windows machine copies) and the Windows key as Control; a Mac viewer
cannot send a real Control key. What to check by hand is in [docs/mac-host-test-checklist.md](docs/mac-host-test-checklist.md).

</details>

## Project structure

```text
├── CppDesk.exe                   # Windows build output of build.ps1 (not in git; download it from Releases)
├── build.ps1                     # Windows build and test automation script
├── CMakeLists.txt                # Cross-platform CMake build configuration
├── src/
│   ├── capture/                  # Screen capture (DXGI, GDI, ScreenCaptureKit, X11 SHM), display management, rewind buffer
│   ├── control/                  # Input injection (SendInput, CGEvent, XTest), session tabs, whiteboard, scratchpad
│   ├── core/                     # Protocol definitions, crypto identity, and TOTP 2FA
│   ├── media/                    # Voice intercom, session recorder, and player
│   ├── net/                      # Socket engine, NAT traversal, and auto-updater
│   ├── simd/                     # Vectorized image conversion and diffing (AVX2 & scalar fallback)
│   └── ui/                       # Shared screens with Direct2D (Windows) and Cairo/X11 (Linux) backends; Cocoa (macOS)
├── relay-dotnet/                 # Optional self-hosted relay server (.NET 10)
├── scripts/                      # Verification and benchmark utilities
├── platform/                     # Linux desktop entry and packaging files, macOS Info.plist
├── docs/                         # Hand-test checklists for sound, Mac and Wayland hosts
├── site/                         # The landing page published to GitHub Pages
└── tests/                        # Automated unit and integration tests, and the UI golden files
```

## Documentation

- [CONTRIBUTING.md](CONTRIBUTING.md): build, test and pull request workflow
- [SECURITY.md](SECURITY.md): supported versions and how to report a vulnerability privately
- [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
- Hand-test checklists for hardware CI cannot cover: [sound](docs/audio-test-checklist.md), [Mac host](docs/mac-host-test-checklist.md), [Wayland host](docs/wayland-host-test-checklist.md)

## Issues and support

If you hit a bug, a connection failure, or want to suggest an improvement, [open an issue](https://github.com/oocsng/cppdesk/issues/new/choose) or [browse open reports](https://github.com/oocsng/cppdesk/issues). Report security problems privately, as described in [SECURITY.md](SECURITY.md).

Include in your report:
- Operating system and version (for example, Windows 11 23H2 x64, macOS 14.5, Ubuntu 24.04 X11)
- CppDesk version (for example, v1.0.0; it is shown in Settings > About & Data)
- Connection type (Direct LAN, STUN NAT traversal, or Relay)
- Steps to reproduce, along with relevant log lines or status messages

## License

CppDesk is licensed under the [Apache License 2.0](LICENSE). Copyright (c) 2026 CppDesk.
