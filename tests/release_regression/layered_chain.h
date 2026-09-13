#pragma once
#include <vector>
// Public-API model of an application rendering off-screen while another
// queue copies to a native swapchain. This is not a Streamline implementation.
inline std::atomic<uint64_t> layeredCounts[5]{};
inline std::atomic<unsigned> layeredBufferFault{0};
// A conforming resource interface whose device query fails. The renderer
// must reject unavailable ownership before creating descriptors or commands.
class UnknownOwnerResource final:public ID3D12Resource {
    ComPtr<ID3D12Resource> resource;std::atomic<ULONG> refs{1};
public:
    explicit UnknownOwnerResource(ID3D12Resource* r):resource(r){}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{const auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override{
        if(!out)return E_POINTER;*out=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(ID3D12Object)||iid==__uuidof(ID3D12DeviceChild)
            ||iid==__uuidof(ID3D12Pageable)||iid==__uuidof(ID3D12Resource)){
            *out=static_cast<ID3D12Resource*>(this);AddRef();return S_OK;}
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g,UINT* n,void* p) override{return resource->GetPrivateData(g,n,p);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g,UINT n,const void* p) override{return resource->SetPrivateData(g,n,p);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g,const IUnknown* p) override{return resource->SetPrivateDataInterface(g,p);}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override{return resource->SetName(n);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void** out) override{if(!out)return E_POINTER;*out=nullptr;return E_NOINTERFACE;}
    HRESULT STDMETHODCALLTYPE Map(UINT s,const D3D12_RANGE* r,void** p) override{return resource->Map(s,r,p);}
    void STDMETHODCALLTYPE Unmap(UINT s,const D3D12_RANGE* r) override{resource->Unmap(s,r);}
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override{return resource->GetDesc();}
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override{return resource->GetGPUVirtualAddress();}
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT s,const D3D12_BOX* b,const void* p,UINT r,UINT d) override{return resource->WriteToSubresource(s,b,p,r,d);}
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void* p,UINT r,UINT d,UINT s,const D3D12_BOX* b) override{return resource->ReadFromSubresource(p,r,d,s,b);}
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* p,D3D12_HEAP_FLAGS* f) override{return resource->GetHeapProperties(p,f);}
};
class LayeredChain final : public OuterChain {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> applicationQueue, presentationQueue;
    ComPtr<ID3D12Fence> applicationFence, presentationFence;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    DXGI_SWAP_CHAIN_DESC1 description{};
    UINT index = 1;
    UINT64 serial = 0;
    HANDLE event = nullptr;
    const unsigned bufferFault=layeredBufferFault.load();
    ComPtr<ID3D12Resource> invalidBuffer;
    bool Wait(ID3D12Fence* fence, UINT64 value) {
        if (!value || fence->GetCompletedValue() >= value) return true;
        return SUCCEEDED(fence->SetEventOnCompletion(value,event))
            && WaitForSingleObject(event,3000)==WAIT_OBJECT_0;
    }
    bool Allocate() {
        buffers.clear();
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=description.Width;d.Height=description.Height;d.DepthOrArraySize=1;
        d.MipLevels=1;d.Format=description.Format;d.SampleDesc.Count=1;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for(UINT i=0;i<description.BufferCount;++i){
            ComPtr<ID3D12Resource> resource;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
                D3D12_RESOURCE_STATE_PRESENT,nullptr,IID_PPV_ARGS(&resource))))return false;
            buffers.push_back(std::move(resource));
        }
        invalidBuffer.Reset();
        if(bufferFault==1)invalidBuffer.Attach(new UnknownOwnerResource(buffers[1].Get()));
        if(bufferFault==3){
            d.Width-=1;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
                D3D12_RESOURCE_STATE_PRESENT,nullptr,IID_PPV_ARGS(&invalidBuffer))))return false;
        }
        index=1;return true;
    }
    bool CopyToPresentationBuffer() {
        if(index>=buffers.size()||!Wait(presentationFence.Get(),serial))return false;
        ComPtr<ID3D12Resource> target;
        const UINT nativeIndex=native->GetCurrentBackBufferIndex();
        if(index!=nativeIndex)++layeredCounts[1];
        if(FAILED(native->GetBuffer(nativeIndex,IID_PPV_ARGS(&target))))return false;
        ++serial;
        // Include the candidate's UI commands submitted immediately before
        // this callback, then transfer ownership across the two queues.
        if(FAILED(applicationQueue->Signal(applicationFence.Get(),serial))
            ||FAILED(presentationQueue->Wait(applicationFence.Get(),serial))
            ||FAILED(allocator->Reset())||FAILED(commands->Reset(allocator.Get(),nullptr)))return false;
        D3D12_RESOURCE_BARRIER barriers[2]{};
        for(auto& b:barriers){b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
        barriers[0].Transition.pResource=buffers[index].Get();
        barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.pResource=target.Get();
        barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        commands->ResourceBarrier(2,barriers);
        commands->CopyResource(target.Get(),buffers[index].Get());
        for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
        commands->ResourceBarrier(2,barriers);
        if(FAILED(commands->Close()))return false;
        ID3D12CommandList* list=commands.Get();presentationQueue->ExecuteCommandLists(1,&list);
        if(FAILED(presentationQueue->Signal(presentationFence.Get(),serial))
            ||!Wait(presentationFence.Get(),serial))return false;
        ++layeredCounts[2];return true;
    }
    HRESULT Forward(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params,bool present1) {
        if(!(flags&DXGI_PRESENT_TEST)&&!CopyToPresentationBuffer())return E_FAIL;
        const HRESULT hr=present1?native->Present1(sync,flags,params):native->Present(sync,flags);
        // DXGI itself can enqueue work on the native presentation queue.
        // A fence before Present covers only our copy, not that later work.
        if(!(flags&DXGI_PRESENT_TEST)){
            ++serial;
            if(FAILED(presentationQueue->Signal(presentationFence.Get(),serial))
                ||!Wait(presentationFence.Get(),serial))return E_FAIL;
        }
        if(SUCCEEDED(hr)&&!(flags&DXGI_PRESENT_TEST))index=(index+1)%description.BufferCount;
        return hr;
    }
public:
    LayeredChain(ComPtr<IDXGISwapChain4> chain,ID3D12CommandQueue* app,
        ID3D12CommandQueue* present,const DXGI_SWAP_CHAIN_DESC1& desc)
        :OuterChain(std::move(chain),4),applicationQueue(app),presentationQueue(present),description(desc){}
    ~LayeredChain() override {
        if(presentationFence)Wait(presentationFence.Get(),serial);
        buffers.clear();commands.Reset();allocator.Reset();
        native.Reset(); // Its creation queue must outlive the native swapchain.
        if(event)CloseHandle(event);
    }
    bool Initialize() {
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event||FAILED(applicationQueue->GetDevice(IID_PPV_ARGS(&device)))
            ||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&applicationFence)))
            ||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&presentationFence)))
            ||FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))
            ||FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)))
            ||FAILED(commands->Close())||!Allocate())return false;
        if(applicationQueue!=presentationQueue)++layeredCounts[0];
        return true;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT n,REFIID iid,void** out) override {
        ++layeredCounts[3];
        if(!out)return E_POINTER;*out=nullptr;
        if(n==1&&invalidBuffer)return invalidBuffer->QueryInterface(iid,out);
        if(n==1&&bufferFault==2)return buffers[0]->QueryInterface(iid,out);
        return n<buffers.size()?buffers[n]->QueryInterface(iid,out):DXGI_ERROR_INVALID_CALL;
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {return index;}
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* out) override {
        if(!out)return E_POINTER;*out=description;
        DXGI_SWAP_CHAIN_DESC1 actual{};native->GetDesc1(&actual);
        if(actual.BufferCount!=description.BufferCount)++layeredCounts[4];return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* out) override {
        const HRESULT hr=native->GetDesc(out);if(SUCCEEDED(hr))out->BufferCount=description.BufferCount;return hr;
    }
    HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT flags) override {return Forward(sync,flags,nullptr,false);}
    HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* params) override {return Forward(sync,flags,params,true);}
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count,UINT w,UINT h,DXGI_FORMAT format,UINT flags) override {
        if(!Wait(presentationFence.Get(),serial))return E_FAIL;
        // The candidate and application must release their own layer's
        // references before this generation is retired.
        buffers.clear();
        const HRESULT hr=native->ResizeBuffers(2,w,h,format,flags);
        if(FAILED(hr))return hr;
        DXGI_SWAP_CHAIN_DESC1 actual{};if(FAILED(native->GetDesc1(&actual)))return E_FAIL;
        if(count)description.BufferCount=count;
        description.Width=actual.Width;description.Height=actual.Height;
        description.Format=actual.Format;description.Flags=actual.Flags;
        return Allocate()?hr:E_FAIL;
    }
};
template<class Fn>
HRESULT CreateLayered(Fn create,IDXGIFactory2* factory,IUnknown* input,HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
    IDXGIOutput* restricted,IDXGISwapChain1** output) {
    if(!output||!desc)return E_INVALIDARG;*output=nullptr;
    ComPtr<ID3D12CommandQueue> app,present;ComPtr<ID3D12Device> device;
    if(FAILED(input->QueryInterface(IID_PPV_ARGS(&app)))||FAILED(app->GetDevice(IID_PPV_ARGS(&device))))return E_FAIL;
    D3D12_COMMAND_QUEUE_DESC q{};
    if(FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&present))))return E_FAIL;
    auto nativeDesc=*desc;nativeDesc.BufferCount=2;
    ComPtr<IDXGISwapChain1> native;ComPtr<IDXGISwapChain4> native4;
    const HRESULT hr=create(factory,present.Get(),window,&nativeDesc,full,restricted,&native);
    if(FAILED(hr)||FAILED(native.As(&native4)))return FAILED(hr)?hr:E_NOINTERFACE;
    auto* layer=new LayeredChain(native4,app.Get(),present.Get(),*desc);
    if(!layer->Initialize()){layer->Release();return E_FAIL;}
    *output=layer;return hr;
}
extern "C" __declspec(dllexport) uint64_t WINAPI OverlayFixtureLayeredCount(unsigned index){return index<5?layeredCounts[index].load():0;}
extern "C" __declspec(dllexport) void WINAPI OverlayFixtureBufferFault(unsigned value){layeredBufferFault.store(value);}
