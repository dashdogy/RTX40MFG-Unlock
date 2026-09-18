// Independent, pinned overlay fixture. Its native inline detours model the
// Steam E9 -> absolute-relay shape observed in Spider-Man. The candidate must
// leave these bytes intact and continue calling this overlay.
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <tuple>
#include <mutex>
#include "fixture_api.h"
#include "MinHook.h"
using Microsoft::WRL::ComPtr;
#include "outer_chain.h"
#include "layered_chain.h"
namespace {
std::atomic<unsigned long long> counts[16]{};
void* targets[13]{};
unsigned char patched[13][16]{};
void** lifecycle[10]{};
void* beforeSlots[10]{};
DWORD beforeProtection[10]{};
std::mutex presentMutex;
FixturePresent lastPresent{};
std::atomic<bool> forceStillDrawing{false};
thread_local unsigned inCreation=0;
std::atomic<bool> releaseBindingReady{true};
bool NativeSlotsUnchanged(){for(unsigned i=0;i<10;++i)if(!lifecycle[i]||*lifecycle[i]!=beforeSlots[i])return false;return true;}
struct Creating {
    Creating(){++inCreation;const bool ready=NativeSlotsUnchanged();releaseBindingReady.store(ready);if(!ready)++counts[13];}
    ~Creating(){--inCreation;}
};
template<int N, class R, class... A> struct Hook {
    using Fn = R (WINAPI*)(A...);
    inline static Fn original = nullptr;
    static R WINAPI Call(A... a) {
        ++counts[N];
        // Model a stateful overlay which declines initialization on a foreign
        // native table, then receives Release recursively during creation.
        // An unavailable binding is recorded instead of executing null.
        if constexpr(N==4) {
            Creating creation;
            const R result=wrapperMode.load()==4?CreateLayered(original,a...):original(a...);
            const auto args=std::forward_as_tuple(a...);
            auto** out=std::get<6>(args);
            if(SUCCEEDED(result)&&out&&*out){
                ComPtr<IUnknown> inner;
                if(wrapperMode.load()==4)(*out)->QueryInterface(nativeGuid,reinterpret_cast<void**>(inner.GetAddressOf()));
                IUnknown* native=inner?inner.Get():*out;
                (*out)->SetPrivateData(kFixtureNativeIdentity,sizeof(native),&native);
                native->AddRef();
                native->Release();
                ++counts[15];
                if(wrapperMode.load()!=4)WrapOuter(out);
            }
            return result;
        }
        if constexpr(N==12) {
            if(inCreation&&!releaseBindingReady.load())++counts[14];
        }
        R result;
        if constexpr(N==7||N==8)result=forceStillDrawing.load()?DXGI_ERROR_WAS_STILL_DRAWING:original(a...);
        else result=original(a...);
        if constexpr(N==7||N==8) {
            const auto args=std::forward_as_tuple(a...);
            FixturePresent value{};value.chain=std::get<0>(args);value.sync=std::get<1>(args);value.flags=std::get<2>(args);value.result=result;value.kind=N;
            if constexpr(N==8){value.metadata=std::get<3>(args);if(value.metadata){value.dirty=value.metadata->pDirtyRects;value.scroll=value.metadata->pScrollRect;value.offset=value.metadata->pScrollOffset;value.dirtyCount=value.metadata->DirtyRectsCount;}}
            std::lock_guard lock(presentMutex);lastPresent=value;
        }
        return result;
    }
    static bool Install(void* target) {
        targets[N] = target;
        return MH_CreateHook(target, reinterpret_cast<void*>(&Call), reinterpret_cast<void**>(&original)) == MH_OK
            && MH_EnableHook(target) == MH_OK;
    }
};
using F0=Hook<0,HRESULT,REFIID,void**>;
using F1=Hook<1,HRESULT,REFIID,void**>;
using F2=Hook<2,HRESULT,UINT,REFIID,void**>;
using C0=Hook<3,HRESULT,IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**>;
using C1=Hook<4,HRESULT,IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**>;
using C2=Hook<5,HRESULT,IDXGIFactory2*,IUnknown*,IUnknown*,const DXGI_SWAP_CHAIN_DESC1*,IDXGIOutput*,IDXGISwapChain1**>;
using C3=Hook<6,HRESULT,IDXGIFactory2*,IUnknown*,const DXGI_SWAP_CHAIN_DESC1*,IDXGIOutput*,IDXGISwapChain1**>;
using P=Hook<7,HRESULT,IDXGISwapChain*,UINT,UINT>;
using P1=Hook<8,HRESULT,IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*>;
using R=Hook<9,HRESULT,IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT>;
using R1=Hook<10,HRESULT,IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown* const*>;
using Color=Hook<11,HRESULT,IDXGISwapChain3*,DXGI_COLOR_SPACE_TYPE>;
using Release=Hook<12,ULONG,IUnknown*>;
}
extern "C" __declspec(dllexport) BOOL WINAPI OverlayFixtureInstall() {
    HMODULE self=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&OverlayFixtureInstall),&self)) return FALSE;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
        || FAILED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) return FALSE;
    D3D12_COMMAND_QUEUE_DESC q{};
    if (FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))) return FALSE;
    WNDCLASSW wc{}; wc.hInstance=self; wc.lpszClassName=L"ExistingOverlayFixture"; wc.lpfnWndProc=DefWindowProcW;
    if (!RegisterClassW(&wc)) return FALSE;
    HWND window=CreateWindowW(wc.lpszClassName,L"",WS_POPUP,0,0,320,240,nullptr,nullptr,self,nullptr);
    if (!window) return FALSE;
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=320;desc.Height=240;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain; ComPtr<IDXGISwapChain3> chain3;
    bool ok=SUCCEEDED(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&chain))&&SUCCEEDED(chain.As(&chain3));
    if (ok) {
        auto ft=*reinterpret_cast<void***>(factory.Get());
        auto st=*reinterpret_cast<void***>(chain3.Get());
        void** slots[]={&ft[10],&ft[15],&ft[16],&ft[24],&st[8],&st[22],&st[13],&st[39],&st[38],&st[2]};
        for (unsigned i=0;i<10;++i) {lifecycle[i]=slots[i];beforeSlots[i]=*slots[i];MEMORY_BASIC_INFORMATION m{};VirtualQuery(slots[i],&m,sizeof(m));beforeProtection[i]=m.Protect;}
        HMODULE dxgi=GetModuleHandleW(L"dxgi.dll");
        auto proc=[&](const char* n){return reinterpret_cast<void*>(GetProcAddress(dxgi,n));};
        ok=MH_Initialize()==MH_OK && F0::Install(proc("CreateDXGIFactory")) && F1::Install(proc("CreateDXGIFactory1"))
          &&F2::Install(proc("CreateDXGIFactory2"))&&C0::Install(ft[10])&&C1::Install(ft[15])&&C2::Install(ft[16])&&C3::Install(ft[24])
          &&P::Install(st[8])&&P1::Install(st[22])&&R::Install(st[13])&&R1::Install(st[39])&&Color::Install(st[38])&&Release::Install(st[2]);
        if(ok) for(unsigned i=0;i<13;++i) memcpy(patched[i],targets[i],16);
    }
    chain3.Reset();chain.Reset();DestroyWindow(window);UnregisterClassW(wc.lpszClassName,self);
    return ok;
}
extern "C" __declspec(dllexport) BOOL WINAPI OverlayFixtureMethodCodeIntact() {
    for(unsigned i=3;i<13;++i)if(!targets[i]||memcmp(patched[i],targets[i],16))return FALSE;
    return TRUE;
}
extern "C" __declspec(dllexport) BOOL WINAPI OverlayFixtureCodeIntact() {
    for(unsigned i=0;i<13;++i) if(!targets[i]||memcmp(patched[i],targets[i],16)) return FALSE;
    return TRUE;
}
extern "C" __declspec(dllexport) BOOL WINAPI OverlayFixtureSlotsRestored() {
    for(unsigned i=0;i<10;++i) {MEMORY_BASIC_INFORMATION m{};if(!lifecycle[i]||*lifecycle[i]!=beforeSlots[i]||!VirtualQuery(lifecycle[i],&m,sizeof(m))||m.Protect!=beforeProtection[i])return FALSE;}
    return TRUE;
}
extern "C" __declspec(dllexport) void WINAPI OverlayFixtureResetCounts() {
    for(auto& n:counts)n.store(0);
}
extern "C" __declspec(dllexport) unsigned long long WINAPI OverlayFixtureCount(unsigned index) {
    return index<16?counts[index].load():0;
}
extern "C" __declspec(dllexport) void WINAPI OverlayFixtureReadPresent(FixturePresent* value) {
    std::lock_guard lock(presentMutex);if(value)*value=lastPresent;
}

extern "C" __declspec(dllexport) void WINAPI OverlayFixtureStillDrawing(BOOL enabled){forceStillDrawing.store(enabled!=FALSE);}
