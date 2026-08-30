cmake_minimum_required(VERSION 3.24)
project(AsiLiveControlHarness LANGUAGES CXX ASM_MASM)

set(STREAMLINE_ROOT "" CACHE PATH "Path to the NVIDIA Streamline SDK")
if(NOT EXISTS "${STREAMLINE_ROOT}/include/sl_dlss_g.h")
    message(FATAL_ERROR "Set STREAMLINE_ROOT to the NVIDIA Streamline SDK")
endif()

add_library(SlInterposerStub SHARED stub.cpp stub.def)
target_compile_features(SlInterposerStub PRIVATE cxx_std_20)
target_compile_definitions(SlInterposerStub PRIVATE SL_INTERPOSER)
target_include_directories(SlInterposerStub PRIVATE "${STREAMLINE_ROOT}/include")
set_target_properties(SlInterposerStub PROPERTIES
    PREFIX ""
    OUTPUT_NAME "sl.interposer"
)

add_library(DlssGWrapperStub SHARED wrapper.cpp wrapper_pattern.asm wrapper.def)
target_compile_features(DlssGWrapperStub PRIVATE cxx_std_20)
target_include_directories(DlssGWrapperStub PRIVATE
    "${STREAMLINE_ROOT}/include"
    "${STREAMLINE_ROOT}/external/ngx-sdk/include"
)
set_target_properties(DlssGWrapperStub PROPERTIES
    PREFIX ""
    OUTPUT_NAME "sl.dlss_g"
)
target_link_libraries(DlssGWrapperStub PRIVATE D3D12)

add_library(NgxDlssGStub SHARED ngx.cpp ngx_pattern.asm ngx.def)
target_compile_features(NgxDlssGStub PRIVATE cxx_std_20)
target_include_directories(NgxDlssGStub PRIVATE
    "${STREAMLINE_ROOT}/external/ngx-sdk/include"
)
set_target_properties(NgxDlssGStub PROPERTIES
    PREFIX ""
    OUTPUT_NAME "nvngx_dlssg"
)

add_library(NgxLegacyStub SHARED ngx.cpp ngx_pattern_legacy.asm ngx_legacy.def)
target_compile_features(NgxLegacyStub PRIVATE cxx_std_20)
target_include_directories(NgxLegacyStub PRIVATE
    "${STREAMLINE_ROOT}/external/ngx-sdk/include"
)
set_target_properties(NgxLegacyStub PROPERTIES
    PREFIX ""
    OUTPUT_NAME "nvngx_dlssg_legacy"
)

add_executable(AsiLiveControlHarness main.cpp version.rc)
target_compile_features(AsiLiveControlHarness PRIVATE cxx_std_20)
target_compile_definitions(AsiLiveControlHarness PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(AsiLiveControlHarness PRIVATE "${STREAMLINE_ROOT}/include")
target_link_libraries(AsiLiveControlHarness PRIVATE Version SlInterposerStub)
add_dependencies(AsiLiveControlHarness DlssGWrapperStub NgxDlssGStub NgxLegacyStub)
