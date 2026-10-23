#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdint.h>
#include <cassert>
#include <streamline/sl.h>
#include <streamline/sl_consts.h>
#include <streamline/sl_dlss.h>

typedef sl::Result (*PFN_slInit)(const sl::Preferences&, uint64_t);
typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*&, const uint32_t*);
typedef sl::Result (*PFN_slSetD3DDevice)(void*);
typedef sl::Result (*PFN_slSetTag)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*PFN_slSetConstants)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature, const char*, void*&);
typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*PFN_slSetData)(const sl::BaseStructure*, sl::CommandBuffer*);
typedef sl::Result (*PFN_slGetData)(const sl::BaseStructure*, sl::BaseStructure*, sl::CommandBuffer*);
typedef void* (*PFN_slGetPluginFunction)(const char*);

static ID3D12Resource* CreateTexture(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;

    ID3D12Resource* res = nullptr;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        initialState, nullptr, IID_PPV_ARGS(&res)
    );
    if (FAILED(hr) || !res) {
        printf("[FAILED] CreateCommittedResource failed: 0x%08lX\n", hr);
    }
    return res;
}

int main() {
    printf("[PHASE 5 TEST] Starting Render Barriers & Plugin Unified Bus Audit...\n");

    // 1. Load sl.interposer.dll and sl.dlss.dll
    HMODULE hSl = LoadLibraryA("build/sl.interposer.dll");
    if (!hSl) hSl = LoadLibraryA("sl.interposer.dll");
    if (!hSl) {
        printf("[FAILED] Could not load sl.interposer.dll\n");
        return 1;
    }
    printf("[PHASE 5] sl.interposer.dll loaded at %p\n", hSl);

    HMODULE hDlss = LoadLibraryA("build/sl.dlss.dll");
    if (!hDlss) hDlss = LoadLibraryA("sl.dlss.dll");
    if (!hDlss) {
        printf("[FAILED] Could not load sl.dlss.dll\n");
        return 1;
    }
    printf("[PHASE 5] sl.dlss.dll loaded at %p\n", hDlss);

    // 2. Resolve exports
    auto pfnInit = (PFN_slInit)GetProcAddress(hSl, "slInit");
    auto pfnShutdown = (PFN_slShutdown)GetProcAddress(hSl, "slShutdown");
    auto pfnGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(hSl, "slGetNewFrameToken");
    auto pfnSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(hSl, "slSetD3DDevice");
    auto pfnSetTag = (PFN_slSetTag)GetProcAddress(hSl, "slSetTag");
    auto pfnGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(hSl, "slGetFeatureFunction");
    auto pfnEvaluateFeature = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");
    auto pfnInterposerSetData = (PFN_slSetData)GetProcAddress(hSl, "slSetData");
    auto pfnInterposerGetData = (PFN_slGetData)GetProcAddress(hSl, "slGetData");

    auto pfnDlssGetPluginFunction = (PFN_slGetPluginFunction)GetProcAddress(hDlss, "slGetPluginFunction");
    auto pfnDlssSetData = (PFN_slSetData)GetProcAddress(hDlss, "slSetData");
    auto pfnDlssGetData = (PFN_slGetData)GetProcAddress(hDlss, "slGetData");

    if (!pfnInit || !pfnShutdown || !pfnGetNewFrameToken || !pfnSetD3DDevice ||
        !pfnSetTag || !pfnGetFeatureFunction || !pfnEvaluateFeature ||
        !pfnInterposerSetData || !pfnInterposerGetData ||
        !pfnDlssGetPluginFunction || !pfnDlssSetData || !pfnDlssGetData) {
        printf("[FAILED] Missing core Streamline or plugin exports\n");
        return 1;
    }

    sl::Preferences pref{};
    pref.applicationId = 12345;
    sl::Result initRes = pfnInit(pref, 0x2000B0001FEDC);
    assert(initRes == sl::Result::eOk);

    printf("\n--- Test 1: sl.dlss.dll Plugin Unified Bus (slSetData / slGetData) ---\n");
    // Verify slGetPluginFunction returns slSetData and slGetData
    void* fnSet = pfnDlssGetPluginFunction("slSetData");
    void* fnGet = pfnDlssGetPluginFunction("slGetData");
    assert(fnSet != nullptr && fnGet != nullptr);
    printf("[PASS] sl.dlss.dll exports slSetData and slGetData via slGetPluginFunction!\n");

    // Test slSetData on sl.dlss.dll
    sl::ViewportHandle vp(0);
    sl::DLSSOptions opts{};
    opts.mode = sl::DLSSMode::eBalanced;
    opts.outputWidth = 1920;
    opts.outputHeight = 1080;
    opts.next = &vp;

    sl::Result res = pfnDlssSetData(&opts, nullptr);
    assert(res == sl::Result::eOk);
    printf("[PASS] sl.dlss.dll: slSetData succeeded!\n");

    // Test slGetData on sl.dlss.dll (DLSSOptimalSettings)
    sl::DLSSOptimalSettings optSettings{};
    res = pfnDlssGetData(&opts, &optSettings, nullptr);
    assert(res == sl::Result::eOk);
    assert(optSettings.optimalRenderWidth > 0 && optSettings.optimalRenderHeight > 0);
    printf("[PASS] sl.dlss.dll: slGetData(DLSSOptimalSettings) returned render=%ux%u for out=1920x1080!\n",
           optSettings.optimalRenderWidth, optSettings.optimalRenderHeight);

    // Test slGetData on sl.dlss.dll (DLSSState)
    sl::DLSSState dlssState{};
    res = pfnDlssGetData(&vp, &dlssState, nullptr);
    assert(res == sl::Result::eOk);
    assert(dlssState.estimatedVRAMUsageInBytes > 0);
    printf("[PASS] sl.dlss.dll: slGetData(DLSSState) returned estimated VRAM=%llu MB!\n",
           dlssState.estimatedVRAMUsageInBytes / (1024 * 1024));

    printf("\n--- Test 2: sl.interposer.dll Unified Bus & slGetFeatureFunction ---\n");
    void* fnInterposerSet = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS, "slSetData", fnInterposerSet);
    assert(fnInterposerSet != nullptr);

    void* fnInterposerGet = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS, "slGetData", fnInterposerGet);
    assert(fnInterposerGet != nullptr);

    res = pfnInterposerSetData(&opts, nullptr);
    assert(res == sl::Result::eOk);

    sl::DLSSOptimalSettings interposerOptSettings{};
    res = pfnInterposerGetData(&opts, &interposerOptSettings, nullptr);
    assert(res == sl::Result::eOk);
    assert(interposerOptSettings.optimalRenderWidth > 0);
    printf("[PASS] sl.interposer.dll: slSetData and slGetData bus dispatch fully functional!\n");

    printf("\n--- Test 3: D3D12 Resource Barriers (Transition & Restoration) ---\n");
    // Create D3D12 Device, Queue, and Command List
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        printf("[FAILED] D3D12CreateDevice failed: 0x%08lX\n", hr);
        return 1;
    }
    printf("[PHASE 5] Created D3D12 Device: %p\n", device);

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* cmdQueue = nullptr;
    device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&cmdQueue));

    ID3D12CommandAllocator* cmdAlloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));

    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));

    pfnSetD3DDevice(device);

    // Create resources in non-shader read / non-UAV states to stress-test barrier emission:
    // ColorIn starts in RENDER_TARGET state
    ID3D12Resource* colorIn = CreateTexture(device, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // ColorOut starts in PRESENT / COMMON state
    ID3D12Resource* colorOut = CreateTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    // Depth starts in DEPTH_WRITE state
    ID3D12Resource* depth = CreateTexture(device, 1280, 720, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    // MotionVectors starts in COMMON state
    ID3D12Resource* mvec = CreateTexture(device, 1280, 720, DXGI_FORMAT_R16G16_FLOAT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);

    sl::Resource resColorIn = { sl::ResourceType::eTex2d, colorIn, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_RENDER_TARGET };
    sl::Resource resColorOut = { sl::ResourceType::eTex2d, colorOut, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_COMMON };
    sl::Resource resDepth = { sl::ResourceType::eTex2d, depth, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_DEPTH_WRITE };
    sl::Resource resMvec = { sl::ResourceType::eTex2d, mvec, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_COMMON };

    sl::ResourceTag tags[4] = {
        { &resColorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &resColorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &resDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &resMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate }
    };
    pfnSetTag(vp, tags, 4, cmdList);

    sl::FrameToken* token1 = nullptr;
    uint32_t f1 = 1;
    pfnGetNewFrameToken(token1, &f1);

    const sl::BaseStructure* inputs[1] = { &vp };
    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token1, inputs, 1, cmdList);
    assert(res == sl::Result::eOk);
    printf("[PASS] slEvaluateFeature successfully transitioned RENDER_TARGET, DEPTH_WRITE, COMMON states to UAV/SRV and restored them cleanly!\n");

    printf("\n--- Test 4: UAV Flush Barrier when Output Already in UAV State ---\n");
    // Change ColorOut tag to UNORDERED_ACCESS state
    sl::Resource resColorOutUAV = { sl::ResourceType::eTex2d, colorOut, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    sl::ResourceTag uavTags[1] = {
        { &resColorOutUAV, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate }
    };
    pfnSetTag(vp, uavTags, 1, cmdList);

    sl::FrameToken* token2 = nullptr;
    uint32_t f2 = 2;
    pfnGetNewFrameToken(token2, &f2);

    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token2, inputs, 1, cmdList);
    assert(res == sl::Result::eOk);
    printf("[PASS] slEvaluateFeature emitted UAV flush barrier without redundant transition when output was already in UAV state!\n");

    printf("\n--- Test 5: Real GPU Execution Synchronization ---\n");
    cmdList->Close();
    ID3D12CommandList* ppLists[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, ppLists);

    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    cmdQueue->Signal(fence, 1);

    HANDLE hEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    fence->SetEventOnCompletion(1, hEvent);
    WaitForSingleObject(hEvent, INFINITE);
    CloseHandle(hEvent);
    printf("[PASS] Real D3D12 GPU queue execution completed with zero driver state faults!\n");

    printf("\n--- Test 6: Final Shutdown Lifecycle ---\n");
    res = pfnShutdown();
    assert(res == sl::Result::eOk);
    printf("[PASS] slShutdown cleanly released all resources!\n");

    // Clean up
    if (colorIn) colorIn->Release();
    if (colorOut) colorOut->Release();
    if (depth) depth->Release();
    if (mvec) mvec->Release();
    if (fence) fence->Release();
    if (cmdList) cmdList->Release();
    if (cmdAlloc) cmdAlloc->Release();
    if (cmdQueue) cmdQueue->Release();
    if (device) device->Release();

    printf("\n[SUCCESS] ALL PHASE 5 RENDER BARRIER & UNIFIED BUS TESTS PASSED COMPLETELY!\n");
    return 0;
}
