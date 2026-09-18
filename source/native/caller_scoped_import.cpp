#include "caller_scoped_import.h"
#include "single_module.h"
#include "protected_pointer.h"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace caller_scoped_import {
namespace {
struct Entry {
    void* target = nullptr;
    void* replacement = nullptr;
    alignas(8) std::atomic<void*> active{nullptr};
    void* relay = nullptr;
    void* trampoline = nullptr;
    bool installed = false;
    std::array<unsigned char,5> published{};
};
std::array<Entry, 9> entries; // Resolver, five input APIs, three DXGI factories.
std::mutex mutex;

const wchar_t* OwnerName(const char* name) noexcept {
    if (!name) return nullptr;
    if (!strcmp(name,"GetProcAddress")) return L"kernel32.dll";
    for (const char* factory : {"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2"})
        if (!strcmp(name,factory)) return L"dxgi.dll";
    for (const char* input : {"GetAsyncKeyState","GetKeyState","GetKeyboardState","SetCursorPos","ClipCursor"})
        if (!strcmp(name,input)) return L"user32.dll";
    return nullptr;
}
bool ImageRange(HMODULE image, uintptr_t& begin, uintptr_t& end) noexcept {
    if (!image || image!=GetModuleHandleW(nullptr)) return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x100000) return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const char*>(image)+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    begin=reinterpret_cast<uintptr_t>(image);
    const auto size=nt->OptionalHeader.SizeOfImage;
    if (!size || begin>UINTPTR_MAX-size) return false;
    end=begin+size;
    return true;
}
// Some loaders make an entire .idata page executable after startup. We never
// write these slots, so that page flag need not veto a public-entry fallback.
// Require a declared non-code PE data section and the exact named import first;
// arbitrary executable storage must not become eligible.
bool DeclaredDataImport(HMODULE image, void** slot, const char* name, size_t size) noexcept {
    if (!name) return false;
    const auto* base=reinterpret_cast<const unsigned char*>(image);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto rva=reinterpret_cast<uintptr_t>(slot)-reinterpret_cast<uintptr_t>(image);
    auto valid=[&](size_t at,size_t bytes){return at<size&&bytes<=size-at;};
    const size_t sectionOffset=size_t(dos->e_lfanew)+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+nt->FileHeader.SizeOfOptionalHeader;
    if (!nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections>96
        || !valid(sectionOffset,size_t(nt->FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER))) return false;
    bool dataSection=false;
    const auto* sections=reinterpret_cast<const IMAGE_SECTION_HEADER*>(base+sectionOffset);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        const auto& section=sections[i];
        if (rva<section.VirtualAddress || section.Misc.VirtualSize<sizeof(void*)
            || rva-section.VirtualAddress>section.Misc.VirtualSize-sizeof(void*)) continue;
        if (section.Characteristics&(IMAGE_SCN_CNT_CODE|IMAGE_SCN_MEM_EXECUTE)) return false;
        dataSection=(section.Characteristics&(IMAGE_SCN_CNT_INITIALIZED_DATA|IMAGE_SCN_CNT_UNINITIALIZED_DATA))!=0;
        break;
    }
    if (!dataSection) return false;
    const auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !valid(dir.VirtualAddress,dir.Size)
        || dir.Size>1024*sizeof(IMAGE_IMPORT_DESCRIPTOR)) return false;
    const auto* imports=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);
    for(size_t i=0;i<dir.Size/sizeof(*imports);++i) {
        const auto& imp=imports[i];
        if (!imp.Name) break;
        if (!imp.OriginalFirstThunk || !imp.FirstThunk || rva<imp.FirstThunk
            || (rva-imp.FirstThunk)%sizeof(void*)) continue;
        const auto index=(rva-imp.FirstThunk)/sizeof(void*);
        if (index>=4096) continue;
        for(size_t j=0;j<=index;++j) {
            const size_t lookup=size_t(imp.OriginalFirstThunk)+j*sizeof(IMAGE_THUNK_DATA64);
            if (!valid(lookup,sizeof(IMAGE_THUNK_DATA64))) return false;
            const auto value=reinterpret_cast<const IMAGE_THUNK_DATA64*>(base+lookup)->u1.AddressOfData;
            if (!value) break;
            if (j!=index) continue;
            if (IMAGE_SNAP_BY_ORDINAL64(value) || !valid(size_t(value),3)) return false;
            const auto* symbol=reinterpret_cast<const char*>(base+value+2);
            const size_t available=size-size_t(value)-2;
            if (!memchr(symbol,0,available<4096?available:4096)) return false;
            return !strcmp(symbol,name);
        }
    }
    return false;
}
bool MitigationsSupported() noexcept {
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY cet{};
    return GetProcessMitigationPolicy(GetCurrentProcess(),ProcessControlFlowGuardPolicy,&cfg,sizeof(cfg))
        && GetProcessMitigationPolicy(GetCurrentProcess(),ProcessUserShadowStackPolicy,&cet,sizeof(cet))
        && !cfg.StrictMode && !cfg.EnableXfg && !cet.EnableUserShadowStackStrictMode
        && !cet.BlockNonCetBinaries && !cet.BlockNonCetBinariesNonEhcont;
}
bool PublicTarget(void* target, const char* name, HMODULE& owner) noexcept {
    const wchar_t* leaf=OwnerName(name);
    if (!leaf) return false;
    owner=GetModuleHandleW(leaf);
    wchar_t actual[MAX_PATH]{},system[MAX_PATH]{};
    if (!owner || !GetModuleFileNameW(owner,actual,MAX_PATH) || !GetSystemDirectoryW(system,MAX_PATH)
        || wcscat_s(system,L"\\") || wcscat_s(system,leaf) || _wcsicmp(actual,system)
        || reinterpret_cast<void*>(GetProcAddress(owner,name))!=target) return false;
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(target,&memory,sizeof(memory))==sizeof(memory)
        && memory.AllocationBase==owner && memory.Type==MEM_IMAGE && memory.State==MEM_COMMIT
        && !(memory.Protect&(PAGE_GUARD|PAGE_NOACCESS))
        && (memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
bool Current(const Entry& e) noexcept {
    return e.installed && !memcmp(e.target,e.published.data(),e.published.size());
}
// Tail dispatch preserves RCX/RDX/R8/R9, all stack arguments and the original
// caller return address. RAX/R10 and flags are volatile in the Windows x64 ABI.
// Each relay and its aligned activation pointer have process lifetime.
bool MakeRelay(Entry& entry, uintptr_t begin, uintptr_t end) noexcept {
    auto* code=static_cast<unsigned char*>(entry.relay);
    if (!code) return false;
    size_t at=0;
    auto bytes=[&](std::initializer_list<unsigned char> data){for(auto b:data)code[at++]=b;};
    auto qword=[&](uintptr_t value){memcpy(code+at,&value,8);at+=8;};
    bytes({0xf3,0x0f,0x1e,0xfa,0x48,0x8b,0x04,0x24}); // endbr64; mov rax,[rsp]
    bytes({0x49,0xba});qword(begin);bytes({0x4c,0x39,0xd0,0x72});const auto below=at++;
    bytes({0x49,0xba});qword(end);bytes({0x4c,0x39,0xd0,0x73});const auto above=at++;
    bytes({0x49,0xba});qword(reinterpret_cast<uintptr_t>(&entry.active));
    bytes({0x49,0x8b,0x02,0x48,0x85,0xc0,0x74});const auto inactive=at++;
    bytes({0xff,0xe0});
    const auto fallback=at;
    code[below]=static_cast<unsigned char>(fallback-below-1);
    code[above]=static_cast<unsigned char>(fallback-above-1);
    code[inactive]=static_cast<unsigned char>(fallback-inactive-1);
    bytes({0x48,0xb8});qword(reinterpret_cast<uintptr_t>(entry.trampoline));bytes({0xff,0xe0});
    DWORD previous=0,observed=0;
    return VirtualProtect(code,4096,PAGE_EXECUTE_READ,&previous)
        && protected_pointer::QueryProtection(reinterpret_cast<uintptr_t>(code),observed,at)
        && observed==PAGE_EXECUTE_READ && FlushInstructionCache(GetCurrentProcess(),code,at);
}
}

bool Eligible(HMODULE importer, void** slot, void* expected, const char* name) noexcept {
    uintptr_t begin=0,end=0;
    const auto address=reinterpret_cast<uintptr_t>(slot);
    if (!expected || (address%8)!=4 || !ImageRange(importer,begin,end)
        || address<begin || address>end-sizeof(void*)) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(slot,&memory,sizeof(memory))!=sizeof(memory) || memory.AllocationBase!=importer
        || memory.Type!=MEM_IMAGE || memory.State!=MEM_COMMIT
        || (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS))
        || memory.RegionSize<sizeof(void*)
        || address-reinterpret_cast<uintptr_t>(memory.BaseAddress)>memory.RegionSize-sizeof(void*)) return false;
    const auto* ownerName=OwnerName(name);
    const bool factory=ownerName && !wcscmp(ownerName,L"dxgi.dll");
    if ((factory || (memory.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
        && !DeclaredDataImport(importer,slot,name,end-begin)) return false;
    void* current=nullptr;
    memcpy(&current,slot,sizeof(current));
    HMODULE owner=nullptr;
    return current==expected && PublicTarget(expected,name,owner);
}

bool Prepare(HMODULE importer, void** slot, void* expected, void* replacement, const char* name, void*& original) noexcept {
    original=nullptr;
    auto fail=[&](const wchar_t* stage,unsigned code=0){
        wchar_t line[256]{};swprintf_s(line,L"MFG_UNALIGNED_IMPORT unavailable symbol=%hs stage=%s code=%u",name?name:"(null)",stage,code);
        single_module::Log(line);return false;
    };
    if (!replacement || replacement==expected || !Eligible(importer,slot,expected,name)) return fail(L"eligibility");
    if (!MitigationsSupported()) return fail(L"mitigation");
    uintptr_t begin=0,end=0;
    if (!ImageRange(importer,begin,end)) return false;
    std::lock_guard lock(mutex);
    Entry* entry=nullptr;
    for (auto& e:entries) {
        if (e.target==expected) {
            if (e.replacement!=replacement || !Current(e)) return false;
            original=e.trampoline;
            return true;
        }
        if (!e.target && !entry) entry=&e;
    }
    if (!entry) return false;
    HMODULE owner=nullptr,pinned=nullptr;
    if (!PublicTarget(expected,name,owner)
        || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(expected),&pinned) || pinned!=owner) return false;
    const auto init=MH_Initialize();
    if (init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return fail(L"initialize",init);
    entry->target=expected;
    entry->replacement=replacement;
    // MinHook requires an executable relay at creation time. Prepare a valid
    // inactive relay, then finalize its trampoline while the hook is disabled.
    entry->relay=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!entry->relay) return fail(L"relay-allocation",GetLastError());
    entry->trampoline=expected;
    if (!MakeRelay(*entry,begin,end)) return fail(L"relay-preparation",GetLastError());
    const auto create=MH_CreateHook(expected,entry->relay,&entry->trampoline);
    if (create!=MH_OK || !entry->trampoline) return fail(L"decode",create);
    // Publish the complete pass-through trampoline before enabling the entry.
    DWORD relayProtection=0;
    if (!VirtualProtect(entry->relay,4096,PAGE_READWRITE,&relayProtection)) {
        MH_RemoveHook(expected);return fail(L"relay-finalization",GetLastError());
    }
    if (!MakeRelay(*entry,begin,end)) { MH_RemoveHook(expected); return fail(L"relay-publication",GetLastError()); }
    DWORD originalProtection=0;
    if (!protected_pointer::QueryProtection(reinterpret_cast<uintptr_t>(expected),originalProtection,8)) {
        MH_RemoveHook(expected); return false;
    }
    const auto enabled=MH_EnableHook(expected);
    if (enabled!=MH_OK) { MH_RemoveHook(expected); return fail(L"entry-publication",enabled); }
    memcpy(entry->published.data(),expected,entry->published.size());
    entry->installed=true;
    if (!protected_pointer::ProtectionMatches(reinterpret_cast<uintptr_t>(expected),originalProtection,8)
        || !Eligible(importer,slot,expected,name) || !Current(*entry)) return fail(L"verification");
    original=entry->trampoline;
    wchar_t line[256]{};
    swprintf_s(line,L"MFG_UNALIGNED_IMPORT prepared symbol=%hs caller=main-executable slotRva=0x%llX slotWrites=0",name,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(slot)-begin));
    single_module::Log(line);
    return true;
}
bool Activate(void* expected, void* replacement) noexcept {
    std::lock_guard lock(mutex);
    for(auto& e:entries)if(e.target==expected&&e.replacement==replacement&&Current(e)) {
        e.active.store(replacement,std::memory_order_release);
        return true;
    }
    return false;
}
bool Deactivate(void* expected, void* replacement) noexcept {
    std::lock_guard lock(mutex);
    for (auto& e:entries) if (e.target==expected && e.replacement==replacement) {
        void* active=replacement;
        e.active.compare_exchange_strong(active,nullptr,std::memory_order_acq_rel);
        return e.active.load(std::memory_order_acquire)==nullptr;
    }
    return false;
}
}
