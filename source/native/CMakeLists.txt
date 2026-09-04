cmake_minimum_required(VERSION 3.24)
project(RTX40MFGNative LANGUAGES C CXX ASM_MASM)

set(STREAMLINE_ROOT "" CACHE PATH "Path to the NVIDIA Streamline SDK directory")
if(NOT EXISTS "${STREAMLINE_ROOT}/include/sl_dlss_g.h")
    message(FATAL_ERROR "Set STREAMLINE_ROOT to a Streamline SDK directory containing include/sl_dlss_g.h")
endif()

option(MFG_UNLOCK_BUILD_UNIVERSAL_UI
    "Build the V1.2 Release ReShade control and telemetry add-on" ON)
set(RESHADE_ROOT "" CACHE PATH
    "Path to a pinned ReShade source tree containing include/reshade.hpp")
set(IMGUI_ROOT "" CACHE PATH
    "Path to the Dear ImGui source tree used by that ReShade version")

set(MFG_UNLOCK_CORE_SOURCES
    patcher.cpp
    midpoint_fix.cpp
    dlssg_provider_policy.cpp
    entry_detour.cpp
    entry_detour.asm
    nvidia_mfg_policy.cpp
    temporal_interval_trace.cpp
    v1_2_core.rc
)

# MinHook is vendored from the ReShade 6.8.0 pinned submodule revision
# 8fda4f5481fed5797dc2651cd91e238e9b3928c6. Its HDE64 relocator is used only
# after the strict atomic-hotpatch route is unavailable.
add_library(MfgUnlockMinHook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(MfgUnlockMinHook PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include"
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/hde"
)
target_compile_definitions(MfgUnlockMinHook PRIVATE WIN32_LEAN_AND_MEAN)

add_library(RTX40MFGCore SHARED ${MFG_UNLOCK_CORE_SOURCES})
target_compile_features(RTX40MFGCore PRIVATE cxx_std_20)
target_compile_definitions(RTX40MFGCore PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    MFG_UNLOCK_UNIVERSAL_CONFIG=1)
target_include_directories(RTX40MFGCore PRIVATE
    "${STREAMLINE_ROOT}/include"
    "${STREAMLINE_ROOT}/external/ngx-sdk/include"
)
target_link_libraries(RTX40MFGCore PRIVATE
    MfgUnlockMinHook bcrypt version)
set_target_properties(RTX40MFGCore PROPERTIES
    PREFIX ""
    OUTPUT_NAME "RTX40MFGCore"
)

add_library(RTX40MFGAuto SHARED loader_shim.cpp v1_2_shim.rc)
target_compile_features(RTX40MFGAuto PRIVATE cxx_std_20)
target_compile_definitions(RTX40MFGAuto PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(RTX40MFGAuto PRIVATE RTX40MFGCore)
set_target_properties(RTX40MFGAuto PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "RTX40MFG"
)

if(MFG_UNLOCK_BUILD_UNIVERSAL_UI)
    if(NOT EXISTS "${RESHADE_ROOT}/include/reshade.hpp")
        message(FATAL_ERROR
            "Set RESHADE_ROOT to the pinned ReShade source directory")
    endif()
    if(NOT EXISTS "${IMGUI_ROOT}/imgui.h")
        message(FATAL_ERROR
            "Set IMGUI_ROOT to ReShade's pinned Dear ImGui directory")
    endif()

    add_library(RTX40MFGReShadeUI SHARED
        reshade_frontend.cpp
        reshade_frontend_entry.cpp
        reshade_ui_version.rc
    )
    target_compile_features(RTX40MFGReShadeUI PRIVATE cxx_std_20)
    target_compile_definitions(RTX40MFGReShadeUI PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        MFG_UNLOCK_RESHADE_ADDON=1
        MFG_UNLOCK_RESHADE_UI_CLIENT=1
        MFG_UNLOCK_V12_UNIVERSAL_UI=1
        ImTextureID=ImU64
    )
    target_include_directories(RTX40MFGReShadeUI PRIVATE
        "${RESHADE_ROOT}/include"
        "${IMGUI_ROOT}"
    )
    set_target_properties(RTX40MFGReShadeUI PROPERTIES
        PREFIX ""
        SUFFIX ".addon64"
        OUTPUT_NAME "RTX40MFG-UI"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
