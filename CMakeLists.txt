# Sunstead client build configuration
cmake_minimum_required(VERSION 3.28)

project(Sunstead)
enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set the output directory to bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# SafetyHook (inline hooking engine, replaces MinHook). Zydis is fetched via
# CPM at configure time (SAFETYHOOK_FETCH_ZYDIS defaults to ON).
set(SAFETYHOOK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(SAFETYHOOK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SAFETYHOOK_BUILD_DOCS OFF CACHE BOOL "" FORCE)
add_subdirectory(ext/safetyhook)

set(LIBHAT_FEATURE_SSE ON CACHE BOOL "" FORCE)
set(LIBHAT_FEATURE_AVX512 ON CACHE BOOL "" FORCE)
set(LIBHAT_INSTALL_TARGET OFF CACHE BOOL "" FORCE)
set(LIBHAT_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(ext/libhat)

# ImGui
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext/imgui)
add_library(ImGui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(ImGui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

# Kiero
set(KIERO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext/kiero)
add_library(Kiero STATIC
    ${KIERO_DIR}/kiero.cpp
)
target_include_directories(Kiero PUBLIC ${KIERO_DIR})

# Add all source files
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "src/*.h")

add_library(Sunstead SHARED ${SOURCES} ${HEADERS}
    "${CMAKE_SOURCE_DIR}/src/resources.rc"
)

target_include_directories(Sunstead PRIVATE src ext ext/safetyhook/include ext/kiero ext/imgui)

target_precompile_headers(Sunstead PRIVATE src/pch.h)

# Add preprocessor definitions needed for ImGui/Windows.
# SUNSTEAD_DEBUG gates the debug console + Info/Warning logs (see Logger.h);
# defined explicitly for the Debug config so it never depends on the CRT flavour.
target_compile_definitions(Sunstead PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _ITERATOR_DEBUG_LEVEL=0 $<$<CONFIG:Debug>:SUNSTEAD_DEBUG=1>)

# Enable SEH with C++ objects (needed for __try/__except around std::string etc.)
# /utf-8 keeps Russian chat/notification literals valid UTF-8 in the binary.
target_compile_options(Sunstead PRIVATE /EHa /MP /utf-8)

target_link_options(Sunstead PRIVATE /MAP /MAPINFO:EXPORTS $<$<CONFIG:Debug>:/DEBUG:FULL>)

target_link_libraries(Sunstead PRIVATE
    safetyhook
    ImGui
    Kiero
    libhat
    d3d11
    d3d12
    dxgi
    d3dcompiler
    d2d1
    dxguid
    user32
    winhttp
    windowscodecs
    ole32
    version
)
