#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace single_overlay
{
struct OverlayScaling
{
    float framebufferX = 1.0f;
    float framebufferY = 1.0f;
    float pixels = 1.0f;
    float logical = 1.0f;
    bool valid = false;
};

inline OverlayScaling CalculateOverlayScaling(uint32_t outputWidth, uint32_t outputHeight,
    float clientWidth, float clientHeight, float dpi) noexcept
{
    OverlayScaling result;
    if (!outputWidth || !outputHeight || !std::isfinite(clientWidth)
        || !std::isfinite(clientHeight) || clientWidth < 16.0f || clientHeight < 16.0f)
        return result;
    if (!std::isfinite(dpi) || dpi <= 0.0f) dpi = 1.0f;
    result.framebufferX = float(outputWidth) / clientWidth;
    result.framebufferY = float(outputHeight) / clientHeight;
    const float resolution = std::min(float(outputWidth)/1920.0f, float(outputHeight)/1080.0f);
    result.pixels = std::clamp(std::max(dpi, resolution), 1.0f, 4.0f);
    // Win32 input stays in client coordinates. Renderer and font rasterizer
    // both receive the actual output/client pixel ratio, without double DPI.
    result.logical = result.pixels / result.framebufferY;
    result.valid = result.framebufferX >= 0.125f && result.framebufferX <= 8.0f
        && result.framebufferY >= 0.125f && result.framebufferY <= 8.0f;
    return result;
}
}
