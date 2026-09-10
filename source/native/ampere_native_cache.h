#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <span>

namespace ampere_native_cache
{
inline constexpr size_t kSlots = 25;
inline constexpr size_t kMaximumSlots = 38;
using Program = std::array<std::vector<uint8_t>, kSlots>;
using CompleteProgram = std::vector<std::vector<uint8_t>>;
using ProgramView = std::span<const std::vector<uint8_t>>;
enum class Mode : uint32_t { ePtx = 0, eAuto = 1, eCubin = 2 };
enum class Status : uint32_t
{
    eNotRequested = 0, eLoaded = 1, eNoManifest = 2, eSourceMismatch = 3,
    eFileUnavailable = 4, eHashMismatch = 5, eInvalidContainer = 6,
    eSourceShape = 7, eAllocation = 8, eDriverRejected = 9,
};
struct NativeContract
{
    const char* entryName;
    uint32_t parameterBytes;
    std::array<uint32_t, 3> maximumThreads;
    uint32_t sharedBytes;
    uint32_t registers;
    uint32_t elfAbi;
    uint32_t elfFlags;
    const char* metadataSha256;
    uint32_t parameterCount = 1;
};
Mode ConfiguredMode() noexcept;
bool HasManifest() noexcept;
Status LoadEmbeddedProgram(const Program& validatedPtx, void* module, Program& output) noexcept;
Status LoadConfiguredProgram(const Program& validatedPtx, Program& output) noexcept;
Status LoadEmbeddedProgram(ProgramView validatedPtx, void* module, CompleteProgram& output) noexcept;
Status LoadConfiguredProgram(ProgramView validatedPtx, CompleteProgram& output) noexcept;
std::wstring DefaultDirectory();
bool ValidateNativeContainer(const std::vector<uint8_t>&) noexcept;
bool ValidateNativeContainer(const std::vector<uint8_t>&, const NativeContract&) noexcept;
Status LoadProgram(const Program& validatedPtx, const std::wstring& directory, Program& output) noexcept;
Status LoadProgram(ProgramView validatedPtx, const std::wstring& directory, CompleteProgram& output) noexcept;
}
