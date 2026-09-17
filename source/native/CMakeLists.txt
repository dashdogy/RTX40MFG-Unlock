cmake_minimum_required(VERSION 3.24)
project(RTX40MFGNative VERSION 1.3.3.1 LANGUAGES C CXX ASM_MASM)
option(MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK "Isolated comparison: keep proxies/input but skip UI GPU work" OFF)
option(MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS "Bounded NGX runtime Create results without DRED or new hooks" OFF)
option(MFG_UNLOCK_GPU_FAULT_CAPTURE "Isolated local DRED and NGX result diagnostics" OFF)
option(MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY "Isolated comparison without integrated overlay interception" OFF)
if(MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY AND NOT MFG_UNLOCK_GPU_FAULT_CAPTURE)
    message(FATAL_ERROR "The overlay isolation requires the local GPU fault recorder")
endif()

option(MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN
    "Built-in UI using owned DXGI proxies and bounded application import gateways" ON)
option(MFG_UNLOCK_OVERLAY_MENU_DRAW
    "Enable menu drawing on owned swapchain proxies" ON)
option(MFG_UNLOCK_OVERLAY_TEST_FAULTS
    "Deterministic coordinator and patch fault points for the regression harness" OFF)
if(MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN AND MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY)
    message(FATAL_ERROR "The bounded redesign and the no-overlay diagnostic are exclusive")
endif()
if(MFG_UNLOCK_OVERLAY_MENU_DRAW AND NOT MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN)
    message(FATAL_ERROR "The restored menu requires the bounded installation")
endif()
if(MFG_UNLOCK_OVERLAY_TEST_FAULTS AND NOT MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN)
    message(FATAL_ERROR "Coordinator fault points require the bounded installation")
endif()
if(MFG_UNLOCK_OVERLAY_TEST_FAULTS AND NOT MFG_UNLOCK_GPU_FAULT_CAPTURE)
    message(FATAL_ERROR "Test-fault builds are diagnostic candidates and require the fault recorder")
endif()

set(STREAMLINE_ROOT "" CACHE PATH "Path to the NVIDIA Streamline SDK directory")
if(NOT EXISTS "${STREAMLINE_ROOT}/include/sl_dlss_g.h")
    message(FATAL_ERROR "Set STREAMLINE_ROOT to a Streamline SDK directory containing include/sl_dlss_g.h")
endif()

option(MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY
    "Apply the exact-profile equivalent OutputPull mask simplification before first Ada FG Create" ON)
option(MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY
    "Isolated mask candidate with the six-block occupancy hint; requires separate runtime validation" OFF)
if(MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY AND NOT MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY)
    message(FATAL_ERROR "The OutputPull mask occupancy candidate requires the mask-only path")
endif()
option(MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT
    "Use the exact-profile OutputPull backport plus six-block occupancy hint" OFF)
if(MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY AND MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT)
    message(FATAL_ERROR "Do not enable two publishers for the OutputPull descriptor")
endif()
option(MFG_UNLOCK_OUTPUT_PULL_TELEMETRY
    "Write low-overhead OutputPull A/B provenance and FG activity telemetry" OFF)
option(MFG_UNLOCK_PREV2CURR_EXPERIMENT
    "Add the exact-profile Prev2CurrUnpackPull backport to the OutputPull experiment" OFF)
if(MFG_UNLOCK_PREV2CURR_EXPERIMENT AND NOT MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT)
    message(FATAL_ERROR "The Prev2Curr experiment requires the OutputPull candidate")
endif()
option(MFG_UNLOCK_INTERM_SCATTER_EXPERIMENT
    "Use the full exact-profile temporal scatter backport with both pull experiments" OFF)
if(MFG_UNLOCK_INTERM_SCATTER_EXPERIMENT AND
    (NOT MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT OR NOT MFG_UNLOCK_PREV2CURR_EXPERIMENT))
    message(FATAL_ERROR "The temporal scatter experiment requires the OutputPull and Prev2Curr candidates")
endif()
set(IMGUI_ROOT "" CACHE PATH
    "Path to the Dear ImGui source tree used by that ReShade version")

option(MFG_UNLOCK_RUNTIME_GPU_SELECTION "Build one renamable Ada and experimental Ampere module" ON)
if(NOT MFG_UNLOCK_RUNTIME_GPU_SELECTION)
    message(FATAL_ERROR "This isolated unified source requires MFG_UNLOCK_RUNTIME_GPU_SELECTION=ON")
endif()
if(MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT OR MFG_UNLOCK_OUTPUT_PULL_TELEMETRY
   OR MFG_UNLOCK_PREV2CURR_EXPERIMENT OR MFG_UNLOCK_INTERM_SCATTER_EXPERIMENT)
    message(FATAL_ERROR "Keep kernel experiments out of the unified build")
endif()
include(ampere_native_cache.cmake)
if(NOT MFG_AMPERE_EMBEDDED_KERNELS)
    message(FATAL_ERROR "The unified distribution requires embedded SM86 kernels")
endif()
add_compile_definitions(MFG_UNLOCK_RUNTIME_GPU_SELECTION=1 MFG_UNLOCK_AMPERE_MFG=0)

set(MFG_UNLOCK_CORE_SOURCES
    patcher.cpp
    midpoint_fix.cpp
    gpu_dispatch.cpp
    ampere_gpu.cpp
    ampere_cuda_program.cpp
    ampere_native_cache.cpp
    ampere_mask_transform.cpp
    ampere_backend.cpp
    ampere_wrapper_capacity.cpp
    dlssg_provider_policy.cpp
    dlssg_preset.cpp
    reflex_control.cpp
    vsync_control.cpp
    entry_detour.cpp
    entry_detour.asm
    nvidia_mfg_policy.cpp
    temporal_interval_trace.cpp
    fault_capture.cpp
    output_pull_telemetry.cpp
)

option(MFG_UNLOCK_BUILD_SINGLE_MODULE "Build integrated backend and ImGui UI" ON)
set(VULKAN_INCLUDE_DIR "" CACHE PATH "Explicit Vulkan SDK headers for the standalone renderer")
if(MFG_UNLOCK_BUILD_SINGLE_MODULE)
    if(NOT EXISTS "${IMGUI_ROOT}/backends/imgui_impl_dx12.cpp" OR
       NOT EXISTS "${IMGUI_ROOT}/backends/imgui_impl_vulkan.cpp")
        message(FATAL_ERROR "The integrated UI requires the pinned ImGui source and render backends")
    endif()
    if(NOT EXISTS "${VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
        message(FATAL_ERROR "Set VULKAN_INCLUDE_DIR to the explicit Vulkan SDK include directory")
    endif()

    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(GLSLANG_VALIDATOR "" CACHE FILEPATH "glslangValidator.exe from Vulkan SDK 1.2.176.1")
    if(NOT EXISTS "${GLSLANG_VALIDATOR}")
        message(FATAL_ERROR "Set GLSLANG_VALIDATOR to the pinned Vulkan shader compiler")
    endif()
    set(MFG_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_build_inputs.py"
        --output "${MFG_GENERATED}" --imgui "${IMGUI_ROOT}" --glslang "${GLSLANG_VALIDATOR}"
        COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        generate_build_inputs.py proxy_exports.txt generated_sha256.json ui_color.frag
        "${IMGUI_ROOT}/backends/imgui_impl_dx12.cpp" "${GLSLANG_VALIDATOR}")
    include_directories("${MFG_GENERATED}")

    add_library(MfgSingleMinHook STATIC
        third_party/minhook/src/buffer.c
        third_party/minhook/src/hook.c
        third_party/minhook/src/trampoline.c
        third_party/minhook/src/hde/hde64.c)
    target_include_directories(MfgSingleMinHook PUBLIC third_party/minhook/include)
    target_compile_definitions(MfgSingleMinHook PRIVATE WIN32_LEAN_AND_MEAN)
    if(MFG_UNLOCK_OVERLAY_TEST_FAULTS)
        target_compile_definitions(MfgSingleMinHook PRIVATE MFG_UNLOCK_TEST_FAULTS=1)
    endif()
    set_target_properties(MfgSingleMinHook PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_library(MfgSingleImGui STATIC
        "${IMGUI_ROOT}/imgui.cpp"
        "${IMGUI_ROOT}/imgui_draw.cpp"
        "${IMGUI_ROOT}/imgui_widgets.cpp"
        "${IMGUI_ROOT}/imgui_tables.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_win32.cpp"
        "${MFG_GENERATED}/imgui_impl_dx12.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_vulkan.cpp")
    target_compile_features(MfgSingleImGui PRIVATE cxx_std_20)
    target_compile_options(MfgSingleImGui PRIVATE /Zi)
    target_include_directories(MfgSingleImGui PUBLIC "${IMGUI_ROOT}" "${VULKAN_INCLUDE_DIR}")
    target_compile_definitions(MfgSingleImGui PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN ImTextureID=ImU64
        VK_USE_PLATFORM_WIN32_KHR=1 VK_NO_PROTOTYPES=1 IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1)
    set_target_properties(MfgSingleImGui PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set(MFG_SINGLE_CORE_SOURCES ${MFG_UNLOCK_CORE_SOURCES})
    add_library(MfgSingleObjects OBJECT ${MFG_SINGLE_CORE_SOURCES}
        single_module.cpp single_overlay.cpp overlay_application_imports.cpp overlay_platform.cpp caller_scoped_import.cpp
        overlay_dx12.cpp overlay_dxgi_proxy.cpp overlay_vulkan.cpp overlay_install.cpp reshade_frontend.cpp
        "${MFG_GENERATED}/proxy_generated/renamable_exports.asm")
    target_compile_features(MfgSingleObjects PRIVATE cxx_std_20)
    target_compile_options(MfgSingleObjects PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
    target_compile_options(MfgSingleObjects PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/Zi>)
    target_compile_definitions(MfgSingleObjects PRIVATE
        MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK=$<BOOL:${MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK}>
        MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS=$<BOOL:${MFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS}>
        MFG_UNLOCK_GPU_FAULT_CAPTURE=$<BOOL:${MFG_UNLOCK_GPU_FAULT_CAPTURE}>
        MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY=$<BOOL:${MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY}>
        MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN=$<BOOL:${MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN}>
        MFG_UNLOCK_OVERLAY_MENU_DRAW=$<BOOL:${MFG_UNLOCK_OVERLAY_MENU_DRAW}>
        MFG_UNLOCK_OVERLAY_TEST_FAULTS=$<BOOL:${MFG_UNLOCK_OVERLAY_TEST_FAULTS}>
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN ImTextureID=ImU64
        MFG_UNLOCK_SINGLE_MODULE_UI=1 MFG_UNLOCK_UNIVERSAL_CONFIG=1
        MFG_UNLOCK_RESHADE_UI_CLIENT=1 MFG_UNLOCK_V12_UNIVERSAL_UI=1
        VK_USE_PLATFORM_WIN32_KHR=1 VK_NO_PROTOTYPES=1 IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1
        MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY=$<BOOL:${MFG_UNLOCK_OUTPUT_PULL_MASK_ONLY}>
        MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY=$<BOOL:${MFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY}>
        MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT=$<BOOL:${MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT}>
        MFG_UNLOCK_OUTPUT_PULL_TELEMETRY=$<BOOL:${MFG_UNLOCK_OUTPUT_PULL_TELEMETRY}>
        MFG_UNLOCK_PREV2CURR_EXPERIMENT=$<BOOL:${MFG_UNLOCK_PREV2CURR_EXPERIMENT}>
        MFG_UNLOCK_INTERM_SCATTER_EXPERIMENT=$<BOOL:${MFG_UNLOCK_INTERM_SCATTER_EXPERIMENT}>)
    target_include_directories(MfgSingleObjects PRIVATE
        "${STREAMLINE_ROOT}/include" "${STREAMLINE_ROOT}/external/ngx-sdk/include"
        "${IMGUI_ROOT}" "${VULKAN_INCLUDE_DIR}" third_party/minhook/include)
    set_target_properties(MfgSingleObjects PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_library(RTXMFGUnified SHARED $<TARGET_OBJECTS:MfgSingleObjects>
        single_version.rc "${MFG_GENERATED}/proxy_generated/renamable.def" ${MFG_AMPERE_KERNEL_RESOURCE})
    target_compile_definitions(RTXMFGUnified PRIVATE MFG_SINGLE_FILENAME="RTXMFG.dll")
    target_link_libraries(RTXMFGUnified PRIVATE MfgSingleImGui MfgSingleMinHook
        bcrypt version wintrust crypt32 delayimp d3dcompiler dxgi user32 gdi32 imm32 dwmapi)
    target_link_options(RTXMFGUnified PRIVATE /DELAYLOAD:version.dll /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:dxgi.dll
        /DEBUG:FULL /OPT:REF /OPT:ICF /IGNORE:4222)
    target_include_directories(RTXMFGUnified PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(RTXMFGUnified PROPERTIES PREFIX "" OUTPUT_NAME "RTXMFG"
        LINKER_LANGUAGE CXX MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_custom_target(RTX40MFGSingle ALL DEPENDS RTXMFGUnified)
endif()
