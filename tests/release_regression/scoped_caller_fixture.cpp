#include <Windows.h>
#include "caller_scoped_import.h"
#include "overlay_slots.h"
#include <dxgi1_6.h>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <atomic>
#include <MinHook.h>
using Lookup=FARPROC(WINAPI*)(HMODULE,LPCSTR);
static std::atomic<Lookup> original{nullptr};
namespace single_module { void Log(const wchar_t* text) { std::wprintf(L"%ls\n",text); } }
static FARPROC WINAPI ScopedLookup(HMODULE module,LPCSTR name) {
    if(reinterpret_cast<uintptr_t>(name)>0xffff&&!strcmp(name,"MfgScopedFixtureSentinel"))return reinterpret_cast<FARPROC>(uintptr_t(0x12345678));
    return original.load(std::memory_order_acquire)(module,name);
}
static FARPROC WINAPI ConflictingLookup(HMODULE,LPCSTR){return nullptr;}
extern "C" __declspec(dllexport) BOOL WINAPI TryScopedImport(HMODULE importer,void** slot,void* expected,const char* name,BOOL conflict) {
    void* trampoline=nullptr;
    void* replacement=reinterpret_cast<void*>(conflict?ConflictingLookup:ScopedLookup);
    if(!caller_scoped_import::Prepare(importer,slot,expected,replacement,name,trampoline))return FALSE;
    original.store(reinterpret_cast<Lookup>(trampoline),std::memory_order_release);
    return caller_scoped_import::Activate(expected,replacement);
}
extern "C" __declspec(dllexport) BOOL WINAPI DeactivateScopedImport(void* expected) {
    return caller_scoped_import::Deactivate(expected,reinterpret_cast<void*>(ScopedLookup));
}
extern "C" __declspec(dllexport) FARPROC WINAPI ForeignLookup(HMODULE module,LPCSTR name) {
    FARPROC volatile result=GetProcAddress(module,name); // Keep this fixture's return address.
    return result;
}
extern "C" __declspec(dllexport) BOOL WINAPI ForeignClip(const RECT* clip) {
    BOOL volatile result=ClipCursor(clip);
    return result;
}

namespace {
const char* factoryNames[]{"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2"};
std::atomic<void*> factoryOriginals[3]{};
std::atomic<unsigned> factoryCalls[3]{};
bool failFactoryPublication=false;
HRESULT WINAPI Factory0(REFIID iid,void** out){++factoryCalls[0];return reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(factoryOriginals[0].load())(iid,out);}
HRESULT WINAPI Factory1(REFIID iid,void** out){++factoryCalls[1];return reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(factoryOriginals[1].load())(iid,out);}
HRESULT WINAPI Factory2(UINT flags,REFIID iid,void** out){++factoryCalls[2];return reinterpret_cast<HRESULT(WINAPI*)(UINT,REFIID,void**)>(factoryOriginals[2].load())(flags,iid,out);}
void* replacements[]{reinterpret_cast<void*>(&Factory0),reinterpret_cast<void*>(&Factory1),reinterpret_cast<void*>(&Factory2)};
bool PublishFactory(const char* name,void*,void* trampoline) noexcept {
    for(unsigned i=0;i<3;++i)if(!strcmp(name,factoryNames[i])){
        if(failFactoryPublication&&i==2)return false;
        factoryOriginals[i].store(trampoline,std::memory_order_release);return true;
    }
    return false;
}
}
extern "C" __declspec(dllexport) BOOL WINAPI TryFactoryBatch(HMODULE importer,void*** addresses,BOOL fail) {
    failFactoryPublication=fail!=FALSE;
    single_overlay::slots::Batch batch;
    for(unsigned i=0;i<3;++i){
        void* expected=nullptr;memcpy(&expected,addresses[i],sizeof(expected));
        if(!batch.Add(importer,addresses[i],expected,replacements[i],factoryNames[i],&PublishFactory))return FALSE;
    }
    return batch.Publish();
}
extern "C" __declspec(dllexport) unsigned WINAPI ScopedFactoryCount(unsigned api) {return api<3?factoryCalls[api].load():0;}
extern "C" __declspec(dllexport) HRESULT WINAPI ForeignFactory(unsigned api,UINT flags,REFIID iid,void** out) {
    // Preserve this DLL's native return address; no tail dispatch to DXGI.
    HRESULT volatile result=api==0?CreateDXGIFactory(iid,out):api==1?CreateDXGIFactory1(iid,out):CreateDXGIFactory2(flags,iid,out);
    return result;
}

// Identify this foreign caller as middleware so application renderer discovery
// does not intentionally instrument it during caller-transparency checks.
extern "C" __declspec(dllexport) BOOL slOnPluginStartup(){return FALSE;}
using Parent=HRESULT(STDMETHODCALLTYPE*)(IDXGIObject*,REFIID,void**);
static Parent parentOriginal=nullptr;
static std::atomic<unsigned> parentCalls{0};
static HRESULT STDMETHODCALLTYPE ParentOverlay(IDXGIObject* object,REFIID iid,void** output) {
    ++parentCalls;return parentOriginal(object,iid,output);
}
extern "C" __declspec(dllexport) HRESULT WINAPI ForeignParent(IDXGIObject* object,REFIID iid,void** output) {
    HRESULT volatile result=object->GetParent(iid,output);return result;
}
extern "C" __declspec(dllexport) BOOL WINAPI InstallParentOverlay(IDXGIObject* object) {
    const auto initialized=MH_Initialize();if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED)return FALSE;
    auto* target=(*reinterpret_cast<void***>(object))[6];
    return MH_CreateHook(target,reinterpret_cast<void*>(&ParentOverlay),reinterpret_cast<void**>(&parentOriginal))==MH_OK
        && MH_EnableHook(target)==MH_OK;
}
extern "C" __declspec(dllexport) unsigned WINAPI ParentOverlayCalls(){return parentCalls.load();}
