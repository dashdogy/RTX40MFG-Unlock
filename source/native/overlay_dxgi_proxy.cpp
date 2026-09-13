// DXGI interface signatures follow the public Windows API. Architecture was
// informed by ReShade 6.8.0 (Patrick Mours, BSD-3-Clause OR MIT): create first,
// then own a proxy with an explicit lifetime. No ReShade code is linked.
#include "overlay_dxgi_proxy.h"
#include "overlay_native.h"
#include "overlay_dx12.h"
#include "overlay_install.h"
#include "overlay_platform.h"
#include <atomic>
#include <new>
#include <array>

namespace single_overlay::proxy {
using Microsoft::WRL::ComPtr;
namespace {
std::atomic<uint64_t> factories{0}, chains{0}, calls{0}, skipped{0};
std::atomic<uint32_t> liveFactories{0}, liveChains{0};
struct Capacity {
    std::atomic<uint32_t>& live;
    bool held=false;
    explicit Capacity(std::atomic<uint32_t>& value):live(value) {
        uint32_t count=live.load(std::memory_order_relaxed);
        while(count<64)if(live.compare_exchange_weak(count,count+1,std::memory_order_acq_rel)){held=true;break;}
    }
    ~Capacity(){if(held)--live;}
    void Transfer(){held=false;}
};
thread_local uint32_t creationDepth = 0;
struct Creation {
    bool outer = creationDepth++ == 0 && !gInsideOverlay && !native::InsideLoader();
    ~Creation() { --creationDepth; }
};
template<class T> struct Lease {
    T* object;
    explicit Lease(T* p) noexcept : object(p) { object->AddRef(); }
    ~Lease() { object->Release(); }
};
constexpr GUID factoryId = {0x95f71528,0xa89b,0x4d8a,{0xa3,0x15,0x60,0xca,0x09,0x12,0x10,0x01}};
constexpr GUID chainId = {0x95f71528,0xa89b,0x4d8a,{0xa3,0x15,0x60,0xca,0x09,0x12,0x10,0x02}};
constexpr IID factoryIids[] = {__uuidof(IDXGIFactory),__uuidof(IDXGIFactory1),__uuidof(IDXGIFactory2),
    __uuidof(IDXGIFactory3),__uuidof(IDXGIFactory4),__uuidof(IDXGIFactory5),__uuidof(IDXGIFactory6),__uuidof(IDXGIFactory7)};
constexpr IID chainIids[] = {__uuidof(IDXGISwapChain),__uuidof(IDXGISwapChain1),__uuidof(IDXGISwapChain2),
    __uuidof(IDXGISwapChain3),__uuidof(IDXGISwapChain4)};
constexpr size_t factoryLast[] = {11,13,24,25,27,28,29,31};
constexpr size_t chainLast[] = {17,28,35,39,40};
template<size_t N> int Version(REFIID iid, const IID (&known)[N]) noexcept {
    for (size_t i=0;i<N;++i) if (iid == known[i]) return static_cast<int>(i);
    return -1;
}

// Cache only interfaces actually requested by the application. Querying every
// factory revision speculatively can itself replace a native table after an
// existing overlay has initialized it. Each published interface owns a stable
// reference and is never changed while proxy methods can access it.
template<class Base,size_t N> class InterfaceCache {
    Base* root;
    const unsigned rootVersion;
    std::array<std::atomic<Base*>,N> values{};
public:
    InterfaceCache(Base* p,unsigned v):root(p),rootVersion(v){values[v].store(p);}
    ~InterfaceCache(){Clear();}
    void Clear() noexcept {
        for(size_t i=0;i<N;++i){auto* p=values[i].exchange(nullptr);if(p&&i!=rootVersion)p->Release();}
    }
    Base* Find(unsigned v) const noexcept {
        for(size_t i=v;i<N;++i)if(auto* p=values[i].load(std::memory_order_acquire))return p;
        return nullptr;
    }
    bool Acquire(unsigned v,REFIID iid,size_t last) noexcept {
        if(Find(v))return true;
        void* queried=nullptr;
        const HRESULT hr=root->QueryInterface(iid,&queried);
        if(hr!=S_OK||!queried){if(queried)static_cast<IUnknown*>(queried)->Release();return false;}
        auto* pointer=static_cast<Base*>(queried);
        if(!native::PinInterface(pointer,last)){pointer->Release();return false;}
        Base* expected=nullptr;
        if(!values[v].compare_exchange_strong(expected,pointer,std::memory_order_acq_rel))pointer->Release();
        return true;
    }
    template<class T> T* As(unsigned version) const noexcept {return static_cast<T*>(Find(version));}
};
template<class T> bool AlreadyWrapped(T* object, REFIID iid) noexcept {
    ComPtr<IUnknown> own;
    return object->QueryInterface(iid, reinterpret_cast<void**>(own.GetAddressOf())) == S_OK && own;
}

void WrapSwapchain(IDXGISwapChain** result, IUnknown* queue, IDXGIFactory* parent) noexcept;
class FactoryProxy final : public IDXGIFactory7 {
public:
    ComPtr<IDXGIFactory> original;
    InterfaceCache<IDXGIFactory,8> interfaces;
    const unsigned version;
    std::atomic<ULONG> refs{1};
    FactoryProxy(ComPtr<IDXGIFactory>&& base, unsigned v) noexcept : original(std::move(base)),interfaces(original.Get(),v),version(v) {
        ++factories;
    }
    ~FactoryProxy() { --liveFactories; }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1,std::memory_order_relaxed)+1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left=refs.fetch_sub(1,std::memory_order_acq_rel)-1;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if (!output) return E_POINTER;
        Lease lease(this);
        const int v=Version(iid,factoryIids);
        if (iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==factoryId||(v>=0&&interfaces.Acquire(unsigned(v),iid,factoryLast[v]))) {
            *output=static_cast<IDXGIFactory7*>(this); AddRef(); return S_OK;
        }
        // Preserve unknown extension contracts. Only supported DXGI interfaces
        // are advertised by the proxy; no private object layouts are inferred.
        return original->QueryInterface(iid,output);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* q,DXGI_SWAP_CHAIN_DESC* d,IDXGISwapChain** out) override {
        Lease lease(this); Creation creation;
        const HRESULT hr=original->CreateSwapChain(q,d,out);
        if (SUCCEEDED(hr)&&out&&*out&&creation.outer) WrapSwapchain(out,q,this);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* q,HWND w,const DXGI_SWAP_CHAIN_DESC1* d,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f,IDXGIOutput* o,IDXGISwapChain1** out) override {
        Lease lease(this); Creation creation;
        const HRESULT hr=interfaces.As<IDXGIFactory2>(2)->CreateSwapChainForHwnd(q,w,d,f,o,out);
        if (SUCCEEDED(hr)&&out&&*out&&creation.outer) WrapSwapchain(reinterpret_cast<IDXGISwapChain**>(out),q,this);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* q,IUnknown* w,const DXGI_SWAP_CHAIN_DESC1* d,
        IDXGIOutput* o,IDXGISwapChain1** out) override {
        Lease lease(this); Creation creation;
        const HRESULT hr=interfaces.As<IDXGIFactory2>(2)->CreateSwapChainForCoreWindow(q,w,d,o,out);
        if (SUCCEEDED(hr)&&out&&*out&&creation.outer) WrapSwapchain(reinterpret_cast<IDXGISwapChain**>(out),q,this);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* q,const DXGI_SWAP_CHAIN_DESC1* d,
        IDXGIOutput* o,IDXGISwapChain1** out) override {
        Lease lease(this); Creation creation;
        const HRESULT hr=interfaces.As<IDXGIFactory2>(2)->CreateSwapChainForComposition(q,d,o,out);
        if (SUCCEEDED(hr)&&out&&*out&&creation.outer) WrapSwapchain(reinterpret_cast<IDXGISwapChain**>(out),q,this);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->SetPrivateData(Name,DataSize,pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->SetPrivateDataInterface(Name,pUnknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->GetPrivateData(Name,pDataSize,pData); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->GetParent(riid,ppParent); }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter **ppAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->EnumAdapters(Adapter,ppAdapter); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->MakeWindowAssociation(WindowHandle,Flags); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->GetWindowAssociation(pWindowHandle); }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory>(0)->CreateSoftwareAdapter(Module,ppAdapter); }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1 **ppAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory1>(1)->EnumAdapters1(Adapter,ppAdapter); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { Lease lease(this); return interfaces.As<IDXGIFactory1>(1)->IsCurrent(); }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { Lease lease(this); return interfaces.As<IDXGIFactory2>(2)->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID *pLuid) override { Lease lease(this); return interfaces.As<IDXGIFactory2>(2)->GetSharedResourceAdapterLuid(hResource,pLuid); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override { Lease lease(this); return interfaces.As<IDXGIFactory2>(2)->RegisterStereoStatusWindow(WindowHandle,wMsg,pdwCookie); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override { Lease lease(this); return interfaces.As<IDXGIFactory2>(2)->RegisterStereoStatusEvent(hEvent,pdwCookie); }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override { Lease lease(this); return interfaces.As<IDXGIFactory2>(2)->UnregisterStereoStatus(dwCookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) override { Lease lease(this); return interfaces.As<IDXGIFactory2>(2)->RegisterOcclusionStatusWindow(WindowHandle,wMsg,pdwCookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) override { Lease lease(this); return interfaces.As<IDXGIFactory2>(2)->RegisterOcclusionStatusEvent(hEvent,pdwCookie); }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override { Lease lease(this); return interfaces.As<IDXGIFactory2>(2)->UnregisterOcclusionStatus(dwCookie); }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { Lease lease(this); return interfaces.As<IDXGIFactory3>(3)->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void **ppvAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory4>(4)->EnumAdapterByLuid(AdapterLuid,riid,ppvAdapter); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void **ppvAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory4>(4)->EnumWarpAdapter(riid,ppvAdapter); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) override { Lease lease(this); return interfaces.As<IDXGIFactory5>(5)->CheckFeatureSupport(Feature,pFeatureSupportData,FeatureSupportDataSize); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void **ppvAdapter) override { Lease lease(this); return interfaces.As<IDXGIFactory6>(6)->EnumAdapterByGpuPreference(Adapter,GpuPreference,riid,ppvAdapter); }
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD *pdwCookie) override { Lease lease(this); return interfaces.As<IDXGIFactory7>(7)->RegisterAdaptersChangedEvent(hEvent,pdwCookie); }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override { Lease lease(this); return interfaces.As<IDXGIFactory7>(7)->UnregisterAdaptersChangedEvent(dwCookie); }
};

class SwapchainProxy;
struct PresentCall {
    SwapchainProxy* object;
    PresentCall* previous;
    bool outer=true;
    static thread_local PresentCall* current;
    explicit PresentCall(SwapchainProxy* p) : object(p),previous(current) {
        for (auto* call=previous;call;call=call->previous) if(call->object==p) outer=false;
        current=this;
    }
    ~PresentCall(){current=previous;}
};
thread_local PresentCall* PresentCall::current=nullptr;

class SwapchainProxy final : public IDXGISwapChain4 {
public:
    ComPtr<IDXGISwapChain> original;
    InterfaceCache<IDXGISwapChain,5> interfaces;
    ComPtr<IDXGIFactory> parent;
    dx12::Session* renderer;
    const unsigned version;
    std::atomic<ULONG> refs{1};
    SwapchainProxy(ComPtr<IDXGISwapChain>&& base,IDXGIFactory* factory,dx12::Session* session,unsigned v) noexcept
        : original(std::move(base)),interfaces(original.Get(),v),parent(factory),renderer(session),version(v) { ++chains; }
    ~SwapchainProxy() {
        dx12::DestroySession(renderer);
        // No proxy/renderer locks are held while the existing chain executes
        // its final Release and any nested Steam/Streamline callbacks.
        interfaces.Clear(); original.Reset(); parent.Reset(); --liveChains;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1,std::memory_order_relaxed)+1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left=refs.fetch_sub(1,std::memory_order_acq_rel)-1;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if (!output) return E_POINTER;
        Lease lease(this);
        const int v=Version(iid,chainIids);
        if (iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIDeviceSubObject)
            ||iid==chainId||(v>=0&&interfaces.Acquire(unsigned(v),iid,chainLast[v]))) {
            *output=static_cast<IDXGISwapChain4*>(this); AddRef(); return S_OK;
        }
        return original->QueryInterface(iid,output);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid,void** output) override {
        Lease lease(this); return parent->QueryInterface(iid,output);
    }
    HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT flags) override {
        Lease lease(this); PresentCall call(this);
        const bool entered=call.outer&&dx12::BeginPresent(renderer,flags,false);
        const HRESULT hr=original->Present(sync,flags);
        if (entered) dx12::EndPresent(renderer,hr,flags);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params) override {
        Lease lease(this); PresentCall call(this);
        const bool partial=params&&(params->DirtyRectsCount||params->pScrollRect||params->pScrollOffset);
        const bool entered=call.outer&&dx12::BeginPresent(renderer,flags,partial);
        const HRESULT hr=interfaces.As<IDXGISwapChain1>(1)->Present1(sync,flags,params);
        if (entered) dx12::EndPresent(renderer,hr,flags);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count,UINT w,UINT h,DXGI_FORMAT format,UINT flags) override {
        Lease lease(this);
        if (!dx12::BeginResize(renderer)) return DXGI_ERROR_WAS_STILL_DRAWING;
        const HRESULT hr=original->ResizeBuffers(count,w,h,format,flags);
        dx12::EndResize(renderer,hr); return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count,UINT w,UINT h,DXGI_FORMAT format,UINT flags,
        const UINT* masks,IUnknown* const* queues) override {
        Lease lease(this);
        if (!dx12::BeginResize(renderer)) return DXGI_ERROR_WAS_STILL_DRAWING;
        const HRESULT hr=interfaces.As<IDXGISwapChain3>(3)->ResizeBuffers1(count,w,h,format,flags,masks,queues);
        if (SUCCEEDED(hr)) dx12::VerifyResizeQueues(renderer,count,queues);
        dx12::EndResize(renderer,hr); return hr;
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE color) override {
        Lease lease(this);
        const HRESULT hr=interfaces.As<IDXGISwapChain3>(3)->SetColorSpace1(color);
        if (SUCCEEDED(hr)) dx12::ObserveColorSpace(renderer,color);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->SetPrivateData(Name,DataSize,pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->SetPrivateDataInterface(Name,pUnknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->GetPrivateData(Name,pDataSize,pData); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->GetDevice(riid,ppDevice); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->GetBuffer(Buffer,riid,ppSurface); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->SetFullscreenState(Fullscreen,pTarget); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->GetFullscreenState(pFullscreen,ppTarget); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->GetDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->ResizeTarget(pNewTargetParameters); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->GetContainingOutput(ppOutput); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->GetFrameStatistics(pStats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) override { Lease lease(this); return interfaces.As<IDXGISwapChain>(0)->GetLastPresentCount(pLastPresentCount); }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->GetDesc1(pDesc); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->GetFullscreenDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->GetHwnd(pHwnd); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->GetCoreWindow(refiid,ppUnk); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->GetRestrictToOutput(ppRestrictToOutput); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->SetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->GetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->SetRotation(Rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) override { Lease lease(this); return interfaces.As<IDXGISwapChain1>(1)->GetRotation(pRotation); }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override { Lease lease(this); return interfaces.As<IDXGISwapChain2>(2)->SetSourceSize(Width,Height); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *pWidth, UINT *pHeight) override { Lease lease(this); return interfaces.As<IDXGISwapChain2>(2)->GetSourceSize(pWidth,pHeight); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override { Lease lease(this); return interfaces.As<IDXGISwapChain2>(2)->SetMaximumFrameLatency(MaxLatency); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override { Lease lease(this); return interfaces.As<IDXGISwapChain2>(2)->GetMaximumFrameLatency(pMaxLatency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { Lease lease(this); return interfaces.As<IDXGISwapChain2>(2)->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) override { Lease lease(this); return interfaces.As<IDXGISwapChain2>(2)->SetMatrixTransform(pMatrix); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) override { Lease lease(this); return interfaces.As<IDXGISwapChain2>(2)->GetMatrixTransform(pMatrix); }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { Lease lease(this); return interfaces.As<IDXGISwapChain3>(3)->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override { Lease lease(this); return interfaces.As<IDXGISwapChain3>(3)->CheckColorSpaceSupport(ColorSpace,pColorSpaceSupport); }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) override { Lease lease(this); return interfaces.As<IDXGISwapChain4>(4)->SetHDRMetaData(Type,Size,pMetaData); }
};

void WrapSwapchain(IDXGISwapChain** output,IUnknown* queue,IDXGIFactory* parent) noexcept {
    if (!output||!*output||!queue||native::InsideLoader()||liveChains.load()>=64) return;
    Capacity capacity(liveChains); if(!capacity.held)return;
    InternalScope internal;
    if (!native::PinInterface(*output,2)||AlreadyWrapped(*output,chainId)) return;
    ComPtr<IDXGISwapChain3> chain3;
    if(FAILED((*output)->QueryInterface(IID_PPV_ARGS(&chain3))))return;
    ComPtr<IDXGISwapChain> base=chain3;
    constexpr unsigned version=3;
    if(!native::PinInterface(base.Get(),chainLast[version])||slots::Fault("proxy-chain")) return;
    if (!install::PrepareInput()) return;
    auto* session=dx12::CreateSession(base.Get(),queue);
    if (!session) return; // Unknown ownership leaves the original object untouched.
    auto* proxy=new(std::nothrow) SwapchainProxy(std::move(base),parent,session,version);
    if (!proxy) { dx12::DestroySession(session); return; }
    auto* old=*output;
    capacity.Transfer();
    *output=proxy;
    old->Release(); // Transfer only the reference returned by successful creation.
    single_module::Log(L"MFG_PROXY_UI swapchain proxy attached after existing creation chain completed; nativeTableWrites=0");
}

void WrapFactory(REFIID iid,void** output) noexcept {
    if (!output||!*output||Version(iid,factoryIids)<0||liveFactories.load()>=64) return;
    Capacity capacity(liveFactories); if(!capacity.held)return;
    InternalScope internal;
    auto* returned=static_cast<IUnknown*>(*output);
    if (!native::PinInterface(returned,2)||AlreadyWrapped(returned,factoryId)) return;
    const unsigned version=static_cast<unsigned>(Version(iid,factoryIids));
    ComPtr<IDXGIFactory> base=static_cast<IDXGIFactory*>(returned);
    if(!native::PinInterface(base.Get(),factoryLast[version])||slots::Fault("proxy-factory")) return;
    auto* proxy=new(std::nothrow) FactoryProxy(std::move(base),version);
    if (!proxy) return;
    capacity.Transfer();
    *output=static_cast<IDXGIFactory7*>(proxy);
    returned->Release();
    gDxgiHooked.store(true);
    install::FactoryReady();
}
}
HRESULT FactoryCall(FactoryFn original,REFIID iid,void** output) noexcept {
    ++calls; Creation creation;
    const HRESULT hr=original(iid,output);
    if (SUCCEEDED(hr)&&creation.outer) WrapFactory(iid,output); else if (!creation.outer) ++skipped;
    return hr;
}
HRESULT FactoryCall(Factory2Fn original,UINT flags,REFIID iid,void** output) noexcept {
    ++calls; Creation creation;
    const HRESULT hr=original(flags,iid,output);
    if (SUCCEEDED(hr)&&creation.outer) WrapFactory(iid,output); else if (!creation.outer) ++skipped;
    return hr;
}
void ReadStatus(MfgSingleModuleStatus& status) noexcept {
    status.factoryGatewayCalls=calls.load();
    status.factoryWrappersCreated=factories.load();
    status.swapchainWrappersCreated=chains.load();
    status.liveFactoryWrappers=liveFactories.load();
    status.liveSwapchainWrappers=liveChains.load();
    status.internalFactoryCallsSkipped=skipped.load();
}
}
