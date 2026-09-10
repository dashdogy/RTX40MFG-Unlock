#pragma once
#include <cstdint>

namespace ampere_mfg
{
inline constexpr uint32_t kMaximumGeneratedFrames = 5;
inline constexpr uint32_t kPresentationBuffers = kMaximumGeneratedFrames + 1;
}
