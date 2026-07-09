cmake_minimum_required(VERSION 3.20)
project(flashie LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use /MT (static CRT) for Release builds
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(JScriptCC)
# Force JScriptCC to use the same CRT as flashie (its cmake_minimum_required resets CMP0091)
set_target_properties(jscriptcc PROPERTIES MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")

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
    assets/app.manifest
)

set_target_properties(flashie PROPERTIES OUTPUT_NAME "FlashIE")

target_compile_options(flashie PRIVATE /utf-8)

target_compile_definitions(flashie PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
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

# Copy the correct Flash.ocx (32-bit or 64-bit) based on target architecture.
# Output filename is always Flash.ocx.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(FLASH_OCX_SOURCE "${CMAKE_SOURCE_DIR}/assets/Flash64.ocx")
else()
    set(FLASH_OCX_SOURCE "${CMAKE_SOURCE_DIR}/assets/Flash32.ocx")
endif()

if(EXISTS "${FLASH_OCX_SOURCE}")
    add_custom_command(TARGET flashie POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FLASH_OCX_SOURCE}"
            "$<TARGET_FILE_DIR:flashie>/Flash.ocx"
    )
endif()
