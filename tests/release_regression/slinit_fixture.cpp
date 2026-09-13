#include <Windows.h>
#include <sl_core_types.h>
#include <atomic>
#include <dxgi1_6.h>
#include <sl_dlss_g.h>
static std::atomic<unsigned> calls{0};
static bool cacheVulkan = false;
static HMODULE vulkanLoader = nullptr;
static FARPROC cachedGipa = nullptr, cachedGdpa = nullptr, cachedCreateInstance = nullptr;
extern "C" __declspec(dllexport) void FixtureEnableVulkan(){cacheVulkan=true;}
extern "C" __declspec(dllexport) FARPROC WINAPI FixtureVulkanProc(const char* name){
    if(!vulkanLoader)return nullptr;
    if(!strcmp(name,"vkGetInstanceProcAddr"))return cachedGipa;
    if(!strcmp(name,"vkGetDeviceProcAddr"))return cachedGdpa;
    if(!strcmp(name,"vkCreateInstance"))return cachedCreateInstance;
    return GetProcAddress(vulkanLoader,name);
}
// Minimal ABI fixture, not an NVIDIA binary. It never loads a plugin/provider.
extern "C" __declspec(dllexport) sl::Result slInit(const sl::Preferences&,uint64_t) {
    ++calls;
    if(cacheVulkan){
        vulkanLoader=LoadLibraryW(L"vulkan-1.dll");
        cachedGipa=GetProcAddress(vulkanLoader,"vkGetInstanceProcAddr");
        cachedGdpa=GetProcAddress(vulkanLoader,"vkGetDeviceProcAddr");
        cachedCreateInstance=GetProcAddress(vulkanLoader,"vkCreateInstance");
    }
    return sl::Result::eOk;
}
extern "C" __declspec(dllexport) unsigned WINAPI FixtureInitCalls(){return calls.load();}
extern "C" __declspec(dllexport) sl::Result slSetD3DDevice(void*){return sl::Result::eOk;}
static unsigned lookupMode = 0;
extern "C" __declspec(dllexport) void FixtureLookupMode(unsigned mode){lookupMode=mode;}
extern "C" __declspec(dllexport) sl::Result slGetFeatureFunction(sl::Feature, const char* name, void*& function){
    HMODULE wrapper=GetModuleHandleW(L"ControlWrapperFixture.dll");
    if(!wrapper)wrapper=LoadLibraryW(L"ControlWrapperFixture.dll");
    if(!wrapper)return sl::Result::eErrorNotInitialized;
    function=reinterpret_cast<void*>(GetProcAddress(wrapper,
        name && strcmp(name,"slDLSSGGetState")==0 ? "FixtureGetState" : "FixtureSetOptions"));
    if(lookupMode==2)function=reinterpret_cast<void*>(&lookupMode);
    if(lookupMode==3)function=reinterpret_cast<void*>(GetProcAddress(wrapper,"FixtureAlternateSetOptions"));
    return lookupMode==1 ? sl::Result::eErrorInvalidParameter : sl::Result::eOk;
}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}

namespace {
HMODULE Dxgi(){wchar_t path[MAX_PATH]{};GetSystemDirectoryW(path,MAX_PATH);wcscat_s(path,L"\\dxgi.dll");return LoadLibraryW(path);}
}
// Private cached resolution inside an interposer. Only the application's
// lookup of these exported factory functions can reach the UI gateway.
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureFactory1(REFIID iid,void** out){
    using Fn=HRESULT(WINAPI*)(REFIID,void**);static auto fn=reinterpret_cast<Fn>(GetProcAddress(Dxgi(),"CreateDXGIFactory1"));return fn(iid,out);
}
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureFactory2(UINT flags,REFIID iid,void** out){
    using Fn=HRESULT(WINAPI*)(UINT,REFIID,void**);static auto fn=reinterpret_cast<Fn>(GetProcAddress(Dxgi(),"CreateDXGIFactory2"));return fn(flags,iid,out);
}
extern "C" __declspec(dllexport) HRESULT WINAPI FixtureFactory(REFIID iid,void** out){return FixtureFactory1(iid,out);}
