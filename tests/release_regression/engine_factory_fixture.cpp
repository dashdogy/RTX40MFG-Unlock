#include <Windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <cstring>
extern "C" __declspec(dllexport) HRESULT WINAPI EngineCreateFactory(unsigned version,REFIID iid,void** result) {
    if (version==0) return CreateDXGIFactory(iid,result);
    if (version==2) return CreateDXGIFactory2(0,iid,result);
    return CreateDXGIFactory1(iid,result);
}
extern "C" __declspec(dllexport) FARPROC WINAPI EngineLookup(HMODULE module,LPCSTR name) {
    return GetProcAddress(module,name);
}
// Keep a genuine device import even in the dynamic-factory case. This function
// is exported, but tests do not create devices through it.
extern "C" __declspec(dllexport) HRESULT WINAPI EngineCreateDevice(IUnknown* adapter,REFIID iid,void** result) {
    return D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,iid,result);
}
namespace {
using Factory=HRESULT(WINAPI*)(REFIID,void**);
Factory original=nullptr;
HRESULT WINAPI ForeignFactory(REFIID iid,void** result) { return original(iid,result); }
}
extern "C" __declspec(dllexport) bool WINAPI EngineInstallForeignFactory() {
    HMODULE self=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&EngineCreateFactory),&self))return false;
    auto* base=reinterpret_cast<unsigned char*>(self);
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);
    for(size_t i=0;imports[i].Name;++i) {
        if(!imports[i].OriginalFirstThunk)continue;
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA64*>(base+imports[i].OriginalFirstThunk);
        auto** slots=reinterpret_cast<void**>(base+imports[i].FirstThunk);
        for(size_t j=0;names[j].u1.AddressOfData;++j) {
            const auto rva=names[j].u1.AddressOfData;
            if(IMAGE_SNAP_BY_ORDINAL64(rva))continue;
            if(strcmp(reinterpret_cast<const char*>(base+rva+2),"CreateDXGIFactory1"))continue;
            original=reinterpret_cast<Factory>(slots[j]);
            DWORD protection=0,ignored=0;
            if(!VirtualProtect(slots+j,sizeof(void*),PAGE_READWRITE,&protection))return false;
            void* previous=InterlockedCompareExchangePointer(slots+j,reinterpret_cast<void*>(&ForeignFactory),reinterpret_cast<void*>(original));
            return VirtualProtect(slots+j,sizeof(void*),protection,&ignored) && previous==reinterpret_cast<void*>(original);
        }
    }
    return false;
}
