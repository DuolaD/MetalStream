// Real-GPU swapchain lifecycle test for DLSS-G frame generation (needs Wine + GPTk, run via
// scripts/run_gptk_tests.sh). Presents without vsync, so MetalStream's present thread is running
// through every step:
//   1. generate frames on swapchain A
//   2. ResizeBuffers A to a new size, keep generating
//   3. release A right after a Present, create B for the same window, keep generating
//   3b. set the tags to null (menu), then tag again
//   4. create a second swapchain C for another window: B must keep working (its proxy buffers);
//      once C is gone, frame generation returns to B
//   5. slShutdown, keep presenting B: the game's backbuffer index tracking must still hold
//   6. release B right after a Present
// Every frame checks that GetCurrentBackBufferIndex matches the index the "game" tracks itself.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <streamline/sl.h>
#include <streamline/sl_consts.h>
#include <streamline/sl_dlss_g.h>

typedef sl::Result (*PFN_slInit)(const sl::Preferences&, uint64_t);
typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slSetD3DDevice)(void*);
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*&, const uint32_t*);
typedef sl::Result (*PFN_slSetTag)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*PFN_slSetConstants)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature, const char*, void*&);
typedef sl::Result (*PFN_slDLSSGSetOptions)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
typedef sl::Result (*PFN_slDLSSGGetState)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);

static const UINT kBuffers = 3;
static int g_failures = 0;

#define CHECK(cond, ...) do { \
        if (cond) { printf("[PASS] "); } else { printf("[FAIL] "); ++g_failures; } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static ID3D12Resource* MakeTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = flags;
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &br);
}

static HWND MakeWindow(const wchar_t* title, UINT w, UINT h, int x) {
    return CreateWindowW(L"MetalStreamFGLife", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, 100, w, h, nullptr, nullptr,
                         GetModuleHandleW(nullptr), nullptr);
}

struct Ctx {
    PFN_slGetNewFrameToken slGetNewFrameToken;
    PFN_slSetTag slSetTag;
    PFN_slSetConstants slSetConstants;
    PFN_slDLSSGGetState slDLSSGGetState;
    bool slAlive = true;

    ID3D12Device* dev;
    ID3D12CommandQueue* queue;
    IDXGIFactory4* factory;
    ID3D12DescriptorHeap* rtvHeap;
    UINT rtvStep;
    ID3D12CommandAllocator* alloc[kBuffers];
    ID3D12GraphicsCommandList* cl;
    ID3D12Fence* fence;
    HANDLE ev;
    UINT64 fenceValue = 0, allocFence[kBuffers] = {};
    uint32_t frame = 0;

    // Current swapchain and the game's own view of it
    IDXGISwapChain3* sc = nullptr;
    ID3D12Resource* bb[kBuffers] = {};
    UINT w = 0, h = 0;
    UINT trackedIndex = 0;
    ID3D12Resource* hudless = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* mvecs = nullptr;
    unsigned indexMismatches = 0;
    unsigned presentFailures = 0;

    void WaitGpu() {
        queue->Signal(fence, ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, ev);
        WaitForSingleObject(ev, 5000);
    }

    void ReleaseBuffers() {
        for (auto& b : bb) { if (b) b->Release(); b = nullptr; }
        if (hudless) hudless->Release();
        if (depth) depth->Release();
        if (mvecs) mvecs->Release();
        hudless = depth = mvecs = nullptr;
    }

    // Fetches the backbuffers (and makes size-matched inputs) like a game after creation / resize
    void FetchBuffers() {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        sc->GetDesc1(&d);
        w = d.Width; h = d.Height;
        D3D12_CPU_DESCRIPTOR_HANDLE base = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBuffers; ++i) {
            sc->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
            D3D12_CPU_DESCRIPTOR_HANDLE hnd = base; hnd.ptr += i * rtvStep;
            dev->CreateRenderTargetView(bb[i], nullptr, hnd);
        }
        hudless = MakeTex(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE hnd = base; hnd.ptr += kBuffers * rtvStep;
        dev->CreateRenderTargetView(hudless, nullptr, hnd);
        depth = MakeTex(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        mvecs = MakeTex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        trackedIndex = 0; // a new or resized swapchain starts at buffer 0
    }

    void RenderFrames(unsigned count, bool tag = true) {
        sl::ViewportHandle vp(0);
        D3D12_CPU_DESCRIPTOR_HANDLE base = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (unsigned n = 0; n < count; ++n, ++frame) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            // Like Unity: the game tracks the backbuffer index itself
            if (sc->GetCurrentBackBufferIndex() != trackedIndex) ++indexMismatches;
            UINT slot = frame % kBuffers;
            if (fence->GetCompletedValue() < allocFence[slot]) { fence->SetEventOnCompletion(allocFence[slot], ev); WaitForSingleObject(ev, 5000); }
            alloc[slot]->Reset();
            cl->Reset(alloc[slot], nullptr);

            sl::FrameToken* token = nullptr;
            if (slAlive) slGetNewFrameToken(token, &frame);
            float t = frame / 60.0f;
            float color[4] = { 0.5f + 0.5f * sinf(t * 3.0f), 0.5f + 0.5f * sinf(t * 2.0f + 1.0f), 0.5f + 0.5f * sinf(t + 2.0f), 1.0f };
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = base; rtv.ptr += trackedIndex * rtvStep;
            D3D12_CPU_DESCRIPTOR_HANDLE hudRtv = base; hudRtv.ptr += kBuffers * rtvStep;
            ID3D12Resource* target = bb[trackedIndex];
            Barrier(cl, target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            cl->ClearRenderTargetView(rtv, color, 0, nullptr);
            cl->ClearRenderTargetView(hudRtv, color, 0, nullptr);
            Barrier(cl, hudless, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (slAlive && tag) {
                sl::Resource rHudless(sl::ResourceType::eTex2d, hudless, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                sl::Resource rDepth(sl::ResourceType::eTex2d, depth, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                sl::Resource rMvecs(sl::ResourceType::eTex2d, mvecs, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                sl::ResourceTag tags[] = {
                    sl::ResourceTag(&rHudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow),
                    sl::ResourceTag(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow),
                    sl::ResourceTag(&rMvecs, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
                };
                slSetTag(vp, tags, 3, cl);
                sl::Constants c{};
                c.mvecScale = { 1.0f / w, 1.0f / h };
                c.depthInverted = sl::Boolean::eTrue;
                c.cameraMotionIncluded = sl::Boolean::eTrue;
                c.reset = n == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
                c.cameraNear = 0.1f; c.cameraFar = 1000.0f; c.cameraFOV = 1.0472f; c.cameraAspectRatio = (float)w / h;
                c.cameraFwd = { 0.0f, 0.0f, 1.0f }; c.cameraUp = { 0.0f, 1.0f, 0.0f }; c.cameraRight = { 1.0f, 0.0f, 0.0f };
                slSetConstants(c, *token, vp);
            }
            Barrier(cl, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Barrier(cl, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            cl->Close();
            ID3D12CommandList* lists[] = { cl };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fence, ++fenceValue);
            allocFence[slot] = fenceValue;

            HRESULT hr = sc->Present(0, 0);
            if (FAILED(hr)) ++presentFailures;
            trackedIndex = (trackedIndex + 1) % kBuffers;
        }
    }

    // "Set all tags to null pointers" (loading, menus, cutscenes), as the DLSS-G guide asks
    void NullTags() {
        sl::ResourceTag tags[] = {
            sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
        };
        slSetTag(sl::ViewportHandle(0), tags, 3, nullptr);
    }

    bool Generating() {
        sl::DLSSGState state{};
        sl::ViewportHandle vp(0);
        if (slDLSSGGetState) slDLSSGGetState(vp, state, nullptr);
        return state.numFramesActuallyPresented == 2;
    }

    IDXGISwapChain3* Create(HWND hwnd, UINT width, UINT height, HRESULT* outHr) {
        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Width = width; scd.Height = height; scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = kBuffers; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* sc1 = nullptr;
        *outHr = factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &sc1);
        if (!sc1) return nullptr;
        IDXGISwapChain3* sc3 = nullptr;
        sc1->QueryInterface(IID_PPV_ARGS(&sc3));
        sc1->Release();
        return sc3;
    }
};

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    HMODULE sl = LoadLibraryA("sl.interposer.dll");
    if (!sl) { printf("[FAIL] cannot load sl.interposer.dll\n"); return 1; }
    auto slInit = (PFN_slInit)GetProcAddress(sl, "slInit");
    auto slShutdown = (PFN_slShutdown)GetProcAddress(sl, "slShutdown");
    auto slSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(sl, "slSetD3DDevice");
    auto slGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(sl, "slGetFeatureFunction");

    Ctx g{};
    g.slGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(sl, "slGetNewFrameToken");
    g.slSetTag = (PFN_slSetTag)GetProcAddress(sl, "slSetTag");
    g.slSetConstants = (PFN_slSetConstants)GetProcAddress(sl, "slSetConstants");

    sl::Feature features[] = { sl::kFeatureDLSS_G };
    sl::Preferences pref{};
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    CHECK(slInit(pref, sl::kSDKVersion) == sl::Result::eOk, "slInit");

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev)))) { printf("[FAIL] D3D12CreateDevice\n"); return 1; }
    CHECK(slSetD3DDevice(g.dev) == sl::Result::eOk, "slSetD3DDevice");
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));

    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"MetalStreamFGLife";
    RegisterClassW(&wc);
    HWND hwnd = MakeWindow(L"MetalStream FG lifecycle", 1280, 720, 100);

    typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
    auto createFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(LoadLibraryA("dxgi.dll"), "CreateDXGIFactory2");
    createFactory2(0, IID_PPV_ARGS(&g.factory));

    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kBuffers + 1;
    g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap));
    g.rtvStep = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (auto& a : g.alloc) g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr, IID_PPV_ARGS(&g.cl));
    g.cl->Close();
    g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    void* fn = nullptr;
    slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn);
    auto slDLSSGSetOptions = (PFN_slDLSSGSetOptions)fn;
    slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn);
    g.slDLSSGGetState = (PFN_slDLSSGGetState)fn;
    sl::DLSSGOptions gopts{};
    gopts.mode = sl::DLSSGMode::eOn;
    gopts.numFramesToGenerate = 1;
    CHECK(slDLSSGSetOptions && slDLSSGSetOptions(sl::ViewportHandle(0), gopts) == sl::Result::eOk, "slDLSSGSetOptions(eOn)");

    // 1. Swapchain A
    HRESULT hr = S_OK;
    g.sc = g.Create(hwnd, 1280, 720, &hr);
    CHECK(g.sc != nullptr, "create swapchain A -> 0x%08lX", hr);
    if (!g.sc) return 1;
    g.FetchBuffers();
    g.RenderFrames(150);
    CHECK(g.Generating(), "1. generating on swapchain A");

    // 2. Resize A while the present thread runs
    g.WaitGpu();
    g.ReleaseBuffers();
    hr = g.sc->ResizeBuffers(0, 960, 540, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr), "2. ResizeBuffers(960x540) -> 0x%08lX", hr);
    g.FetchBuffers();
    g.RenderFrames(150);
    CHECK(g.Generating() && g.w == 960, "2. generating after the resize (%ux%u)", g.w, g.h);

    // 3. Release A right after its last Present (the present thread may still hold that frame), then
    //    create B for the same window: creation fails while A is still alive
    g.RenderFrames(1);
    g.WaitGpu();
    g.ReleaseBuffers();
    ULONG left = g.sc->Release();
    g.sc = g.Create(hwnd, 960, 540, &hr);
    CHECK(g.sc != nullptr, "3. create swapchain B for the same window after releasing A (refs left %lu) -> 0x%08lX", left, hr);
    if (!g.sc) return 1;
    g.FetchBuffers();
    g.RenderFrames(150);
    CHECK(g.Generating(), "3. generating on swapchain B");

    // 3b. Null tags (a menu): generation stops and must not touch the old inputs; re-tagging resumes it
    g.NullTags();
    g.RenderFrames(30, false);
    CHECK(!g.Generating(), "3b. no frame generation while the tags are null");
    g.RenderFrames(60);
    CHECK(g.Generating(), "3b. generating again after re-tagging");

    // 4. A second swapchain C (another window): MetalStream follows C, B must keep showing the game's frames
    HWND hwnd2 = MakeWindow(L"MetalStream FG lifecycle (secondary)", 960, 540, 1200);
    IDXGISwapChain3* scC = g.Create(hwnd2, 960, 540, &hr);
    CHECK(scC != nullptr, "4. create secondary swapchain C -> 0x%08lX", hr);
    if (scC) {
        for (int i = 0; i < 5; ++i) scC->Present(0, 0);
    }
    unsigned mismatchesBefore = g.indexMismatches;
    g.RenderFrames(60, false);
    CHECK(g.indexMismatches == mismatchesBefore, "4. swapchain B keeps its backbuffer index after C appears (%u mismatches)",
          g.indexMismatches - mismatchesBefore);
    if (scC) { g.WaitGpu(); scC->Release(); }

    // 4b. C is gone and B keeps presenting: MetalStream must go back to generating frames for B
    g.RenderFrames(700);
    CHECK(g.Generating(), "4b. generating on swapchain B again after C stopped presenting");

    // 5. slShutdown, then keep presenting B
    g.WaitGpu();
    slShutdown();
    g.slAlive = false;
    mismatchesBefore = g.indexMismatches;
    g.RenderFrames(30);
    CHECK(g.indexMismatches == mismatchesBefore, "5. swapchain B keeps its backbuffer index after slShutdown (%u mismatches)",
          g.indexMismatches - mismatchesBefore);

    // 6. Release B right after a Present
    g.WaitGpu();
    g.ReleaseBuffers();
    g.sc->Release();
    g.sc = nullptr;

    CHECK(g.indexMismatches == 0, "backbuffer index always matched the game's own tracking (%u mismatches)", g.indexMismatches);
    CHECK(g.presentFailures == 0, "every Present succeeded (%u failures)", g.presentFailures);
    DestroyWindow(hwnd2);
    DestroyWindow(hwnd);
    printf("RESULT: %s (%d failure(s)) - see metalstream.log for framegen stats\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
