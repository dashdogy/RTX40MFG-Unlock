#include "output_pull_telemetry.h"
#include "output_pull_experiment.h"
#include "prev2curr_experiment.h"
#include "temporal_interval_trace.h"

#include <atomic>
#include <cstdio>
#include <string>

namespace output_pull_telemetry
{
namespace
{
std::atomic<uint64_t> gCreates{0};
std::atomic<uint64_t> gProvenCreates{0};
std::atomic<uint64_t> gPrev2CurrProvenCreates{0};
std::atomic<uint64_t> gEvaluates{0};
std::atomic<uint64_t> gNonNullEvaluates{0};
std::atomic<uintptr_t> gLastHandle{0};
std::atomic<bool> gLastCreateReady{false};
std::atomic<bool> gLastPrev2CurrCreateReady{false};
std::FILE* gFile = nullptr; // Only the existing patch worker accesses the file.
uint64_t gNextFlush = 0;
DWORD gPid = 0;

std::string Utf8Json(const wchar_t* source)
{
    if (!source || !*source)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, source, -1,
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string converted(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, source, -1, converted.data(), bytes,
        nullptr, nullptr);
    converted.pop_back();
    std::string escaped;
    for (const unsigned char c : converted)
    {
        if (c == '\\' || c == '"')
            escaped.push_back('\\');
        if (c >= 0x20)
            escaped.push_back(static_cast<char>(c));
    }
    return escaped;
}
}

void Initialize(const wchar_t* directory, DWORD pid) noexcept
{
    if (gFile || !directory || !*directory)
        return;
    try
    {
        wchar_t path[MAX_PATH]{};
        const wchar_t* variant = MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT
            ? L"candidate" : L"baseline";
        if (_snwprintf_s(path, _countof(path), _TRUNCATE,
                L"%sRTX40MFG-OutputPull-%s-%lu.jsonl", directory, variant,
                static_cast<unsigned long>(pid)) < 0)
            return;
        if (_wfopen_s(&gFile, path, L"wb") != 0 || !gFile)
            return;
        gPid = pid;
        LARGE_INTEGER frequency{}, qpc{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&qpc);
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, _countof(executable));
        std::fprintf(gFile,
            "{\"schema\":1,\"event\":\"startup\",\"pid\":%lu,"
            "\"variant\":\"%s\",\"qpc\":%llu,\"qpc_frequency\":%llu,"
            "\"executable\":\"%s\",\"timing_scope\":\"fg_request_counts; "
            "frame GPU/display timing is in the matched PresentMon CSV\","
            "\"per_kernel_gpu_timing\":false,\"runtime_toggle\":false,"
            "\"prev2curr_experiment\":%s}\n",
            static_cast<unsigned long>(pid),
            MFG_UNLOCK_OUTPUT_PULL_EXPERIMENT ? "candidate" : "baseline",
            static_cast<unsigned long long>(qpc.QuadPart),
            static_cast<unsigned long long>(frequency.QuadPart),
            Utf8Json(executable).c_str(),
            MFG_UNLOCK_PREV2CURR_EXPERIMENT ? "true" : "false");
        std::fflush(gFile);
    }
    catch (...)
    {
        // A telemetry setup failure must not change FG behavior.
    }
}

void RecordCreate(bool proofReady, bool prev2CurrProofReady) noexcept
{
    gCreates.fetch_add(1, std::memory_order_relaxed);
    if (proofReady)
        gProvenCreates.fetch_add(1, std::memory_order_relaxed);
    gLastCreateReady.store(proofReady, std::memory_order_release);
    if (prev2CurrProofReady)
        gPrev2CurrProvenCreates.fetch_add(1, std::memory_order_relaxed);
    gLastPrev2CurrCreateReady.store(prev2CurrProofReady, std::memory_order_release);
}

void RecordEvaluate(const void* handle) noexcept
{
    gEvaluates.fetch_add(1, std::memory_order_relaxed);
    if (handle)
    {
        gNonNullEvaluates.fetch_add(1, std::memory_order_relaxed);
        gLastHandle.store(reinterpret_cast<uintptr_t>(handle),
            std::memory_order_relaxed);
    }
}

void Flush() noexcept
{
    const uint64_t now = GetTickCount64();
    if (!gFile || now < gNextFlush)
        return;
    gNextFlush = now + 1000;
    const auto output = midpoint_fix::ReadOutputPullSnapshot();
    const auto prev2Curr = midpoint_fix::ReadPrev2CurrSnapshot();
    const auto interval = temporal_interval_trace::ReadSnapshot();
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    uint64_t firstRequests = 0;
    for (const auto& counter : interval.firstSampleCounters)
        firstRequests += counter.samples;
    std::fprintf(gFile,
        "{\"schema\":1,\"event\":\"state\",\"pid\":%lu,\"qpc\":%llu,"
        "\"variant\":\"%s\",\"attempted\":%s,\"source_verified\":%s,"
        "\"ready\":%s,\"published\":%s,\"failure\":%u,"
        "\"provider\":\"0x%llX\",\"descriptor_entry\":\"0x%llX\","
        "\"original_fatbin\":\"0x%llX\",\"selected_fatbin\":\"0x%llX\","
        "\"source_bytes\":%u,\"selected_bytes\":%u,"
        "\"source_sha256\":\"%s\",\"selected_sha256\":\"%s\","
        "\"selected_ptx_sha256\":\"%s\",\"creates\":%llu,"
        "\"creates_with_program_proof\":%llu,\"last_create_program_ready\":%s,"
        "\"fg_evaluate_callbacks\":%llu,\"fg_nonnull_evaluate_callbacks\":%llu,"
        "\"last_feature_handle\":\"0x%llX\",\"temporal_log_enabled\":%s,"
        "\"temporal_valid_requests\":%llu,\"temporal_invalid_requests\":%llu,"
        "\"temporal_dropped_requests\":%llu,\"first_sample_requests\":%llu,"
        "\"seen_count_mask\":%u,\"seen_index_mask\":%u,"
        "\"prev2curr\":{\"candidate\":%s,\"attempted\":%s,"
        "\"source_verified\":%s,\"ready\":%s,\"published\":%s,\"failure\":%u,"
        "\"descriptor_entry\":\"0x%llX\",\"selected_fatbin\":\"0x%llX\","
        "\"source_bytes\":%u,\"selected_bytes\":%u,"
        "\"source_sha256\":\"%s\",\"selected_sha256\":\"%s\","
        "\"selected_ptx_sha256\":\"%s\",\"creates_with_program_proof\":%llu,"
        "\"last_create_program_ready\":%s}}\n",
        static_cast<unsigned long>(gPid),
        static_cast<unsigned long long>(qpc.QuadPart),
        output.candidate ? "candidate" : "baseline",
        output.attempted ? "true" : "false",
        output.sourceVerified ? "true" : "false",
        output.ready ? "true" : "false", output.published ? "true" : "false",
        output.failure, static_cast<unsigned long long>(output.provider),
        static_cast<unsigned long long>(output.descriptorEntry),
        static_cast<unsigned long long>(output.originalFatbin),
        static_cast<unsigned long long>(output.selectedFatbin),
        output.sourceBytes, output.activeBytes,
        output.sourceSha256, output.selectedSha256, output.selectedPtxSha256,
        static_cast<unsigned long long>(gCreates.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gProvenCreates.load(std::memory_order_relaxed)),
        gLastCreateReady.load(std::memory_order_acquire) ? "true" : "false",
        static_cast<unsigned long long>(gEvaluates.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gNonNullEvaluates.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(gLastHandle.load(std::memory_order_relaxed)),
        interval.enabled ? "true" : "false",
        static_cast<unsigned long long>(interval.validSamples),
        static_cast<unsigned long long>(interval.invalidSamples),
        static_cast<unsigned long long>(interval.droppedSamples),
        static_cast<unsigned long long>(firstRequests),
        interval.seenCountMask, interval.seenIndexMask,
        prev2Curr.candidate ? "true" : "false",
        prev2Curr.attempted ? "true" : "false",
        prev2Curr.sourceVerified ? "true" : "false",
        prev2Curr.ready ? "true" : "false",
        prev2Curr.published ? "true" : "false", prev2Curr.failure,
        static_cast<unsigned long long>(prev2Curr.descriptorEntry),
        static_cast<unsigned long long>(prev2Curr.selectedFatbin),
        prev2Curr.sourceBytes, prev2Curr.activeBytes,
        prev2Curr.sourceSha256, prev2Curr.selectedSha256,
        prev2Curr.selectedPtxSha256,
        static_cast<unsigned long long>(gPrev2CurrProvenCreates.load(
            std::memory_order_relaxed)),
        gLastPrev2CurrCreateReady.load(std::memory_order_acquire) ? "true" : "false");
    std::fflush(gFile);
}
}
