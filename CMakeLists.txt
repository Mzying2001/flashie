cmake_minimum_required(VERSION 3.20)
project(flashie LANGUAGES CXX)

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

add_executable(flashie WIN32
    source/main.cpp
    source/browser.cpp
    source/flash.cpp
    source/flash_loader.cpp
    source/app.manifest
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
)

target_link_libraries(flashie PRIVATE
    jscriptcc
    detours
    ole32
    oleaut32
    uuid
    shlwapi
    urlmon
)

# Copy the Flash.ocx matching the target Windows version and architecture.
# Output filename is always Flash.ocx.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(FLASH_OCX_SOURCE
        "${CMAKE_SOURCE_DIR}/assets/Flash64${FLASHIE_OCX_SUFFIX}.ocx")
else()
    set(FLASH_OCX_SOURCE
        "${CMAKE_SOURCE_DIR}/assets/Flash32${FLASHIE_OCX_SUFFIX}.ocx")
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
