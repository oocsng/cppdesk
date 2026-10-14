cmake_minimum_required(VERSION 3.20)
project(CppDesk VERSION 3.5.2 LANGUAGES C CXX ASM)

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
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
    list(APPEND CORE_SOURCES
        src/simd/simd_kernels.S
        src/control/windows_service_manager.cpp
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

# Automated test suite target
add_executable(CppDeskTests
    ${CORE_SOURCES}
    tests/test_suite.cpp
)
target_link_libraries(CppDeskTests PRIVATE ${PLATFORM_LIBS})

# Application executable
if(WIN32)
    add_executable(CppDesk
        ${CORE_SOURCES}
        src/ui/ui_window.cpp
        src/main.cpp
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
        MACOSX_BUNDLE_SHORT_VERSION_STRING "3.5.2"
        MACOSX_BUNDLE_LONG_VERSION_STRING "3.5.2"
    )
    target_link_libraries(CppDesk PRIVATE
        ${PLATFORM_LIBS}
        ${COCOA_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZ_CORE}
    )
endif()
