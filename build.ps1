param(
    [switch]$Test,
    [switch]$Run,
    [switch]$BuildRelay,
    [switch]$Benchmark
)

$ErrorActionPreference = "Stop"

$Gpp = "C:\msys64\ucrt64\bin\g++.exe"
if (-not (Test-Path $Gpp)) {
    $Gpp = "g++"
}

$CoreSources = @(
    "src/simd/simd_kernels.S",
    "src/core/crypto_identity.cpp",
    "src/capture/screen_capture.cpp",
    "src/control/input_injector.cpp",
    "src/control/shell_clipboard.cpp",
    "src/control/clipboard_file_manager.cpp",
    "src/control/whiteboard_manager.cpp",
    "src/control/file_sync_manager.cpp",
    "src/net/network_engine.cpp",
    "src/net/updater.cpp",
    "src/ui/notification_manager.cpp",
    "src/media/session_recorder.cpp",
    "src/media/session_recording_player.cpp",
    "src/media/voice_intercom.cpp",
    "src/capture/display_manager.cpp",
    "src/capture/virtual_display_manager.cpp",
    "src/capture/screen_blank_manager.cpp",
    "src/control/session_tab_manager.cpp",
    "src/control/windows_service_manager.cpp",
    "src/control/shortcut_manager.cpp",
    "src/core/totp_manager.cpp",
    "src/core/qr_matrix.cpp",
    "src/capture/session_rewind_buffer.cpp",
    "src/capture/privacy_cloak_manager.cpp",
    "src/control/scratchpad_manager.cpp",
    "src/control/command_palette.cpp"
)

$AppSources = $CoreSources + @(
    "src/ui/ui_window.cpp",
    "src/main.cpp"
)

$TestSources = $CoreSources + @(
    "tests/test_suite.cpp"
)

$CommonFlags = @(
    "-std=c++20",
    "-O2",
    "-mavx2",
    "-Wall",
    "-Wextra",
    "-DNOMINMAX",
    "-DUNICODE",
    "-D_UNICODE",
    "-static"
)

$Libs = @(
    "-lzstd",
    "-ld3d11",
    "-ldxgi",
    "-ld2d1",
    "-ldwrite",
    "-lgdiplus",
    "-lws2_32",
    "-ldwmapi",
    "-lbcrypt",
    "-lole32",
    "-loleaut32",
    "-luuid",
    "-lshlwapi",
    "-lcomdlg32",
    "-lgdi32",
    "-luser32",
    "-lshell32",
    "-ladvapi32",
    "-lwinmm",
    "-lwinhttp",
    "-lpsapi"
)

$Windres = "C:\msys64\ucrt64\bin\windres.exe"
if (-not (Test-Path $Windres)) {
    $Windres = "windres"
}

Write-Host "[1/3] Compiling CppDesk.exe (C++20 + x86-64 AVX2 Assembly)..." -ForegroundColor Cyan
& $Windres "src/cppdesk.rc" -O coff -o "src/cppdesk_res.o"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile Win32 icon resource src/cppdesk.rc"
}
& $Gpp @CommonFlags -mwindows @AppSources "src/cppdesk_res.o" -o "CppDesk.exe" @Libs
Remove-Item "src/cppdesk_res.o" -ErrorAction SilentlyContinue
if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile CppDesk.exe"
}
Write-Host "  -> Built CppDesk.exe ($((Get-Item 'CppDesk.exe').Length / 1KB -as [int]) KB)" -ForegroundColor Green

if ($BuildRelay -or $Test -or $Benchmark) {
    Write-Host "[2/3] Building C# (.NET 10) Standalone Relay Server..." -ForegroundColor Cyan
    & dotnet build "relay-dotnet/CppDeskRelay.csproj" -c Release --nologo -v q
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to build C# CppDeskRelay.csproj"
    }
    Write-Host "  -> Verified C# .NET 10 Relay Server (--self-test)..." -ForegroundColor Green
    & dotnet "relay-dotnet/bin/Release/net10.0/CppDeskRelay.dll" --port 51998 --self-test
    if ($LASTEXITCODE -ne 0) {
        throw "C# CppDeskRelay self-test failed"
    }
}

if ($Test) {
    Write-Host "[3/3] Compiling and running CppDeskTests.exe..." -ForegroundColor Cyan
    & $Gpp @CommonFlags @TestSources -o "CppDeskTests.exe" @Libs
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to compile CppDeskTests.exe"
    }
    & .\CppDeskTests.exe
    if ($LASTEXITCODE -ne 0) {
        throw "CppDeskTests.exe reported test failures!"
    }
}

if ($Benchmark) {
    Write-Host "[Polyglot Benchmark] Running Python 3.14 Benchmark & Stress Suite..." -ForegroundColor Cyan
    & python "scripts/benchmark_suite.py"
    if ($LASTEXITCODE -ne 0) {
        throw "Python polyglot benchmark suite failed"
    }
}

if ($Run) {
    Start-Process ".\CppDesk.exe"
}
