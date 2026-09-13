// Included only by fault_capture.cpp in the isolated result-logging build.
// Uses existing runtime post-call observations and provider entry observations.
// No device creation, worker, DRED configuration, provider post-call detour,
// DLL loading, output writes or NGX parameter/handle-content inspection.
#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace fault_capture {
namespace {
constexpr uint64_t kLimit=16;
std::atomic<LogCallback> logSink{nullptr};
std::atomic<uint64_t> nextCall{0}, nextProvider{0};
thread_local uint64_t parentCall=0;
struct PreserveError { DWORD value=GetLastError(); ~PreserveError(){SetLastError(value);} };
void Emit(const wchar_t* format,...) noexcept {
    PreserveError preserve;
    auto log=logSink.load(std::memory_order_acquire);if(!log)return;
    wchar_t line[640]{};va_list args;va_start(args,format);
    _vsnwprintf_s(line,_countof(line),_TRUNCATE,format,args);va_end(args);log(line);
}
bool ReadOutput(void* const* source,void*& value) noexcept {
    if(!source)return false;
    __try {value=*source;return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){value=nullptr;return false;}
}
bool CreateKind(entry_detour::Kind kind) noexcept {
    return kind==entry_detour::Kind::eNgxRuntimeD3D12CreateFeature
        ||kind==entry_detour::Kind::eNgxD3D12CreateFeature;
}
}
void Initialize(LogCallback log) noexcept {
    PreserveError preserve;logSink.store(log,std::memory_order_release);
    Emit(L"NGX_RESULT_LITE enabled=1 runtimeCreateLimit=16 providerEntryLimit=16 providerResults=entry-only newHooks=0 DRED=0");
}
void BeforeSlInit() noexcept {}
void ObserveDevice(void*,bool) noexcept {}
FARPROC ResolveProc(HMODULE,LPCSTR,FARPROC original) noexcept {return original;}
NgxCall::NgxCall(entry_detour::Handle handle,const void* caller,const void* command,
    uintptr_t feature,const void* parameters) noexcept {
    PreserveError preserve;
    if(feature!=11)return; // NVSDK_NGX_Feature_FrameGeneration, not a parameter heuristic.
    const auto snapshot=entry_detour::ReadSnapshot(handle);
    if(!snapshot.current||!CreateKind(snapshot.kind))return;
    const auto id=nextCall.fetch_add(1,std::memory_order_relaxed)+1;if(id>kLimit)return;
    id_=id;parent_=parentCall;parentCall=id;entry_=snapshot;caller_=caller;
    command_=command;parameters_=parameters;handleOrFeature_=feature;
    Emit(L"NGX_RESULT_LITE event=begin id=%llu parent=%llu tid=%u kind=%u owner=%p generation=%llu rva=0x%X command=%p parameters=%p caller=%p",
        id_,parent_,GetCurrentThreadId(),static_cast<uint32_t>(entry_.kind),entry_.owner,
        entry_.generation,entry_.targetRva,command_,parameters_,caller_);
}
void NgxCall::Complete(uint32_t result,void* const* output) noexcept {
    PreserveError preserve;if(!id_)return;
    void* handle=nullptr;
    const bool readable=result==1&&ReadOutput(output,handle);
    Emit(L"NGX_RESULT_LITE event=return id=%llu parent=%llu tid=%u kind=%u owner=%p rva=0x%X result=0x%08X success=%u outputReadable=%u handle=%p",
        id_,parent_,GetCurrentThreadId(),static_cast<uint32_t>(entry_.kind),entry_.owner,
        entry_.targetRva,result,result==1,readable,handle);
    completed_=true;
}
NgxCall::~NgxCall(){
    PreserveError preserve;if(!id_)return;
    if(!completed_)Emit(L"NGX_RESULT_LITE event=unwound id=%llu parent=%llu",id_,parent_);
    parentCall=parent_;
}
void ProviderForward(entry_detour::Handle handle,const void* caller,const void* command,
    uintptr_t feature,const void* parameters) noexcept {
    PreserveError preserve;if(feature!=11)return;
    const auto entry=entry_detour::ReadSnapshot(handle);
    if(!entry.current||entry.kind!=entry_detour::Kind::eNgxD3D12CreateFeature)return;
    const auto id=nextProvider.fetch_add(1,std::memory_order_relaxed)+1;if(id>kLimit)return;
    Emit(L"NGX_RESULT_LITE event=provider-entry id=%llu parent=%llu tid=%u owner=%p generation=%llu rva=0x%X command=%p parameters=%p caller=%p result=unobserved",
        id,parentCall,GetCurrentThreadId(),entry.owner,entry.generation,entry.targetRva,command,parameters,caller);
}
}
