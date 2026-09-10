#pragma once

#include <Windows.h>
#include <cstdint>

namespace output_pull_telemetry
{
void Initialize(const wchar_t* directory, DWORD pid) noexcept;
void RecordCreate(bool proofReady, bool prev2CurrProofReady = false) noexcept;
void RecordEvaluate(const void* featureHandle) noexcept;
void Flush() noexcept;
}
