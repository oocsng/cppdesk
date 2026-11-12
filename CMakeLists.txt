cmake_minimum_required(VERSION 3.20)
project(CppDesk VERSION 3.9.2 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Common core sources
set(CORE_SOURCES
    src/core/crypto_identity.cpp
    src/core/totp_manager.cpp
    src/core/qr_matrix.cpp
    src/net/network_engine.cpp
    src/net/updater.cpp
    src/capture/screen_capture.cpp
    src/capture/display_manager.cpp
    src/capture/virtual_display_manager.cpp
    src/capture/screen_blank_manager.cpp
    src/capture/session_rewind_buffer.cpp
    src/capture/privacy_cloak_manager.cpp
    src/control/input_injector.cpp
    src/control/shell_clipboard.cpp
    src/control/clipboard_file_manager.cpp
    src/control/remote_file_browser.cpp
    src/control/whiteboard_manager.cpp
    src/control/file_sync_manager.cpp
    src/control/session_tab_manager.cpp
    src/control/shortcut_manager.cpp
    src/control/scratchpad_manager.cpp
    src/control/command_palette.cpp
    src/media/session_recorder.cpp
    src/media/session_recording_player.cpp
    src/media/voice_intercom.cpp
    src/ui/notification_manager.cpp
    src/ui/ui_platform.cpp
    src/ui/recording_canvas.cpp
    src/ui/app_ui.cpp
    src/ui/theme.cpp
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
    list(APPEND CORE_SOURCES
        src/simd/simd_kernels.S
        src/control/windows_service_manager.cpp
        src/ui/canvas_d2d.cpp
    )

    add_compile_definitions(
        NOMINMAX
        UNICODE
        _UNICODE
    )

    set(PLATFORM_LIBS
        ws2_32
        bcrypt
        d3d11
        dxgi
        d2d1
        dwrite
        gdiplus
        dwmapi
        ole32
        oleaut32
        uuid
        shlwapi
        comdlg32
        gdi32
        user32
        shell32
        advapi32
        winmm
        winhttp
        psapi
        zstd
    )
elseif(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

    list(APPEND CORE_SOURCES
        src/simd/simd_fallback.cpp
    )

    find_library(LIB_ZSTD zstd REQUIRED)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZ_CORE QuartzCore REQUIRED)
    find_library(CORE_GRAPHICS CoreGraphics REQUIRED)
    find_library(APP_SERVICES ApplicationServices REQUIRED)
    find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
    find_library(IMAGE_IO ImageIO REQUIRED)

    set(PLATFORM_LIBS
        ${CORE_GRAPHICS}
        ${APP_SERVICES}
        ${AUDIO_TOOLBOX}
        ${SECURITY_FRAMEWORK}
        ${CORE_FOUNDATION}
        ${IMAGE_IO}
        ${LIB_ZSTD}
        pthread
    )
elseif(UNIX AND NOT APPLE)
    list(APPEND CORE_SOURCES
        src/simd/simd_fallback.cpp
    )

    find_package(X11 REQUIRED)
    find_package(JPEG QUIET)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
    pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
    find_package(OpenSSL REQUIRED)
    find_library(LIB_ZSTD zstd REQUIRED)
    find_library(LIB_XTST Xtst)
    find_library(LIB_XRANDR Xrandr)
    find_library(LIB_XEXT Xext)

    list(APPEND CORE_SOURCES
        src/ui/canvas_cairo.cpp
        src/control/x11_clipboard.cpp
    )

    set(PLATFORM_LIBS
        ${LIB_ZSTD}
        OpenSSL::Crypto
        PkgConfig::CAIRO
        PkgConfig::PANGOCAIRO
        pthread
        dl
    )
    if(X11_FOUND)
        list(APPEND PLATFORM_LIBS ${X11_LIBRARIES})
    endif()
    if(LIB_XEXT)
        list(APPEND PLATFORM_LIBS ${LIB_XEXT})
    endif()
    if(LIB_XTST)
        list(APPEND PLATFORM_LIBS ${LIB_XTST})
    endif()
    if(LIB_XRANDR)
        list(APPEND PLATFORM_LIBS ${LIB_XRANDR})
    endif()
    if(JPEG_FOUND)
        list(APPEND PLATFORM_LIBS ${JPEG_LIBRARIES})
    endif()
else()
    list(APPEND CORE_SOURCES
        src/simd/simd_fallback.cpp
    )
    find_library(LIB_ZSTD zstd REQUIRED)
    set(PLATFORM_LIBS
        ${LIB_ZSTD}
        pthread
    )
endif()

# The Linux window and its embedded app icon (assets/icon.png as a byte array).
if(UNIX AND NOT APPLE)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png ICON_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ICON_BYTES "${ICON_HEX}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/app_icon_png.cpp
        "#include <cstddef>\nextern const unsigned char kAppIconPng[] = {${ICON_BYTES}};\n"
        "extern const std::size_t kAppIconPngSize = sizeof(kAppIconPng);\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png)
    set(LINUX_UI_SOURCES
        src/ui/ui_window_linux.cpp
        src/ui/x11_dnd.cpp
        src/ui/x11_tray.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/generated/app_icon_png.cpp
    )
endif()

# Automated test suite target
if(WIN32)
    add_executable(CppDeskTests
        ${CORE_SOURCES}
        tests/test_suite.cpp
    )
    target_link_libraries(CppDeskTests PRIVATE ${PLATFORM_LIBS})
elseif(UNIX AND NOT APPLE)
    add_executable(CppDeskTests
        ${CORE_SOURCES}
        ${LINUX_UI_SOURCES}
        tests/test_suite.cpp
    )
    target_link_libraries(CppDeskTests PRIVATE ${PLATFORM_LIBS})
endif()
if(TARGET CppDeskTests)
    # The UI goldens live in the source tree; CMake builds run the tests from the build directory.
    target_compile_definitions(CppDeskTests PRIVATE CPPDESK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Application executable
if(WIN32)
    enable_language(RC)
    add_executable(CppDesk
        ${CORE_SOURCES}
        src/ui/ui_window.cpp
        src/main.cpp
        src/cppdesk.rc
    )
    target_link_libraries(CppDesk PRIVATE ${PLATFORM_LIBS})
elseif(APPLE)
    add_executable(CppDesk MACOSX_BUNDLE
        ${CORE_SOURCES}
        src/ui/ui_window_mac.mm
        src/main.cpp
    )
    set_target_properties(CppDesk PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "CppDesk"
        MACOSX_BUNDLE_BUNDLE_IDENTIFIER "com.oocsng.cppdesk"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "3.9.2"
        MACOSX_BUNDLE_LONG_VERSION_STRING "3.9.2"
    )
    target_link_libraries(CppDesk PRIVATE
        ${PLATFORM_LIBS}
        ${COCOA_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZ_CORE}
    )
elseif(UNIX AND NOT APPLE)
    add_executable(CppDesk
        ${CORE_SOURCES}
        ${LINUX_UI_SOURCES}
        src/main.cpp
    )
    target_link_libraries(CppDesk PRIVATE ${PLATFORM_LIBS})
endif()
