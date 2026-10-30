# Sunstead client build configuration
cmake_minimum_required(VERSION 3.20)
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

# MinHook
add_subdirectory(ext/minhook)

set(LIBHAT_DISABLE_SSE OFF CACHE BOOL "Disables SSE scanning" FORCE)
set(LIBHAT_INSTALL_TARGET OFF CACHE BOOL "" FORCE)
set(LIBHAT_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(ext/libhat)

# ImGui
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext/imgui)
add_library(ImGui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
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

target_include_directories(Sunstead PRIVATE src ext ext/minhook/include ext/minhook/src ext/kiero ext/imgui)

# Add preprocessor definitions needed for ImGui/Windows.
# SUNSTEAD_DEBUG gates the debug console + Info/Warning logs (see Logger.h);
# defined explicitly for the Debug config so it never depends on the CRT flavour.
target_compile_definitions(Sunstead PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _ITERATOR_DEBUG_LEVEL=0 $<$<CONFIG:Debug>:SUNSTEAD_DEBUG=1>)

# VMProtect SDK markers (see src/Security/VMP.h and tools/VMPROTECT.md).
# OFF by default: Sunstead.dll then imports VMProtectSDK64.dll (copied next to
# the output by the post-build step below) and the SUN_VMP_* markers become
# real VMProtectBegin*/End calls the VMProtect GUI consumes when protecting the
# final DLL. Turn OFF for a clean build with no VMProtect dependency.
set(SUNSTEAD_VMP OFF CACHE BOOL "Embed VMProtect SDK markers")
set(VMP_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext/vmprotect)
if(SUNSTEAD_VMP)
    target_compile_definitions(Sunstead PRIVATE SUNSTEAD_VMP=1)
    target_include_directories(Sunstead PRIVATE ${VMP_SDK_DIR})
    # MSVC picks the .lib up through the #pragma comment in VMProtectSDK.h, but
    # an explicit link keeps non-MSVC generators honest.
    target_link_libraries(Sunstead PRIVATE ${VMP_SDK_DIR}/VMProtectSDK64.lib)
    add_custom_command(TARGET Sunstead POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${VMP_SDK_DIR}/VMProtectSDK64.dll $<TARGET_FILE_DIR:Sunstead>
        COMMENT "Copying VMProtectSDK64.dll next to Sunstead.dll")
endif()

# Enable SEH with C++ objects (needed for __try/__except around std::string etc.)
# /utf-8 keeps Russian chat/notification literals valid UTF-8 in the binary.
target_compile_options(Sunstead PRIVATE /EHa /MP /utf-8)

target_link_options(Sunstead PRIVATE /MAP /MAPINFO:EXPORTS)

target_link_libraries(Sunstead PRIVATE
    minhook
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
