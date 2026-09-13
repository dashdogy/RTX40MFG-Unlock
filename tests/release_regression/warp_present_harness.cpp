// Real D3D12 WARP swapchain/presentation harness for the bounded overlay
// redesign. Modes:
//   present - full presentation suite with drawing disabled (build A)
//   menu    - same base plus actual menu rendering, input, resize, teardown
//             (build B)
#include "common.h"
#include "existing_overlay.h"
#include <dwmapi.h>
#include <cmath>
#include <mutex>
#include <thread>
#include <sl.h>

namespace
{
using namespace harness;

constexpr uint32_t kStateActive = 2;
unsigned receivedKeys=0;
LRESULT CALLBACK GameWindowProc(HWND window,UINT message,WPARAM key,LPARAM data) {
    if(message==WM_KEYDOWN&&key=='A')++receivedKeys;
    return DefWindowProcW(window,message,key,data);
}
struct RestoreForeground { HWND window=GetForegroundWindow();~RestoreForeground(){if(IsWindow(window))SetForegroundWindow(window);} };

struct Chain
{
    ComPtr<IDXGISwapChain3> swapchain;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    DXGI_SWAP_CHAIN_DESC1 desc{};

    bool Create(WarpFixture& fixture, HWND window, UINT width, UINT height,
        UINT bufferCount = 3, DXGI_SWAP_EFFECT effect = DXGI_SWAP_EFFECT_FLIP_DISCARD)
    {
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = bufferCount;
        desc.SwapEffect = effect;
        // Waitable latency: the harness must prove queued presentations have
        // retired before backbuffer references are dropped, or the D3D12
        // debug layer fail-fasts its final-release corruption report.
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> chain;
        if (FAILED(fixture.factory->CreateSwapChainForHwnd(fixture.queue.Get(),
                window, &desc, nullptr, nullptr, &chain))
            || FAILED(chain.As(&swapchain)))
            return false;
        // A waitable-latency chain requires an explicit maximum before its
        // first present and again after every ResizeBuffers.
        ComPtr<IDXGISwapChain2> latency;
        if (FAILED(swapchain.As(&latency))
            || FAILED(latency->SetMaximumFrameLatency(bufferCount)))
            return false;
        return Acquire();
    }

    bool Acquire()
    {
        buffers.clear();
        for (UINT index = 0; index < desc.BufferCount; ++index)
        {
            ComPtr<ID3D12Resource> buffer;
            if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer)))) return false;
            buffers.push_back(std::move(buffer));
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = desc.BufferCount;
        ComPtr<ID3D12Device> device;
        if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))
            || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvs))))
            return false;
        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto handle = rtvs->GetCPUDescriptorHandleForHeapStart();
        for (auto& buffer : buffers)
        {
            device->CreateRenderTargetView(buffer.Get(), nullptr, handle);
            handle.ptr += step;
        }
        return true;
    }

    // Releasing backbuffer references requires an idle application queue
    // and a drained presentation queue: with the D3D12 debug layer active,
    // releasing a backbuffer that in-flight GPU work still references is a
    // fatal corruption report.
    static void Drain(WarpFixture& fixture, IDXGISwapChain3* chain = nullptr)
    {
        // Idle the application queue, then retire queued presentations via
        // the waitable latency object and the compositor: the overlay's
        // teardown inside ResizeBuffers performs the final backbuffer
        // release, and the D3D12 debug layer fail-fasts if presentation work
        // still references the buffer at that moment.
        fixture.SignalAndWait();
        MSG message;
        for (int pass = 0; pass < 4; ++pass)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(20);
        }
        if (chain)
        {
            ComPtr<IDXGISwapChain2> latency;
            if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&latency))) && latency)
            {
                // The handle is owned by the swap chain and must not be closed.
                HANDLE waitable = latency->GetFrameLatencyWaitableObject();
                if (waitable && waitable != INVALID_HANDLE_VALUE)
                    for (unsigned attempt = 0; attempt < descBufferCount(chain) + 1; ++attempt)
                        WaitForSingleObject(waitable, 1000);
            }
        }
        DwmFlush();
        for (int pass = 0; pass < 3; ++pass)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(20);
        }
    }

    static UINT descBufferCount(IDXGISwapChain3* chain)
    {
        DXGI_SWAP_CHAIN_DESC1 current{};
        return SUCCEEDED(chain->GetDesc1(&current)) ? current.BufferCount + 1 : 3;
    }

    bool Resize(WarpFixture& fixture, UINT width, UINT height)
    {
        Drain(fixture, swapchain.Get());
        buffers.clear();
        rtvs.Reset();
        const HRESULT resized = swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (FAILED(resized))
        {
            std::printf("dbg ResizeBuffers hr=0x%08lx\n", static_cast<unsigned long>(resized));
            std::fflush(stdout);
            return false;
        }
        ComPtr<IDXGISwapChain2> latency;
        if (FAILED(swapchain.As(&latency))
            || FAILED(latency->SetMaximumFrameLatency(desc.BufferCount)))
            return false;
        desc.Width = width;
        desc.Height = height;
        return Acquire();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index) const
    {
        ComPtr<ID3D12Device> device;
        swapchain->GetDevice(IID_PPV_ARGS(&device));
        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto handle = rtvs->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * step;
        return handle;
    }
};

bool RenderAndPresent(WarpFixture& fixture, Chain& chain, float color[4], UINT sync = 0,
    UINT flags = 0)
{
    const UINT index = chain.swapchain->GetCurrentBackBufferIndex();
    if (index >= chain.buffers.size()) return false;
    if (!ClearBackbuffer(fixture, chain.swapchain.Get(), chain.buffers[index].Get(),
            chain.rtvs.Get(), chain.Rtv(index), color))
        return false;
    ID3D12CommandList* list = fixture.commands.Get();
    fixture.queue->ExecuteCommandLists(1, &list);
    if (!fixture.SignalAndWait()) return false;
    const HRESULT result = chain.swapchain->Present(sync, flags);
    return SUCCEEDED(result) || result == DXGI_STATUS_OCCLUDED;
}

// The swapchain is R8G8B8A8_UNORM: the readback carries four unorm bytes in
// the float array. Compare against the clear color converted the same way
// ClearRenderTargetView does, with a small rounding tolerance.
bool PixelMatches(const float observed[4], const float expected[4])
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(observed);
    for (int channel = 0; channel < 4; ++channel)
    {
        int value;
        const float clamped = expected[channel] < 0.0f ? 0.0f
            : expected[channel] > 1.0f ? 1.0f : expected[channel];
        value = static_cast<int>(clamped * 255.0f + 0.5f);
        if (std::abs(static_cast<int>(bytes[channel]) - value) > 2) return false;
    }
    return true;
}

bool ReadCenter(WarpFixture& fixture, Chain& chain, float out[4],UINT x=UINT_MAX,UINT y=UINT_MAX)
{
    const UINT index = chain.swapchain->GetCurrentBackBufferIndex();
    if (index >= chain.buffers.size()) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC stagingDesc{};
    stagingDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    stagingDesc.Width = RequiredReadbackBytes(fixture, chain.buffers[index].Get());
    stagingDesc.Height = 1;
    stagingDesc.DepthOrArraySize = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    if (FAILED(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &stagingDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&staging))))
        return false;
    return ReadCenterPixel(fixture, chain.buffers[index].Get(), out, staging.Get(),x,y);
}

// Renders the clear, waits for completion, reads the app-owned current
// backbuffer, then presents it. The read is always taken before the buffer is
// handed back to DXGI.
bool RenderReadPresent(WarpFixture& fixture, Chain& chain, float color[4], float out[4])
{
    const UINT index = chain.swapchain->GetCurrentBackBufferIndex();
    if (index >= chain.buffers.size()) return false;
    if (!ClearBackbuffer(fixture, chain.swapchain.Get(), chain.buffers[index].Get(),
            chain.rtvs.Get(), chain.Rtv(index), color))
        return false;
    ID3D12CommandList* list = fixture.commands.Get();
    fixture.queue->ExecuteCommandLists(1, &list);
    if (!fixture.SignalAndWait()) return false;
    if (!ReadCenter(fixture, chain, out)) return false;
    const HRESULT result = chain.swapchain->Present(0, 0);
    return SUCCEEDED(result) || result == DXGI_STATUS_OCCLUDED;
}


int RunPresentation(const wchar_t* dll, bool menuMode, uint32_t expectedTargets, bool raceStartup = false, bool coexist = false,bool dynamicFactory=false,bool streamlineStartup=false,bool wrapped=false,bool layered=false,bool reshadeWrapped=false,bool firstLaunch=false)
{
    // ---------------------------------------------------------------- setup
    // Enable validation before any candidate/fixture device can be retained.
    // Turning it on after slInit's probe would invalidate an existing device.
    if(coexist||streamlineStartup){
        using Debug=HRESULT(WINAPI*)(REFIID,void**);
        const auto getDebug=reinterpret_cast<Debug>(GetProcAddress(SystemModule(L"d3d12.dll"),"D3D12GetDebugInterface"));
        ComPtr<ID3D12Debug> debug;if(getDebug&&SUCCEEDED(getDebug(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    }
    ExistingOverlay existing;
    if (coexist && !Check(existing.Load(), "pre-existing overlay installed on native exports and methods")) return 1;
    if(wrapped){
        const auto set=reinterpret_cast<void(WINAPI*)(unsigned)>(GetProcAddress(existing.module,"OverlayFixtureWrapper"));
        if(!Check(set!=nullptr,"outer COM fixture available"))return 1;set(reshadeWrapped?5:layered?4:1);
    }
    HMODULE module = raceStartup ? LoadCandidate(dll) : nullptr;
    HMODULE interposer=nullptr;
    if (raceStartup && !Check(module != nullptr, "candidate loaded before the first factory/device")) return 1;
    if(streamlineStartup){
        wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
        const auto path=std::filesystem::path(exe).parent_path()/L"sl.interposer.dll";
        HMODULE stub=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!Check(stub!=nullptr,"local Streamline ABI fixture loaded"))return 1; interposer=stub;
        using Init=sl::Result(*)(const sl::Preferences&,uint64_t);
        const auto init=reinterpret_cast<Init>(GetProcAddress(stub,"slInit"));
        Check(init&&Status(module).overlayInstallState==1,"slInit lookup does no graphics work");
        sl::Preferences prefs{};const auto flags=prefs.flags;
        const auto result=init?init(prefs,sl::kSDKVersion):sl::Result::eErrorNotInitialized;
        auto calls=reinterpret_cast<unsigned(WINAPI*)()>(GetProcAddress(stub,"FixtureInitCalls"));
        Check(result==sl::Result::eOk&&calls&&calls()==1&&prefs.flags==flags,"slInit result, single original call and caller preferences preserved");
        if(!Check(Status(module).factoryWrappersCreated==0&&Status(module).swapchainWrappersCreated==0&&existing.count(4)==0,"slInit creates no graphics objects or swapchain hooks"))return 1;
        Check(existing.intact(),"slInit startup preserved pre-existing overlay code");
    }
    if(dynamicFactory){
        using Factory=HRESULT(WINAPI*)(REFIID,void**);
        const auto factory=reinterpret_cast<Factory>(GetProcAddress(SystemModule(L"dxgi.dll"),"CreateDXGIFactory1"));
        Check(factory&&Status(module).overlayInstallState==1,"dynamic export resolution performs no installation");
        ComPtr<IDXGIFactory1> created;
        if(!Check(factory&&SUCCEEDED(factory(IID_PPV_ARGS(&created)))&&Status(module).overlayInstallState==2,"dynamic factory gateway completes startup before returning to game"))return 1;
    }
    WarpFixture fixture;
    if (!Check(fixture.Initialize(true), "WARP device, queue and factory created"
            " (debug layer when available)"))
        return 1;
    if (!fixture.debugLayer)
        std::printf("NOTE D3D12 debug layer unavailable on this machine; validation reduced\n");

    RestoreForeground previousForeground;
    TestWindow window(L"BoundedWarp", 640, 480,GameWindowProc);
    if (!Check(window.window != nullptr, "test window created")) return 1;
    window.Show();
    if (firstLaunch)
    {
        const DWORD other = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD own = GetCurrentThreadId();
        const bool attached = own != other && AttachThreadInput(own, other, TRUE);
        SetForegroundWindow(window.window); SetFocus(window.window);
        if (attached) AttachThreadInput(own, other, FALSE);
        if (!Check(GetForegroundWindow() == window.window, "first-launch test owns foreground")) return 2;
    }

    if (!module) module = LoadCandidate(dll);
    if (!Check(module != nullptr, "candidate winhttp.dll loads")) return 1;
    // Invoke the ordinary imported API; never synchronize through private status.
    ComPtr<IDXGIFactory1> applicationFactory;
    if (!Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&applicationFactory))), "real factory call returns after initialization")) return 1;
    // A candidate without the version-2 status ABI (the earlier diagnostic)
    // is exercised as a baseline control: forwarding and presentation still
    // run, but the bounded assertions are reported as unavailable.
    MfgSingleModuleStatus probe{};
    probe.size = sizeof(probe);
    const QueryFn query = CandidateQuery(module);
    const bool boundedAbi = query && query(&probe) && probe.version == 3;
    if (!boundedAbi) std::printf("NOTE version-2 status ABI absent; running as baseline control\n");
    if (boundedAbi && !raceStartup)
    {

        const MfgSingleModuleStatus installed = Status(module);
        std::printf("STARTUP state=%u reason=%u targets=%u\n", installed.overlayInstallState, installed.overlayInstallReason, installed.overlayActivatedTargets);
        if (installed.overlayInstallState != kStateActive) Sleep(1500); // Preserve failure diagnostics, never gates success.
        if (!Check(installed.overlayInstallState == kStateActive, "bounded hooks active"))
            return 1;
        Check(installed.overlayActivatedTargets <= 5,
            "only optional application input imports are published");
        Check(installed.overlaySuspendedThreads == 0, "UI installation never suspended process threads");
    }
    // Use a real post-load factory. The old harness accidentally retained a
    // pre-install factory; shared-table changes masked that coverage error.
    if(interposer){
        using Factory=HRESULT(WINAPI*)(REFIID,void**);
        auto create=reinterpret_cast<Factory>(GetProcAddress(interposer,"CreateDXGIFactory1"));
        Check(create&&SUCCEEDED(create(IID_PPV_ARGS(&fixture.factory))),"Streamline exported factory gateway returns owned proxy");
    }else Check(SUCCEEDED(applicationFactory.As(&fixture.factory)),"use intercepted application factory");
    Chain chain;
    if (coexist) {
        Check(existing.intact(), "candidate preserved every existing overlay code patch");
        existing.reset();
    }
    if (!Check(chain.Create(fixture, window.window, 640, 480, menuMode ? 2 : 3,
            menuMode ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_FLIP_DISCARD),
            menuMode ? "flip-sequential swapchain created (menu content validation)"
                     : "flip-model swapchain created"))
        return 1;

    if (raceStartup)
    {
        // Applications do not call the private coordinator status ABI before
        // creating their swapchains. Exercise that ordering explicitly.
        if (!Check(boundedAbi, "startup regression requires the real coordinator ABI")
            || !Check(Status(module).overlayInstallState == kStateActive, "initialization complete at factory return")) return 1;
        auto state = Status(module);
        std::printf("STARTUP state=%u reason=%u targets=%u\n", state.overlayInstallState,
            state.overlayInstallReason, state.overlayActivatedTargets);
    }

    // ------------------------------------------------- known content + read
    float teal[4] = {0.10f, 0.60f, 0.60f, 1.0f};
    for (int frame = 0; frame < 5; ++frame)
        if (!Check(RenderAndPresent(fixture, chain, teal), "frame rendered and presented"))
            return 1;
    float observed[4]{};
    if (firstLaunch)
    {
        Check(Status(module).overlayVisible == 1, "clean first launch opens full menu without a hotkey");
        float pixel[4]{};
        Check(ReadCenter(fixture,chain,pixel)&&!PixelMatches(pixel,teal), "automatic menu changes presented center pixels");
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
        const auto ini=std::filesystem::path(exe).parent_path()/L"RTX40MFG-UI.ini";
        Check(GetPrivateProfileIntW(L"Overlay",L"FirstLaunchMenuShown",0,ini.c_str())==1,"first-launch marker persisted after drawing menu");
        PostMessageW(window.window,WM_KEYDOWN,VK_BACK,0x000E0001);
        PostMessageW(window.window,WM_KEYUP,VK_BACK,0xC0E0001);
        window.Pump(4);
        for(int i=0;i<3;++i) RenderAndPresent(fixture,chain,teal);
        Check(Status(module).overlayVisible==0,"Backspace closes automatic menu");
    }
    else if(menuMode){float hint[4]{};Check(ReadCenter(fixture,chain,hint,24,24)&&!PixelMatches(hint,teal),"startup hint visibly composited into retained backbuffer");}
    if (!Check(RenderReadPresent(fixture, chain, teal, observed),
            "frame rendered, read back before presentation, and presented"))
        return 1;
    Check(PixelMatches(observed, teal), "backbuffer readback matches the rendered clear color");
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after presents");

    // ----------------------------------------------------- Present1 + ABI
    // Full-frame Present1.
    {
        std::printf("dbg present1 block enter\n"); std::fflush(stdout);
        const UINT index = chain.swapchain->GetCurrentBackBufferIndex();
        std::printf("dbg index=%u\n", index); std::fflush(stdout);
        float violet[4] = {0.55f, 0.20f, 0.70f, 1.0f};
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = chain.Rtv(index);
        std::printf("dbg rtv resolved\n"); std::fflush(stdout);
        if (!ClearBackbuffer(fixture, chain.swapchain.Get(), chain.buffers[index].Get(),
                chain.rtvs.Get(), handle, violet))
            return 1;
        std::printf("dbg clear done\n"); std::fflush(stdout);
        ID3D12CommandList* list = fixture.commands.Get();
        fixture.queue->ExecuteCommandLists(1, &list);
        fixture.SignalAndWait();
        // Note: on this Windows build, native WARP Present1 with a null
        // DXGI_PRESENT_PARAMETERS pointer faults inside dxgi with no hooking
        // present (verified by the standalone ReadbackProbe control). The
        // overlay forwards the pointer unchanged; the harness therefore
        // presents a valid empty parameter structure.
        DXGI_PRESENT_PARAMETERS empty{};
        std::printf("dbg calling Present1\n"); std::fflush(stdout);
        const HRESULT full = chain.swapchain->Present1(0, 0, &empty);
        std::printf("dbg present1 returned 0x%08lx\n", static_cast<unsigned long>(full)); std::fflush(stdout);
        Check(SUCCEEDED(full) || full == DXGI_STATUS_OCCLUDED, "Present1 full-frame succeeds");
    }
    // Exact metadata forwarding on a controlled flip-sequential fixture:
    // legal combinations succeed and the documented invalid combination is
    // rejected by DXGI itself. A layer that dropped or rewrote the metadata
    // would change these outcomes.
    {
        TestWindow metadataWindow(L"BoundedMetadata", 320, 240);
        Chain metadataChain;
        if (!Check(metadataChain.Create(fixture, metadataWindow.window, 320, 240, 2,
                DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL), "metadata fixture chain created"))
            return 1;
        float gray[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        for (int i = 0; i < 3; ++i) RenderAndPresent(fixture, metadataChain, gray);
        // Metadata forwarding on this Windows build: a Present1 carrying a
        // scroll rect plus offset succeeds through the activated detour,
        // proving both pointers reach DXGI exactly. Dirty-rect-only presents
        // and the scroll-without-offset rejection documented for earlier
        // Windows builds are not deterministic here (the standalone
        // ReadbackProbe control records INVALID_CALL, S_OK, and native
        // faults for those inputs with no DLL loaded), so those rejection
        // assertions are reported as unavailable rather than relied upon.
        RECT scroll{120, 120, 240, 240};
        POINT offset{8, 12};
        DXGI_PRESENT_PARAMETERS withScroll{};
        withScroll.pScrollRect = &scroll;
        withScroll.pScrollOffset = &offset;
        const HRESULT scrolled = metadataChain.swapchain->Present1(0, 0, &withScroll);
        if(coexist){FixturePresent seen{};existing.readPresent(&seen);Check(seen.kind==8&&seen.chain==FixtureNativeIdentity(metadataChain.swapchain.Get())&&seen.metadata==&withScroll&&seen.scroll==withScroll.pScrollRect&&seen.offset==withScroll.pScrollOffset&&seen.dirty==withScroll.pDirtyRects&&seen.dirtyCount==withScroll.DirtyRectsCount&&seen.sync==0&&seen.flags==0&&seen.result==scrolled,"Present1 preserves exact pointers, metadata, sync, flags and HRESULT through existing overlay");}
        Check(SUCCEEDED(scrolled) || scrolled == DXGI_STATUS_OCCLUDED,
            "Present1 forwards scroll-rect and scroll-offset metadata and succeeds");
        std::printf(
            "NOTE Present1 dirty-only and scroll-without-offset rejection checks are"
            " not deterministic on this Windows build and are not asserted\n");
        std::fflush(stdout);

        // One more full-frame metadata-free Present1 through the detour for
        // balanced fixture activity, then release.
        DXGI_PRESENT_PARAMETERS full{};
        metadataChain.swapchain->Present1(0, 0, &full);
        Chain::Drain(fixture, metadataChain.swapchain.Get());
    }
    // DXGI_PRESENT_TEST is forwarded as a test, never a real present.
    {
        const UINT before = 0;
        UINT count = before;
        chain.swapchain->GetLastPresentCount(&count);
        const HRESULT test = chain.swapchain->Present(0, DXGI_PRESENT_TEST);
        if(coexist){FixturePresent seen{};existing.readPresent(&seen);Check(seen.kind==7&&seen.chain==FixtureNativeIdentity(chain.swapchain.Get())&&seen.sync==0&&seen.flags==DXGI_PRESENT_TEST&&seen.result==test,"Present TEST preserves chain, flags, sync and HRESULT through existing overlay");}
        UINT after = 0;
        chain.swapchain->GetLastPresentCount(&after);
        Check(SUCCEEDED(test) && after == count, "DXGI_PRESENT_TEST forwarded without presenting");
    }
    if(coexist){
        auto busy=reinterpret_cast<void(WINAPI*)(BOOL)>(GetProcAddress(existing.module,"OverlayFixtureStillDrawing"));
        if(!Check(busy!=nullptr,"deterministic nonblocking-present fixture available"))return 1;
        busy(TRUE);
        Check(chain.swapchain->Present(0,DXGI_PRESENT_DO_NOT_WAIT)==DXGI_ERROR_WAS_STILL_DRAWING,"first rejected nonblocking Present preserves HRESULT");
        const auto beforeBusy=Status(module);
        Check(chain.swapchain->Present(0,DXGI_PRESENT_DO_NOT_WAIT)==DXGI_ERROR_WAS_STILL_DRAWING,"repeated rejected nonblocking Present preserves HRESULT");
        const auto afterBusy=Status(module);
        Check(beforeBusy.renderedFrames==afterBusy.renderedFrames&&beforeBusy.dxgiFrames==afterBusy.dxgiFrames,"repeated rejected nonblocking Present submits no duplicate UI frame");
        busy(FALSE);
        Check(SUCCEEDED(chain.swapchain->Present(0,DXGI_PRESENT_DO_NOT_WAIT)),"nonblocking Present recovers after original accepts work");
    }
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after Present1");

    // -------------------------------------------------------------- resize
    const uint32_t before = Status(module).dxgiFrames;
    for (uint32_t round = 0; round < 3; ++round)
    {
        const UINT width = 320 + 64 * round;
        const UINT height = 240 + 48 * round;
        std::printf("dbg resize round %u enter\n", round); std::fflush(stdout);
        if (!Check(chain.Resize(fixture, width, height), "ResizeBuffers with backbuffer release"))
            return 1;
        float amber[4] = {0.80f, 0.55f, 0.15f, 1.0f};
        float pixel[4]{};
        if (!Check(RenderReadPresent(fixture, chain, amber, pixel), "frame after resize"))
            return 1;
        Check(PixelMatches(pixel, amber), "content after resize matches the new clear color");
    }

    // ------------------------------------------------ destroy and recreate
    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    if (!Check(chain.Create(fixture, window.window, 512, 384), "swapchain recreated"))
        return 1;
    float green[4] = {0.15f, 0.70f, 0.25f, 1.0f};
    Check(RenderAndPresent(fixture, chain, green), "presented on the recreated chain");
    if (menuMode) Check(Status(module).overlayVisible==0,"closed menu stays closed across swapchain recreation");

    // ----------------------------------------------- two independent chains
    {
        TestWindow second(L"BoundedWarp2", 256, 256);
        Chain other;
        if (!Check(other.Create(fixture, second.window, 256, 256, 2),
                "second independent swapchain created"))
            return 1;
        std::mutex step;
        std::thread presenter([&] {
            std::lock_guard lock(step);
            float red[4] = {0.75f, 0.15f, 0.15f, 1.0f};
            for (int i = 0; i < 4; ++i)
                if (!RenderAndPresent(fixture, other, red)) return;
        });
        {
            std::lock_guard lock(step);
            float blue[4] = {0.15f, 0.25f, 0.80f, 1.0f};
            for (int i = 0; i < 4; ++i)
                if (!RenderAndPresent(fixture, chain, blue)) return 1;
        }
        presenter.join();
        float red[4]{}, blue[4]{};
        const float redColor[4] = {0.75f, 0.15f, 0.15f, 1.0f};
        const float blueColor[4] = {0.15f, 0.25f, 0.80f, 1.0f};
        Check(RenderReadPresent(fixture, other, const_cast<float*>(redColor), red)
                && PixelMatches(red, redColor),
            "independent chain A retained its own content");
        Check(RenderReadPresent(fixture, chain, const_cast<float*>(blueColor), blue)
                && PixelMatches(blue, blueColor),
            "independent chain B retained its own content");
    }
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after concurrency");

    // ------------------------------------------------------- tracked count
    const uint32_t after = Status(module).dxgiFrames;
    if (boundedAbi)
        Check(after > before, "principal-chain presentation telemetry counted");
    else
        std::printf("NOTE baseline control: skipping bounded telemetry assertion\n");

    // ----------------------------------------------------------- menu mode
    if (menuMode)
    {
        // Foreground ownership is a setup requirement for the real capture gate.
        const DWORD ourThread=GetCurrentThreadId();
        // Draw several frames so the overlay initializes its renderer on the
        // tracked principal chain, then toggle the menu through the window's
        // keyboard path (default hotkey VK_BACK) and present again.
        float base[4] = {0.30f, 0.30f, 0.35f, 1.0f};
        for (int i = 0; i < 8; ++i)
        {
            RenderAndPresent(fixture, chain, base);
            window.Pump(2);
        }
        const uint64_t renderedBefore = Status(module).renderedFrames;
        Check(Status(module).dxgiFrames > 0, "presentation tracked on the principal chain");

        WPARAM key = 0;
        {
            MfgSingleModuleStatus status = Status(module);
            key = status.shortcutBindingActive ? status.shortcutVirtualKey : VK_BACK;
        }
        window.Pump(4);
        const DWORD foregroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
        const bool attached=foregroundThread!=ourThread&&AttachThreadInput(ourThread,foregroundThread,TRUE);
        BringWindowToTop(window.window); SetForegroundWindow(window.window); SetFocus(window.window);
        if(attached)AttachThreadInput(ourThread,foregroundThread,FALSE);
        window.Pump(2);
        Check(GetForegroundWindow()==window.window,"menu input test owns foreground window");
        PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
        PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
        for (int i = 0; i < 12; ++i)
        {
            RenderAndPresent(fixture, chain, base);
            window.Pump(4);
        }
        MfgSingleModuleStatus menu = Status(module);
        Check(menu.overlayVisible == 1, "menu hotkey made the overlay visible");
        Check(menu.renderedFrames > renderedBefore,
            "menu rendering executed actual UI draw submissions");

        // Output content: FLIP_SEQUENTIAL preserves what each present
        // composited, so after the queue drains the retained backbuffers
        // carry base+UI. Read without rendering anything new.
        bool differs = false;
        for (int attempt = 0; attempt < 6 && !differs; ++attempt)
        {
            RenderAndPresent(fixture, chain, base);
            window.Pump(3);
            Sleep(120);
            float pixel[4]{};
            if (ReadCenter(fixture, chain, pixel))
                differs = !PixelMatches(pixel, base);
        }
        Check(differs, "menu visibly composited into the presented backbuffer");

        // Input capture and release: the input exports stay functional while
        // the menu is open, and the cursor clip is released after closing.
        // The content readback above may span compositor waits. Re-establish
        // and verify foreground ownership at the actual message boundary;
        // focus can change independently of the renderer during that time.
        const DWORD inputForegroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
        const bool inputAttached=inputForegroundThread!=ourThread&&AttachThreadInput(ourThread,inputForegroundThread,TRUE);
        SetForegroundWindow(window.window);SetFocus(window.window);
        if(inputAttached)AttachThreadInput(ourThread,inputForegroundThread,FALSE);
        if(!Check(GetForegroundWindow()==window.window,"keyboard capture check owns foreground at message boundary"))return 2;
        const SHORT async = GetAsyncKeyState('A');
        const SHORT state = GetKeyState('B');
        std::printf("NOTE input forwarding GetAsyncKeyState=%d GetKeyState=%d\n",
            static_cast<int>(async), static_cast<int>(state));
        Check(async==0&&state==0,"input exports suppress game keys while menu is open");
        unsigned keysBefore=receivedKeys;
        SendMessageW(window.window,WM_KEYDOWN,'A',0x001E0001);
        Check(receivedKeys==keysBefore,"open menu captures keyboard message before game WndProc");
        RECT probe{0, 0, 200, 200};
        Check(ClipCursor(&probe) && ClipCursor(nullptr), "ClipCursor forwards and releases");

        PostMessageW(window.window, WM_KEYDOWN, key, 0x000E0001);
        PostMessageW(window.window, WM_KEYUP, key, 0xC0E0001);
        for (int i = 0; i < 6; ++i)
        {
            RenderAndPresent(fixture, chain, base);
            window.Pump(3);
        }
        Check(Status(module).overlayVisible == 0, "second hotkey press closed the menu");
        keysBefore=receivedKeys;
        SendMessageW(window.window,WM_KEYDOWN,'A',0x001E0001);
        Check(receivedKeys==keysBefore+1,"closed menu returns keyboard message to game WndProc");

        // Resize while the renderer is initialized, then keep presenting.
        Check(chain.Resize(fixture, 400, 300), "resize after menu use");
        for (int i = 0; i < 4; ++i) RenderAndPresent(fixture, chain, base);
        Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy after menu");
    }
    else if (expectedTargets == 10)
    {
        // Build A: drawing stays disabled; presenting must not render UI.
        float base[4] = {0.30f, 0.30f, 0.35f, 1.0f};
        for (int i = 0; i < 8; ++i) RenderAndPresent(fixture, chain, base);
        MfgSingleModuleStatus status = Status(module);
        Check(status.renderedFrames == 0, "forwarding build performs no UI rendering");
        Check(status.overlayActivatedTargets > 0, "comparison retains application input setup");
        Check(status.overlayVisible == 0, "overlay stays invisible in the forwarding build");
        Check(status.renderFailures == 0, "no UI render failures in the forwarding build");
    }
    else
    {
        // Menu-enabled candidate with the menu never opened: the renderer
        // initializes on the tracked principal chain and stays healthy. UI
        // submissions may still occur (the software cursor is part of the
        // shipped renderer), so only visibility and failures are asserted.
        float base[4] = {0.30f, 0.30f, 0.35f, 1.0f};
        for (int i = 0; i < 8; ++i) RenderAndPresent(fixture, chain, base);
        MfgSingleModuleStatus status = Status(module);
        Check(status.overlayVisible == 0, "overlay stays invisible without the hotkey");
        Check(status.renderFailures == 0, "no UI render failures with the menu closed");
        std::printf("NOTE menu-enabled candidate submitted %llu UI frames with the menu closed (software cursor)\n",
            static_cast<unsigned long long>(status.renderedFrames));
        std::fflush(stdout);
    }

    // -------------------------------------------------------------- teardown
    Chain::Drain(fixture, chain.swapchain.Get());
    chain.buffers.clear();
    chain.rtvs.Reset();
    chain.swapchain.Reset();
    Chain::Drain(fixture);
    fixture.commands.Reset();
    fixture.allocator.Reset();
    fixture.queue.Reset();
    window.Pump(4);
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "device healthy at teardown");
    if (fixture.debugLayer)
    {
        ComPtr<struct ID3D12InfoQueue> info;
        if (SUCCEEDED(fixture.device->QueryInterface(IID_PPV_ARGS(&info))) && info)
        {
            const UINT64 errors = info->GetNumStoredMessages();
            for (UINT64 index = 0; index < errors; ++index) {
                SIZE_T length = 0;
                info->GetMessage(index, nullptr, &length);
                std::vector<unsigned char> storage(length);
                auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(info->GetMessage(index, message, &length)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    std::printf("DEBUG_ERROR %s\n", message->pDescription);
                    Check(false, "D3D12 debug error/corruption");
                }
            }
            std::printf("NOTE debug layer stored messages: %llu\n",
                static_cast<unsigned long long>(errors));
        }
        else
            std::printf("NOTE info queue unavailable; error reporting reduced\n");
    }
    if (menuMode)
    {
        // Destroy the renderer-owned resources with the module still loaded.
        MfgSingleModuleStatus status = Status(module);
        Check(status.overlayInstallState == kStateActive, "coordinator still active at teardown");
    }
    std::printf("COMPLETE mode=%ls\n", menuMode ? L"menu" : L"present");
    if (coexist) {
        Check(existing.intact(), "existing overlay code unchanged after menu, resize and teardown");
        Check(existing.restored(),"all native tables and protections unchanged");
        Check(existing.count(13)==0&&existing.count(14)==0&&existing.count(15)>0,"stateful overlay initializes before nested Release without a missing binding");
        Check(existing.count(4) > 0 && existing.count(7) > 0 && existing.count(8) > 0
            && existing.count(9) > 0 && existing.count(12) > 0,
            "existing overlay received real create, Present, Present1, resize and release calls");
    }
    Check(Status(module).liveSwapchainWrappers==0,"all swapchain proxies retired after teardown");
    if(layered){
        auto count=reinterpret_cast<uint64_t(WINAPI*)(unsigned)>(GetProcAddress(existing.module,"OverlayFixtureLayeredCount"));
        Check(count&&count(0)>=3,"layered fixture used separate application and presentation queues on the same device");
        Check(count&&count(1)>0&&count(4)>0,"application and native buffer counts and frame indices diverged");
        Check(count&&count(2)>20&&count(3)>0,"off-screen application buffers crossed a synchronized queue boundary before native presentation");
        auto live=reinterpret_cast<unsigned(WINAPI*)()>(GetProcAddress(existing.module,"OverlayFixtureLiveOuter"));
        Check(live&&live()==0,"layered swapchain generations released all fixture ownership");
    }
    return gFailures == 0 ? 0 : 10;
}
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 3 && argc != 4) return 2;
    const std::wstring mode = argv[1];
    const wchar_t* dll = argv[2];
    std::printf("mode=%ls dll=%ls\n", argv[1], dll);
    if (mode == L"present") return RunPresentation(dll, false, argc == 4 ? _wtoi(argv[3]) : 10);
    if (mode == L"coexist-present") return RunPresentation(dll, false, 10,true,true);
    if (mode == L"wrapped-slinit-forward") return RunPresentation(dll,false,10,true,true,false,true,true);
    if (mode == L"menu") return RunPresentation(dll, true, 18);
    if (mode == L"startup-menu") return RunPresentation(dll, true, 18, true);
    if (mode == L"first-launch-menu") return RunPresentation(dll,true,18,true,false,false,false,false,false,false,true);
    if (mode == L"coexist-menu") return RunPresentation(dll, true, 18, true, true);
    if (mode == L"dynamic-coexist-menu") return RunPresentation(dll,true,18,true,true,true);
    if (mode == L"wrapped-slinit-menu") return RunPresentation(dll,true,18,true,true,false,true,true);
    if (mode == L"layered-slinit-menu") return RunPresentation(dll,true,18,true,true,false,true,true,true);
    if (mode == L"reshade-wrapped-menu") return RunPresentation(dll,true,18,true,true,false,true,true,false,true);
    if (mode == L"slinit-coexist-menu") return RunPresentation(dll,true,18,true,true,false,true);
    std::printf("unknown mode\n");
    return 2;
}
