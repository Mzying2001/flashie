cmake_minimum_required(VERSION 3.20)
project(flashie LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use /MT (static CRT) for Release builds
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(flashie WIN32
    source/main.cpp
    source/browser.cpp
    source/flash_loader.cpp
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
    ole32
    oleaut32
    uuid
    shlwapi
    urlmon
    shell32
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
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:flashie>/Flash"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FLASH_OCX_SOURCE}"
            "$<TARGET_FILE_DIR:flashie>/Flash/Flash.ocx"
    )
endif()

# Copy mms.cfg to Flash subdirectory (Flash EOL bypass config)
add_custom_command(TARGET flashie POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:flashie>/Flash"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/mms.cfg"
        "$<TARGET_FILE_DIR:flashie>/Flash/mms.cfg"
)
