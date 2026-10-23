// Real-GPU end-to-end test for DLSS-G frame generation (needs Wine + GPTk, run via
// scripts/run_gptk_tests.sh). Drives sl.interposer.dll like a game would:
//   slInit(DLSS-G) -> D3D12 device -> factory/swapchain from the system dxgi.dll
//   -> slDLSSGSetOptions(eOn) -> per frame: render, slSetTag, slSetConstants, Present
// and checks that MetalStream's Present hook generated frames.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
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

static const UINT W = 1280, H = 720, kBuffers = 3, kFrames = 420;
static int g_failures = 0;

#define CHECK(cond, ...) do { \
        if (cond) { printf("[PASS] "); } else { printf("[FAIL] "); ++g_failures; } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static ID3D12Resource* MakeTex(ID3D12Device* dev, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H;
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

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // METALSTREAM_TEST_SYNC=0: present without vsync, which makes MetalStream pace on its present thread
    const char* syncEnv = getenv("METALSTREAM_TEST_SYNC");
    const UINT syncInterval = (syncEnv && syncEnv[0] == '0') ? 0 : 1;
    printf("[INFO] presenting with SyncInterval=%u\n", syncInterval);

    HMODULE sl = LoadLibraryA("sl.interposer.dll");
    if (!sl) { printf("[FAIL] cannot load sl.interposer.dll\n"); return 1; }
    auto slInit = (PFN_slInit)GetProcAddress(sl, "slInit");
    auto slShutdown = (PFN_slShutdown)GetProcAddress(sl, "slShutdown");
    auto slSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(sl, "slSetD3DDevice");
    auto slGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(sl, "slGetNewFrameToken");
    auto slSetTag = (PFN_slSetTag)GetProcAddress(sl, "slSetTag");
    auto slSetConstants = (PFN_slSetConstants)GetProcAddress(sl, "slSetConstants");
    auto slGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(sl, "slGetFeatureFunction");

    sl::Feature features[] = { sl::kFeatureDLSS_G };
    sl::Preferences pref{};
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.logLevel = sl::LogLevel::eDefault;
    CHECK(slInit(pref, sl::kSDKVersion) == sl::Result::eOk, "slInit");

    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) { printf("[FAIL] D3D12CreateDevice\n"); return 1; }
    CHECK(slSetD3DDevice(dev) == sl::Result::eOk, "slSetD3DDevice");

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"MetalStreamFG";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"MetalStreamFG", L"MetalStream FG test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, W, H, nullptr, nullptr, wc.hInstance, nullptr);

    // Factory straight from the system dxgi.dll: exercises the global vtable hook installed by slInit
    typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
    auto createFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(LoadLibraryA("dxgi.dll"), "CreateDXGIFactory2");
    IDXGIFactory4* factory = nullptr;
    createFactory2(0, IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = W; scd.Height = H; scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kBuffers; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &sc1);
    CHECK(SUCCEEDED(hr) && sc1, "CreateSwapChainForHwnd -> 0x%08lX", hr);
    if (!sc1) return 1;
    IDXGISwapChain3* sc = nullptr;
    sc1->QueryInterface(IID_PPV_ARGS(&sc));

    void* fn = nullptr;
    slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn);
    auto slDLSSGSetOptions = (PFN_slDLSSGSetOptions)fn;
    slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn);
    auto slDLSSGGetState = (PFN_slDLSSGGetState)fn;
    sl::ViewportHandle vp(0);
    sl::DLSSGOptions gopts{};
    gopts.mode = sl::DLSSGMode::eOn;
    gopts.numFramesToGenerate = 1;
    CHECK(slDLSSGSetOptions && slDLSSGSetOptions(vp, gopts) == sl::Result::eOk, "slDLSSGSetOptions(eOn)");

    // Rendering resources: backbuffers + HUD-less color, depth, motion vectors
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kBuffers + 1;
    ID3D12DescriptorHeap* rtvHeap; dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    UINT rtvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvBase = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    ID3D12Resource* bb[kBuffers];
    for (UINT i = 0; i < kBuffers; ++i) {
        sc->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvBase; h.ptr += i * rtvStep;
        dev->CreateRenderTargetView(bb[i], nullptr, h);
    }
    ID3D12Resource* hudless = MakeTex(dev, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE hudlessRtv = rtvBase; hudlessRtv.ptr += kBuffers * rtvStep;
    dev->CreateRenderTargetView(hudless, nullptr, hudlessRtv);
    ID3D12Resource* depth = MakeTex(dev, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* mvecs = MakeTex(dev, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12CommandAllocator* alloc[kBuffers];
    for (auto& a : alloc) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    ID3D12GraphicsCommandList* cl;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ID3D12Fence* fence; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue = 0, allocFence[kBuffers] = {};

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    UINT presentCountStart = 0;
    sc->GetLastPresentCount(&presentCountStart);
    QueryPerformanceCounter(&t0);

    // At least kFrames frames and 6.5s, so MetalStream logs at least one 5s statistics window even
    // when the game runs uncapped
    UINT framesRendered = 0;
    auto elapsedSeconds = [&]() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart;
    };
    for (UINT frame = 0; frame < kFrames || elapsedSeconds() < 6.5; ++frame, ++framesRendered) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

        // Like Unity: track the backbuffer index ourselves instead of asking the swapchain. Frame
        // generation presents twice per frame, so this only works if the game sees proxy buffers.
        UINT idx = frame % kBuffers;
        if (frame == 0) CHECK(sc->GetCurrentBackBufferIndex() == 0, "initial backbuffer index is 0");
        UINT slot = frame % kBuffers;
        if (fence->GetCompletedValue() < allocFence[slot]) { fence->SetEventOnCompletion(allocFence[slot], ev); WaitForSingleObject(ev, 5000); }
        alloc[slot]->Reset();
        cl->Reset(alloc[slot], nullptr);

        sl::FrameToken* token = nullptr;
        slGetNewFrameToken(token, &frame);

        // "Scene": a color that moves over time, rendered to both the HUD-less buffer and the backbuffer
        float t = frame / 60.0f;
        float color[4] = { 0.5f + 0.5f * sinf(t * 3.0f), 0.5f + 0.5f * sinf(t * 2.0f + 1.0f), 0.5f + 0.5f * sinf(t + 2.0f), 1.0f };
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvBase; rtv.ptr += idx * rtvStep;
        Barrier(cl, bb[idx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ClearRenderTargetView(rtv, color, 0, nullptr);
        cl->ClearRenderTargetView(hudlessRtv, color, 0, nullptr);
        Barrier(cl, hudless, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // HUD-less/mvecs live until Present; depth is tagged volatile to exercise the copy path
        sl::Resource rHudless(sl::ResourceType::eTex2d, hudless, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource rDepth(sl::ResourceType::eTex2d, depth, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource rMvecs(sl::ResourceType::eTex2d, mvecs, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::ResourceTag tags[] = {
            sl::ResourceTag(&rHudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow),
            sl::ResourceTag(&rMvecs, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
        };
        slSetTag(vp, tags, 3, cl);

        sl::Constants c{};
        c.mvecScale = { 1.0f / W, 1.0f / H };
        c.jitterOffset = { 0.0f, 0.0f };
        c.depthInverted = sl::Boolean::eTrue;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.reset = frame == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.cameraNear = 0.1f; c.cameraFar = 1000.0f; c.cameraFOV = 1.0472f; c.cameraAspectRatio = (float)W / H;
        c.cameraFwd = { 0.0f, 0.0f, 1.0f }; c.cameraUp = { 0.0f, 1.0f, 0.0f }; c.cameraRight = { 1.0f, 0.0f, 0.0f };
        slSetConstants(c, *token, vp);

        Barrier(cl, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(cl, bb[idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        cl->Close();
        ID3D12CommandList* lists[] = { cl };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++fenceValue);
        allocFence[slot] = fenceValue;

        // The hudless buffer is back in RENDER_TARGET when Present runs; re-tag with that state so
        // the hook's transitions match reality
        sl::Resource rHudlessRt(sl::ResourceType::eTex2d, hudless, nullptr, nullptr, D3D12_RESOURCE_STATE_RENDER_TARGET);
        sl::ResourceTag hudTag(&rHudlessRt, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent);
        slSetTag(vp, &hudTag, 1, nullptr);

        hr = sc->Present(syncInterval, 0);
        if (FAILED(hr)) { CHECK(false, "Present frame %u -> 0x%08lX", frame, hr); break; }
    }

    QueryPerformanceCounter(&t1);
    UINT presentCountEnd = 0;
    HRESULT hrCount = sc->GetLastPresentCount(&presentCountEnd);
    double seconds = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;

    sl::DLSSGState state{};
    if (slDLSSGGetState) slDLSSGGetState(vp, state, nullptr);
    // Uncapped without vsync the game outruns the display and only real frames fit, which
    // numFramesActuallyPresented reports as 1; otherwise every game frame shows a generated one too
    const char* expectEvenState = getenv("METALSTREAM_TEST_EXPECT_EVEN");
    const bool uncappedState = syncInterval == 0 && !(expectEvenState && expectEvenState[0] == '1');
    CHECK(uncappedState ? state.numFramesActuallyPresented >= 1 : state.numFramesActuallyPresented == 2,
          "DLSS-G active after %u frames (numFramesActuallyPresented=%u)", framesRendered, state.numFramesActuallyPresented);
    printf("[INFO] %u game frames in %.2fs (%.1f game fps); GetLastPresentCount hr=0x%08lX delta=%u\n",
           framesRendered, seconds, framesRendered / seconds, hrCount, presentCountEnd - presentCountStart);
    // Uncapped without vsync the game outruns the display, so MetalStream presents at most one frame
    // per refresh instead of two per game frame
    const char* expectEvenEnv = getenv("METALSTREAM_TEST_EXPECT_EVEN");
    const bool uncappedNoVsync = syncInterval == 0 && !(expectEvenEnv && expectEvenEnv[0] == '1');
    if (SUCCEEDED(hrCount) && presentCountEnd != presentCountStart && !uncappedNoVsync) {
        CHECK(presentCountEnd - presentCountStart >= framesRendered * 3 / 2, "swapchain presented ~2x the game frames");
    }

    queue->Signal(fence, ++fenceValue);
    fence->SetEventOnCompletion(fenceValue, ev);
    WaitForSingleObject(ev, 5000);
    slShutdown();
    DestroyWindow(hwnd);

    // Without vsync the present thread must never bunch presents (any game frame rate), and must
    // space the generated and the real frame evenly when the game is capped
    // (METALSTREAM_TEST_EXPECT_EVEN=1, metalstream.ini FGMaxGameFps=-1)
    if (syncInterval == 0) {
        FILE* log = fopen("metalstream.log", "r");
        char line[1024];
        double genToReal = 0, realToGen = 0;
        unsigned long long presents = 0, bursts = 0;
        int pacingLines = 0;
        bool activated = false;
        while (log && fgets(line, sizeof line, log)) {
            if (strstr(line, "framegen: activated")) activated = true;
            const char* p = strstr(line, "pacing: generated->real ");
            if (!p) continue;
            const char* q = strstr(p, "real->generated ");
            const char* r = strstr(p, "presents ");
            if (q && r && sscanf(p, "pacing: generated->real %lfms", &genToReal) == 1 &&
                sscanf(q, "real->generated %lfms", &realToGen) == 1 &&
                sscanf(r, "presents %llu, bursts %llu", &presents, &bursts) == 2) {
                ++pacingLines;
            }
        }
        if (log) fclose(log);
        CHECK(activated, "frame generation activated (metalstream.log)");
        printf("[INFO] last pacing sample: generated->real %.2fms, real->generated %.2fms, %llu presents, %llu bursts (%d samples)\n",
               genToReal, realToGen, presents, bursts, pacingLines);
        CHECK(pacingLines > 0 && presents > 0 && bursts * 50 <= presents,
              "present thread never bunches presents (bursts <= 2%%)");
        const char* expectEven = getenv("METALSTREAM_TEST_EXPECT_EVEN");
        if (expectEven && expectEven[0] == '1') {
            CHECK(genToReal > 0 && realToGen > 0 && fabs(genToReal - realToGen) < 0.35 * (genToReal + realToGen),
                  "capped game: generated and real frames evenly spaced");
        }
    }

    // With FGCapture=2 MetalStream dumped 3 consecutive frames: the real frames must be the game's
    // consecutive (different) frames, not stale buffer contents
    {
        uint8_t px[3][4] = {};
        int found = 0;
        for (int k = 0; k < 3; ++k) {
            char path[128];
            snprintf(path, sizeof path, "metalstream_capture\\burst01_frame%d_real.raw", k);
            FILE* f = fopen(path, "rb");
            if (!f) break;
            fseek(f, (long)(((H / 2) * W + W / 2) * 4), SEEK_SET); // tight pitch: 1280 * 4 is 256-aligned
            if (fread(px[k], 1, 4, f) == 4) ++found;
            fclose(f);
        }
        if (found == 3) {
            printf("[INFO] captured real frames center RGB: (%u,%u,%u) (%u,%u,%u) (%u,%u,%u)\n",
                   px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2], px[2][0], px[2][1], px[2][2]);
            auto differ = [&](int a, int b) {
                return abs(px[a][0] - px[b][0]) + abs(px[a][1] - px[b][1]) + abs(px[a][2] - px[b][2]) > 2;
            };
            CHECK(differ(0, 1) && differ(1, 2) && differ(0, 2), "consecutive captured real frames are distinct game frames");
        } else {
            printf("[INFO] no capture found (run with FGCapture=2 to check real-frame freshness)\n");
        }
    }

    printf("RESULT: %s (%d failure(s)) - see metalstream.log for framegen stats\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
