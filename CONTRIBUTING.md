# Contributing to CppDesk

Thank you for your interest in improving CppDesk. We welcome bug reports, documentation updates, and pull requests.

## Development Setup

### System Requirements

- Windows 10 or 11 (x64), macOS 12.3+, or Linux (X11 desktop)
- C++20 compliant compiler:
  - Windows: GCC 13+ (MSYS2 UCRT64 recommended) or MSVC 2022 v143+
  - macOS: Apple Clang 15+ or LLVM Clang 17+
  - Linux: GCC 11+ or Clang
- CMake 3.20 or newer
- .NET 10 SDK (only required if modifying `relay-dotnet`)

### Building and Testing

Run the automated test suite locally before submitting any changes:

```powershell
# Windows PowerShell build and full test suite
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Test

# Windows standalone executable build
powershell -ExecutionPolicy Bypass -File .\build.ps1

# Optional standalone relay server build and self-test
powershell -ExecutionPolicy Bypass -File .\build.ps1 -BuildRelay
```

For cross-platform CMake builds:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/CppDeskTests
```

On Linux the tests need an X server (`xvfb-run -a ./build/CppDeskTests` on a headless machine); `./build.sh --test` builds and runs them. The Linux packages you need are listed in the README.

The whole test suite must pass with zero failures before you open a pull request; CI runs it on Windows, Linux (Ubuntu 22.04 and the latest) and macOS. If you change a screen, the UI golden files in `tests/ui_goldens/` change too: regenerate them with `CPPDESK_UPDATE_UI_GOLDENS=1 ./build/CppDeskTests` and review the diff.

## Contribution Guidelines

1. **Keep diffs focused**: Make small, cohesive changes. Avoid mixing refactoring, formatting changes, and bug fixes into a single PR.
2. **Preserve existing style**: Follow modern C++20 conventions:
   - Use RAII for all system handles, sockets, and memory allocations.
   - Enforce bounds checks on all network deserializers and wire buffers.
   - Keep crypto operations failing closed with zero fallback keys.
3. **Add regression tests**: If fixing a bug or adding protocol fields, add a unit or integration test in `tests/test_suite.cpp`.
4. **No broken platforms**: Code must compile cleanly on Windows (MSYS2/MSVC), Linux and macOS.
5. **No binary bloat**: Do not commit build artifacts, temporary logs, or uncompressed media to git.

## Pull Request Workflow

1. Fork the repository and create a descriptive branch:
   ```bash
   git checkout -b fix/socket-reconnect-timeout
   ```
2. Implement your changes and verify that `build.ps1 -Test` passes.
3. Commit with an imperative commit message (e.g., `fix: handle socket timeout on reconnect`).
4. Push your branch to your fork and submit a pull request against the `main` branch.
5. Provide a clear summary in the pull request template describing the problem and your verification steps.
