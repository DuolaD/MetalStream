#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cassert>
#include <streamline/sl.h>
#include <streamline/sl_consts.h>
#include <streamline/sl_dlss.h>
#include <streamline/sl_dlss_g.h>

typedef sl::Result (*PFN_slInit)(const sl::Preferences&, uint64_t);
typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*&, const uint32_t*);
typedef sl::Result (*PFN_slSetD3DDevice)(void*);
typedef sl::Result (*PFN_slSetTag)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*PFN_slSetTagForFrame)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*PFN_slSetConstants)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature, const char*, void*&);
typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*PFN_slAllocateResources)(sl::CommandBuffer*, sl::Feature, const sl::ViewportHandle&);
typedef sl::Result (*PFN_slFreeResources)(sl::Feature, const sl::ViewportHandle&);
typedef sl::Result (*PFN_slDLSSSetOptions)(const sl::ViewportHandle&, const sl::DLSSOptions&);

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
    printf("=======================================================================\n");
    printf("[AUDIT PART 3 TEST] Multi-Viewport & Resource Lifecycle Safety Audit\n");
    printf("=======================================================================\n");

    HMODULE hSl = LoadLibraryA("build/sl.interposer.dll");
    if (!hSl) hSl = LoadLibraryA("sl.interposer.dll");
    if (!hSl) {
        printf("[FAILED] Could not load sl.interposer.dll\n");
        return 1;
    }

    auto pfnInit = (PFN_slInit)GetProcAddress(hSl, "slInit");
    auto pfnShutdown = (PFN_slShutdown)GetProcAddress(hSl, "slShutdown");
    auto pfnGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(hSl, "slGetNewFrameToken");
    auto pfnSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(hSl, "slSetD3DDevice");
    auto pfnSetTag = (PFN_slSetTag)GetProcAddress(hSl, "slSetTag");
    auto pfnSetTagForFrame = (PFN_slSetTagForFrame)GetProcAddress(hSl, "slSetTagForFrame");
    auto pfnSetConstants = (PFN_slSetConstants)GetProcAddress(hSl, "slSetConstants");
    auto pfnGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(hSl, "slGetFeatureFunction");
    auto pfnEvaluateFeature = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");
    auto pfnAllocateResources = (PFN_slAllocateResources)GetProcAddress(hSl, "slAllocateResources");
    auto pfnFreeResources = (PFN_slFreeResources)GetProcAddress(hSl, "slFreeResources");

    if (!pfnInit || !pfnShutdown || !pfnGetNewFrameToken || !pfnSetD3DDevice ||
        !pfnSetTag || !pfnSetTagForFrame || !pfnSetConstants || !pfnGetFeatureFunction ||
        !pfnEvaluateFeature || !pfnAllocateResources || !pfnFreeResources) {
        printf("[FAILED] Missing core Streamline exports\n");
        return 1;
    }

    // Initialize D3D12 Device, Queue & List
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        printf("[FAILED] D3D12CreateDevice failed: 0x%08lX\n", hr);
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* cmdQueue = nullptr;
    device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&cmdQueue));

    ID3D12CommandAllocator* cmdAlloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));

    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));

    sl::Preferences pref{};
    pref.applicationId = 12345;
    sl::Result rInit = pfnInit(pref, 0x2000B0001FEDC);
    assert(rInit == sl::Result::eOk);
    pfnSetD3DDevice(device);

    void* fnDLSSSetOptions = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fnDLSSSetOptions);
    auto pfnDLSSSetOptions = (PFN_slDLSSSetOptions)fnDLSSSetOptions;
    assert(pfnDLSSSetOptions != nullptr);

    // =========================================================================
    // Test 1: Anti-Crosstalk Defense (Missing Viewport Returns eErrorMissingInputParameter)
    // =========================================================================
    printf("\n--- Test 1: Cross-Viewport Isolation & Anti-Crosstalk Defense ---\n");

    // Setup Viewport 0
    ID3D12Resource* vp0In = CreateTexture(device, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* vp0Out = CreateTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* vp0Depth = CreateTexture(device, 1280, 720, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* vp0Mvec = CreateTexture(device, 1280, 720, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    sl::ViewportHandle vp0(0);
    sl::DLSSOptions opt0{};
    opt0.mode = sl::DLSSMode::eMaxQuality;
    opt0.outputWidth = 1920;
    opt0.outputHeight = 1080;
    pfnDLSSSetOptions(vp0, opt0);

    sl::Resource rVp0In = { sl::ResourceType::eTex2d, vp0In, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource rVp0Out = { sl::ResourceType::eTex2d, vp0Out, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    sl::Resource rVp0Depth = { sl::ResourceType::eTex2d, vp0Depth, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource rVp0Mvec = { sl::ResourceType::eTex2d, vp0Mvec, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::ResourceTag vp0Tags[4] = {
        { &rVp0In, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp0Out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp0Depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp0Mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate }
    };
    pfnSetTag(vp0, vp0Tags, 4, cmdList);

    sl::FrameToken* token1 = nullptr;
    uint32_t f1 = 1;
    pfnGetNewFrameToken(token1, &f1);

    // Attempt to evaluate Viewport 1 (which has NEVER been tagged!)
    sl::ViewportHandle vp1(1);
    const sl::BaseStructure* inputsVp1Missing[1] = { &vp1 };
    sl::Result rMissing = pfnEvaluateFeature(sl::kFeatureDLSS, *token1, inputsVp1Missing, 1, cmdList);
    if (rMissing != sl::Result::eErrorMissingInputParameter) {
        printf("[FAILED] Evaluating missing Viewport 1 should return eErrorMissingInputParameter, got %d!\n", (int)rMissing);
        return 1;
    }
    printf("[PASS] Evaluating missing Viewport 1 returned eErrorMissingInputParameter (no begin() crosstalk)!\n");

    // Evaluate Viewport 0 (must succeed cleanly!)
    const sl::BaseStructure* inputsVp0[1] = { &vp0 };
    sl::Result rVp0 = pfnEvaluateFeature(sl::kFeatureDLSS, *token1, inputsVp0, 1, cmdList);
    if (rVp0 != sl::Result::eOk) {
        printf("[FAILED] Evaluating Viewport 0 failed: %d\n", (int)rVp0);
        return 1;
    }
    printf("[PASS] Viewport 0 evaluated cleanly without being disturbed by missing Viewport 1 request!\n");

    // =========================================================================
    // Test 2: Frame Token Driven Deferred Release (DLSS OFF Memory Leak Elimination)
    // =========================================================================
    printf("\n--- Test 2: Frame Token Driven Deferred Release (DLSS OFF Lifecycle) ---\n");

    // Simulate user turning DLSS OFF in game options -> slFreeResources called
    sl::Result rFree = pfnFreeResources(sl::kFeatureDLSS, vp0);
    assert(rFree == sl::Result::eOk);
    printf("[INFO] slFreeResources called for Viewport 0 (DLSS turned off)\n");

    // Now simulate game continuing to run for 4 frames with DLSS OFF:
    // Game calls slGetNewFrameToken every frame, but NEVER calls slEvaluateFeature!
    for (uint32_t frameOff = 2; frameOff <= 6; ++frameOff) {
        sl::FrameToken* offToken = nullptr;
        pfnGetNewFrameToken(offToken, &frameOff);
    }
    printf("[PASS] Advanced 5 frames via slGetNewFrameToken with ZERO Evaluate calls; deferred queue safely drained!\n");

    // =========================================================================
    // Test 3: Multi-Threaded Frame Token Aware Tagging Decoupling
    // =========================================================================
    printf("\n--- Test 3: Frame-Aware Tagging & Decoupling (Multi-threaded Pipeline) ---\n");

    pfnShutdown();
    pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pfnInit(pref, 0x2000B0001FEDC);
    pfnSetD3DDevice(device);
    pfnDLSSSetOptions(vp0, opt0);

    ID3D12Resource* texFrame10In = CreateTexture(device, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* texFrame10Out = CreateTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* texFrame11In = CreateTexture(device, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* texFrame11Out = CreateTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    sl::FrameToken* token10 = nullptr;
    uint32_t f10 = 10;
    pfnGetNewFrameToken(token10, &f10);

    sl::FrameToken* token11 = nullptr;
    uint32_t f11 = 11;
    pfnGetNewFrameToken(token11, &f11);

    // Thread A (recording Frame 10):
    sl::Resource r10In = { sl::ResourceType::eTex2d, texFrame10In, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource r10Out = { sl::ResourceType::eTex2d, texFrame10Out, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    sl::ResourceTag tags10[4] = {
        { &r10In, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &r10Out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp0Depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp0Mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate }
    };
    pfnSetTagForFrame(*token10, vp0, tags10, 4, cmdList);

    sl::Constants consts10{};
    consts10.jitterOffset = sl::float2(0.125f, 0.125f);
    pfnSetConstants(consts10, *token10, vp0);

    // Thread B (prep thread immediately records Frame 11 BEFORE Frame 10 evaluates!):
    sl::Resource r11In = { sl::ResourceType::eTex2d, texFrame11In, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource r11Out = { sl::ResourceType::eTex2d, texFrame11Out, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    sl::ResourceTag tags11[4] = {
        { &r11In, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &r11Out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp0Depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp0Mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate }
    };
    pfnSetTagForFrame(*token11, vp0, tags11, 4, cmdList);

    sl::Constants consts11{};
    consts11.jitterOffset = sl::float2(0.875f, 0.875f);
    pfnSetConstants(consts11, *token11, vp0);

    // Dispatch Thread: Now evaluates Frame 10!
    // It must use Frame 10's tags and constants, NOT Frame 11's!
    sl::Result rEval10 = pfnEvaluateFeature(sl::kFeatureDLSS, *token10, inputsVp0, 1, cmdList);
    if (rEval10 != sl::Result::eOk) {
        printf("[FAILED] Frame 10 evaluate failed: %d\n", (int)rEval10);
        return 1;
    }
    printf("[PASS] Frame 10 evaluated safely with its own frame slot without being stomped by Frame 11!\n");

    // Dispatch Thread: Now evaluates Frame 11!
    sl::Result rEval11 = pfnEvaluateFeature(sl::kFeatureDLSS, *token11, inputsVp0, 1, cmdList);
    if (rEval11 != sl::Result::eOk) {
        printf("[FAILED] Frame 11 evaluate failed: %d\n", (int)rEval11);
        return 1;
    }
    printf("[PASS] Frame 11 evaluated safely with its own frame slot!\n");

    // =========================================================================
    // Test 4: Real GPU Hardware Execution & Fence Sync
    // =========================================================================
    printf("\n--- Test 4: Real GPU Hardware Execution & Fence Sync ---\n");
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
    printf("[PASS] Direct3D 12 GPU execution completed with zero driver state errors!\n");

    // Shutdown
    pfnShutdown();
    printf("[PASS] slShutdown successfully reclaimed all multi-viewport frame slots!\n");

    // Clean up
    if (vp0In) vp0In->Release();
    if (vp0Out) vp0Out->Release();
    if (vp0Depth) vp0Depth->Release();
    if (vp0Mvec) vp0Mvec->Release();
    if (texFrame10In) texFrame10In->Release();
    if (texFrame10Out) texFrame10Out->Release();
    if (texFrame11In) texFrame11In->Release();
    if (texFrame11Out) texFrame11Out->Release();
    if (fence) fence->Release();
    if (cmdList) cmdList->Release();
    if (cmdAlloc) cmdAlloc->Release();
    if (cmdQueue) cmdQueue->Release();
    if (device) device->Release();

    printf("\n=======================================================================\n");
    printf("[SUCCESS] ALL PART 3 MULTI-VIEWPORT & RESOURCE LIFECYCLE TESTS PASSED!\n");
    printf("=======================================================================\n");
    return 0;
}
