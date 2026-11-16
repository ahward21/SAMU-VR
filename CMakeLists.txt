cmake_minimum_required(VERSION 3.24)

# Static CRT so the proxy DLL has no VC++ redistributable dependency.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(samu_vr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ASAMU-Win32-Shipping.exe is PE32 / machine 0x14C. Enforce Win32.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SAMU-VR must be built for Win32 (x86) to match ASAMU-Win32-Shipping.exe. "
                        "Configure with: -A Win32")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ---------------- OpenXR-SDK (static loader) ----------------
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.65
    GIT_SHALLOW    TRUE)

# ---------------- MinHook (used from Phase 2 onward) ----------------
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(openxr minhook)

# The OpenXR static loader forces /MD; override to match our static CRT (/MT).
set_target_properties(openxr_loader minhook PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------- common (logging, config, shared utilities) ----------------
add_library(samu_common STATIC
    src/common/Config.cpp
    src/common/Config.h
    src/common/Log.cpp
    src/common/Log.h)
target_include_directories(samu_common PUBLIC src)
target_compile_definitions(samu_common PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(samu_common PUBLIC /W4 /permissive- /utf-8)

# ---------------- vr_core (OpenXR + DX9-to-DX11 stereo rendering; NO ue3_bridge deps) ----------------
add_library(vr_core STATIC
    src/vr_core/DX9DX11Bridge.cpp
    src/vr_core/DX9DX11Bridge.h
    src/vr_core/OpenXRManager.cpp
    src/vr_core/OpenXRManager.h
    src/vr_core/VrSettingsMenu.cpp
    src/vr_core/VrSettingsMenu.h
    src/vr_core/XrMath.h)
target_link_libraries(vr_core PUBLIC samu_common openxr_loader minhook d3d11 dxgi gdi32)
target_compile_definitions(vr_core PUBLIC XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11)

# ---------------- ue3_bridge (UE3 SDK dumper + ProcessEvent hook; NO vr_core deps) ----------------
add_library(ue3_bridge STATIC
    src/ue3_bridge/UE3Types.h
    src/ue3_bridge/SdkDumper.cpp
    src/ue3_bridge/SdkDumper.h
    src/ue3_bridge/UE3Bridge.cpp
    src/ue3_bridge/UE3Bridge.h)
target_link_libraries(ue3_bridge PUBLIC samu_common minhook)

# ---------------- d3d9.dll proxy ----------------
add_library(d3d9_proxy SHARED
    src/proxy/dllmain.cpp
    src/proxy/D3D9Proxy.cpp
    src/proxy/D3D9Proxy.h
    src/proxy/ModThread.cpp
    src/proxy/ModThread.h
    src/proxy/d3d9.def)
set_target_properties(d3d9_proxy PROPERTIES OUTPUT_NAME "d3d9")
target_link_libraries(d3d9_proxy PRIVATE vr_core ue3_bridge)
target_link_options(d3d9_proxy PRIVATE /SAFESEH:NO)

# Convenience: copy the built proxy next to the copied game binaries.
add_custom_command(TARGET d3d9_proxy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:d3d9_proxy> "${CMAKE_SOURCE_DIR}/dist/d3d9.dll"
    COMMENT "Copying d3d9.dll to dist/")

# ---------------- SAMU-VR-Launcher.exe (standalone single-file installer + VR launcher) ----------------
add_executable(samu_vr_launcher WIN32
    tools/launcher/launcher.cpp
    tools/launcher/launcher.rc
    tools/launcher/resource.h)
set_target_properties(samu_vr_launcher PROPERTIES OUTPUT_NAME "SAMU-VR-Launcher")
target_compile_definitions(samu_vr_launcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(samu_vr_launcher PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(samu_vr_launcher PRIVATE user32 gdi32 shell32 ole32 uuid comctl32 advapi32 winhttp bcrypt)
add_dependencies(samu_vr_launcher d3d9_proxy)
add_custom_command(TARGET samu_vr_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:samu_vr_launcher> "${CMAKE_SOURCE_DIR}/dist/SAMU-VR-Launcher.exe"
    COMMENT "Copying SAMU-VR-Launcher.exe to dist/")
