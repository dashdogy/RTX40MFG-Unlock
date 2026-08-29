cmake_minimum_required(VERSION 3.24)
project(RTX40MFGNative LANGUAGES CXX)

set(STREAMLINE_ROOT "" CACHE PATH "Path to the NVIDIA Streamline SDK directory")
if(NOT EXISTS "${STREAMLINE_ROOT}/include/sl_dlss_g.h")
    message(FATAL_ERROR "Set STREAMLINE_ROOT to a Streamline SDK directory containing include/sl_dlss_g.h")
endif()

add_library(RTX40MFGAuto SHARED patcher.cpp)
target_compile_features(RTX40MFGAuto PRIVATE cxx_std_20)
target_compile_definitions(RTX40MFGAuto PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(RTX40MFGAuto PRIVATE "${STREAMLINE_ROOT}/include")
set_target_properties(RTX40MFGAuto PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "RTX40MFG"
)
