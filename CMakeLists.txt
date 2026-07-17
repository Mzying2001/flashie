cmake_minimum_required(VERSION 3.20)
project(flashie VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FLASHIE_WINDOWS_TARGET "WIN8" CACHE STRING
    "Windows compatibility profile: WIN7 uses the Windows 7-and-earlier OCX; WIN8 uses the Windows 8-and-later OCX")
set_property(CACHE FLASHIE_WINDOWS_TARGET PROPERTY STRINGS WIN7 WIN8)
string(TOUPPER "${FLASHIE_WINDOWS_TARGET}" FLASHIE_WINDOWS_TARGET_NORMALIZED)

if(FLASHIE_WINDOWS_TARGET_NORMALIZED STREQUAL "WIN7")
    set(FLASHIE_WINVER 0x0601)
    set(FLASHIE_NTDDI_VERSION 0x06010000)
    set(FLASHIE_SUBSYSTEM_VERSION 6.01)
    set(FLASHIE_OCX_SUFFIX "_Win7")
elseif(FLASHIE_WINDOWS_TARGET_NORMALIZED STREQUAL "WIN8")
    set(FLASHIE_WINVER 0x0602)
    set(FLASHIE_NTDDI_VERSION 0x06020000)
    set(FLASHIE_SUBSYSTEM_VERSION 6.02)
    set(FLASHIE_OCX_SUFFIX "")
else()
    message(FATAL_ERROR
        "Unsupported FLASHIE_WINDOWS_TARGET='${FLASHIE_WINDOWS_TARGET}'. "
        "Expected WIN7 or WIN8.")
endif()

# Use /MT (static CRT) for Release builds
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(JScriptCC)
# Force JScriptCC to use the same CRT as flashie (its cmake_minimum_required resets CMP0091)
set_target_properties(jscriptcc PROPERTIES MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
target_compile_options(jscriptcc PRIVATE /utf-8)

# Build the Rust SWC wrapper for the current MSVC architecture and expose its
# C ABI as a normal CMake static-library target.
set(SWC_ES5_SOURCE_DIR "${CMAKE_SOURCE_DIR}/swc-es5-c-api")
if(NOT EXISTS "${SWC_ES5_SOURCE_DIR}/Cargo.toml")
    message(FATAL_ERROR
        "swc-es5-c-api is not initialized. Run: git submodule update --init --recursive")
endif()

find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SWC_ES5_RUST_TARGET "x86_64-win7-windows-msvc")
else()
    set(SWC_ES5_RUST_TARGET "i686-win7-windows-msvc")
endif()

set(SWC_ES5_TARGET_DIR
    "${SWC_ES5_SOURCE_DIR}/target/${SWC_ES5_RUST_TARGET}")
set(SWC_ES5_DEBUG_LIBRARY
    "${SWC_ES5_TARGET_DIR}/debug/swc_es5_c_api.lib")
set(SWC_ES5_RELEASE_LIBRARY
    "${SWC_ES5_TARGET_DIR}/release/swc_es5_c_api.lib")

add_custom_target(swc_es5_build
    COMMAND "${CMAKE_COMMAND}" -E env
        "RUSTFLAGS=-C target-feature=+crt-static$<$<NOT:$<CONFIG:Debug>>: -Z location-detail=none>"
        "CARGO_PROFILE_RELEASE_OPT_LEVEL=s"
        "CARGO_PROFILE_RELEASE_LTO=fat"
        "CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1"
        "CARGO_PROFILE_RELEASE_DEBUG=0"
        "CARGO_PROFILE_RELEASE_STRIP=symbols"
        "${CARGO_EXECUTABLE}" build --locked --lib
        --target "${SWC_ES5_RUST_TARGET}"
        -Z "build-std=std,panic_unwind"
        $<$<NOT:$<CONFIG:Debug>>:--release>
    WORKING_DIRECTORY "${SWC_ES5_SOURCE_DIR}"
    COMMENT "Building swc-es5-c-api (${SWC_ES5_RUST_TARGET})"
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_library(swc_es5 STATIC IMPORTED GLOBAL)
set_target_properties(swc_es5 PROPERTIES
    IMPORTED_LOCATION "${SWC_ES5_RELEASE_LIBRARY}"
    IMPORTED_LOCATION_DEBUG "${SWC_ES5_DEBUG_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SWC_ES5_SOURCE_DIR}/include"
)

# Keep this list synchronized with swc-es5-c-api's native-static-libs report.
target_link_libraries(swc_es5 INTERFACE
    kernel32
    legacy_stdio_definitions
    advapi32
    ntdll
    userenv
    ws2_32
    dbghelp
)
# Microsoft Detours — function hooking library (no CMake support, build manually)
add_library(detours STATIC
    Detours/src/detours.cpp
    Detours/src/disasm.cpp
    Detours/src/modules.cpp
    Detours/src/image.cpp
    Detours/src/creatwth.cpp
    Detours/src/disolx86.cpp
    Detours/src/disolx64.cpp
    Detours/src/disolia64.cpp
    Detours/src/disolarm.cpp
    Detours/src/disolarm64.cpp
)
target_include_directories(detours PUBLIC Detours/src)
target_compile_options(detours PRIVATE /W0)
set_target_properties(detours PROPERTIES MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")

set(FLASHIE_ICON_SOURCE "${CMAKE_SOURCE_DIR}/assets/icons/flashie.ico")
if(NOT EXISTS "${FLASHIE_ICON_SOURCE}")
    message(FATAL_ERROR "FlashIE icon not found: ${FLASHIE_ICON_SOURCE}")
endif()

configure_file(
    source/version.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)

add_executable(flashie WIN32
    source/main.cpp
    source/browser.cpp
    source/swf_mime_filter.cpp
    source/flash.cpp
    source/flash_loader.cpp
    source/debug.cpp
    source/resource.h
    source/app.manifest
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)

set_target_properties(flashie PROPERTIES OUTPUT_NAME "FlashIE")

target_compile_options(flashie PRIVATE /utf-8)

target_compile_definitions(flashie PRIVATE
    UNICODE
    _UNICODE
    WINVER=${FLASHIE_WINVER}
    _WIN32_WINNT=${FLASHIE_WINVER}
    NTDDI_VERSION=${FLASHIE_NTDDI_VERSION}
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_options(flashie PRIVATE
    "/SUBSYSTEM:WINDOWS,${FLASHIE_SUBSYSTEM_VERSION}"
    # rustc's crt-static target emits /DEFAULTLIB:libcmt in Debug as well.
    "$<$<CONFIG:Debug>:/NODEFAULTLIB:libcmt>"
)

target_link_libraries(flashie PRIVATE
    jscriptcc
    swc_es5
    detours
    ole32
    oleaut32
    uuid
    shell32
    shlwapi
    comctl32
    urlmon
)

add_dependencies(flashie swc_es5_build)

# Copy the Flash.ocx matching the target Windows version and architecture.
# Output filename is always Flash.ocx.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(FLASH_OCX_SOURCE
        "${CMAKE_SOURCE_DIR}/assets/flash/Flash64${FLASHIE_OCX_SUFFIX}.ocx")
else()
    set(FLASH_OCX_SOURCE
        "${CMAKE_SOURCE_DIR}/assets/flash/Flash32${FLASHIE_OCX_SUFFIX}.ocx")
endif()

if(NOT EXISTS "${FLASH_OCX_SOURCE}")
    message(FATAL_ERROR "Flash OCX not found: ${FLASH_OCX_SOURCE}")
endif()

message(STATUS
    "FlashIE target: ${FLASHIE_WINDOWS_TARGET_NORMALIZED}; OCX: ${FLASH_OCX_SOURCE}")

add_custom_command(TARGET flashie POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FLASH_OCX_SOURCE}"
        "$<TARGET_FILE_DIR:flashie>/Flash.ocx"
)
