#include <Windows.h>
#include "caller_scoped_import.h"
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <atomic>
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
