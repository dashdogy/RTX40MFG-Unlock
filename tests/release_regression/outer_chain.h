#pragma once
// Independent public COM wrapper fixture: forwarding-only, with the documented
// NVIDIA base-interface GUID. It models no private Streamline implementation.
inline std::atomic<unsigned> wrapperMode{0}, liveOuterChains{0};
inline std::atomic<IUnknown*> releaseOnPresent{nullptr};
constexpr GUID nativeGuid={0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
constexpr GUID reshadeGuid={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};
class OuterChain:public IDXGISwapChain4 {
protected:
    ComPtr<IDXGISwapChain4> native;
    std::atomic<ULONG> refs{1};
    const unsigned mode;
public:
    OuterChain(ComPtr<IDXGISwapChain4> p,unsigned m):native(std::move(p)),mode(m){++liveOuterChains;}
    virtual ~OuterChain(){--liveOuterChains;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{const auto left=--refs;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(iid==nativeGuid){
            if(mode==2){*out=static_cast<IDXGISwapChain4*>(this);AddRef();return S_OK;}
            if(mode==3||mode==5)return E_NOINTERFACE;
            return native->QueryInterface(__uuidof(IUnknown),out);
        }
        if(iid==reshadeGuid&&mode==5)return native->QueryInterface(__uuidof(IUnknown),out);
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIDeviceSubObject)
            ||iid==__uuidof(IDXGISwapChain)||iid==__uuidof(IDXGISwapChain1)||iid==__uuidof(IDXGISwapChain2)
            ||iid==__uuidof(IDXGISwapChain3)||iid==__uuidof(IDXGISwapChain4)){
            *out=static_cast<IDXGISwapChain4*>(this);AddRef();return S_OK;
        }
        return native->QueryInterface(iid,out);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override {  return native->SetPrivateData(Name,DataSize,pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override {  return native->SetPrivateDataInterface(Name,pUnknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override {  return native->GetPrivateData(Name,pDataSize,pData); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {  return native->GetParent(riid,ppParent); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) override {  return native->GetDevice(riid,ppDevice); }
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override { if(auto* p=releaseOnPresent.exchange(nullptr))p->Release(); return native->Present(SyncInterval,Flags); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) override {  return native->GetBuffer(Buffer,riid,ppSurface); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) override {  return native->SetFullscreenState(Fullscreen,pTarget); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) override {  return native->GetFullscreenState(pFullscreen,ppTarget); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) override {  return native->GetDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override {  return native->ResizeBuffers(BufferCount,Width,Height,NewFormat,SwapChainFlags); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) override {  return native->ResizeTarget(pNewTargetParameters); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) override {  return native->GetContainingOutput(ppOutput); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) override {  return native->GetFrameStatistics(pStats); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) override {  return native->GetLastPresentCount(pLastPresentCount); }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) override {  return native->GetDesc1(pDesc); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) override {  return native->GetFullscreenDesc(pDesc); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) override {  return native->GetHwnd(pHwnd); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) override {  return native->GetCoreWindow(refiid,ppUnk); }
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters) override { if(auto* p=releaseOnPresent.exchange(nullptr))p->Release(); return native->Present1(SyncInterval,PresentFlags,pPresentParameters); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override {  return native->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) override {  return native->GetRestrictToOutput(ppRestrictToOutput); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) override {  return native->SetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) override {  return native->GetBackgroundColor(pColor); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override {  return native->SetRotation(Rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) override {  return native->GetRotation(pRotation); }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override {  return native->SetSourceSize(Width,Height); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *pWidth, UINT *pHeight) override {  return native->GetSourceSize(pWidth,pHeight); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {  return native->SetMaximumFrameLatency(MaxLatency); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override {  return native->GetMaximumFrameLatency(pMaxLatency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {  return native->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) override {  return native->SetMatrixTransform(pMatrix); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) override {  return native->GetMatrixTransform(pMatrix); }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {  return native->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override {  return native->CheckColorSpaceSupport(ColorSpace,pColorSpaceSupport); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override {  return native->SetColorSpace1(ColorSpace); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT *pCreationNodeMask, IUnknown *const *ppPresentQueue) override {  return native->ResizeBuffers1(BufferCount,Width,Height,Format,SwapChainFlags,pCreationNodeMask,ppPresentQueue); }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) override {  return native->SetHDRMetaData(Type,Size,pMetaData); }
};
inline void WrapOuter(IDXGISwapChain1** output){
    const auto mode=wrapperMode.load();if(!mode||!output||!*output)return;
    ComPtr<IDXGISwapChain4> native;if(FAILED((*output)->QueryInterface(IID_PPV_ARGS(&native))))return;
    auto* wrapped=new OuterChain(native,mode);(*output)->Release();*output=wrapped;
}
extern "C" __declspec(dllexport) void WINAPI OverlayFixtureWrapper(unsigned mode){wrapperMode.store(mode);}
extern "C" __declspec(dllexport) unsigned WINAPI OverlayFixtureLiveOuter(){return liveOuterChains.load();}
extern "C" __declspec(dllexport) void WINAPI OverlayFixtureReleaseOnPresent(IUnknown* p){releaseOnPresent.store(p);}
