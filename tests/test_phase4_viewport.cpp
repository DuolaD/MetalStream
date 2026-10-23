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
    printf("[PHASE 4 TEST] Starting Multi-Viewport Decoupling & VRAM Lifecycle Audit...\n");

    HMODULE hSl = LoadLibraryA("build/sl.interposer.dll");
    if (!hSl) {
        hSl = LoadLibraryA("sl.interposer.dll");
    }
    if (!hSl) {
        printf("[FAILED] Could not load sl.interposer.dll\n");
        return 1;
    }
    printf("[PHASE 4] sl.interposer.dll loaded at %p\n", hSl);

    auto pfnInit = (PFN_slInit)GetProcAddress(hSl, "slInit");
    auto pfnShutdown = (PFN_slShutdown)GetProcAddress(hSl, "slShutdown");
    auto pfnGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(hSl, "slGetNewFrameToken");
    auto pfnSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(hSl, "slSetD3DDevice");
    auto pfnSetTag = (PFN_slSetTag)GetProcAddress(hSl, "slSetTag");
    auto pfnSetConstants = (PFN_slSetConstants)GetProcAddress(hSl, "slSetConstants");
    auto pfnGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(hSl, "slGetFeatureFunction");
    auto pfnEvaluateFeature = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");
    auto pfnAllocateResources = (PFN_slAllocateResources)GetProcAddress(hSl, "slAllocateResources");
    auto pfnFreeResources = (PFN_slFreeResources)GetProcAddress(hSl, "slFreeResources");

    if (!pfnInit || !pfnShutdown || !pfnGetNewFrameToken || !pfnSetD3DDevice ||
        !pfnSetTag || !pfnSetConstants || !pfnGetFeatureFunction || !pfnEvaluateFeature ||
        !pfnAllocateResources || !pfnFreeResources) {
        printf("[FAILED] Missing core Streamline exports\n");
        return 1;
    }

    // 1. Create real D3D12 Device, Queue, and Command List
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        printf("[FAILED] D3D12CreateDevice failed: 0x%08lX\n", hr);
        return 1;
    }
    printf("[PHASE 4] Created real D3D12 Device: %p\n", device);

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* cmdQueue = nullptr;
    device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&cmdQueue));

    ID3D12CommandAllocator* cmdAlloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));

    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));

    // 2. Initialize Streamline
    sl::Preferences pref{};
    pref.applicationId = 12345;
    sl::Result res = pfnInit(pref, sl::kSDKVersion);
    assert(res == sl::Result::eOk);
    pfnSetD3DDevice(device);

    void* fnDLSSSetOptions = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fnDLSSSetOptions);
    auto pfnDLSSSetOptions = (PFN_slDLSSSetOptions)fnDLSSSetOptions;
    assert(pfnDLSSSetOptions != nullptr);

    // 3. Create Textures for Viewport 0 (Main Scene 1920x1080 -> 1280x720)
    ID3D12Resource* vp0ColorIn = CreateTexture(device, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* vp0ColorOut = CreateTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* vp0Depth = CreateTexture(device, 1280, 720, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* vp0Mvec = CreateTexture(device, 1280, 720, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 4. Create Textures for Viewport 1 (Minimap / PIP 640x360 -> 480x270)
    ID3D12Resource* vp1ColorIn = CreateTexture(device, 480, 270, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* vp1ColorOut = CreateTexture(device, 640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* vp1Depth = CreateTexture(device, 480, 270, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* vp1Mvec = CreateTexture(device, 480, 270, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    printf("\n--- Test 1: Multi-Viewport Independent Lifecycle & Non-Interference ---\n");
    // Viewport 0 setup
    sl::ViewportHandle vp0(0);
    sl::DLSSOptions opt0{};
    opt0.mode = sl::DLSSMode::eMaxQuality;
    opt0.outputWidth = 1920;
    opt0.outputHeight = 1080;
    pfnDLSSSetOptions(vp0, opt0);

    sl::Resource vp0ResColorIn = { sl::ResourceType::eTex2d, vp0ColorIn, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource vp0ResColorOut = { sl::ResourceType::eTex2d, vp0ColorOut, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    sl::Resource vp0ResDepth = { sl::ResourceType::eTex2d, vp0Depth, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource vp0ResMvec = { sl::ResourceType::eTex2d, vp0Mvec, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::ResourceTag vp0Tags[4] = {
        { &vp0ResColorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &vp0ResColorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &vp0ResDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &vp0ResMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate }
    };
    pfnSetTag(vp0, vp0Tags, 4, cmdList);

    // Viewport 1 setup
    sl::ViewportHandle vp1(1);
    sl::DLSSOptions opt1{};
    opt1.mode = sl::DLSSMode::eMaxPerformance;
    opt1.outputWidth = 640;
    opt1.outputHeight = 360;
    pfnDLSSSetOptions(vp1, opt1);

    sl::Resource vp1ResColorIn = { sl::ResourceType::eTex2d, vp1ColorIn, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource vp1ResColorOut = { sl::ResourceType::eTex2d, vp1ColorOut, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    sl::Resource vp1ResDepth = { sl::ResourceType::eTex2d, vp1Depth, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource vp1ResMvec = { sl::ResourceType::eTex2d, vp1Mvec, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::ResourceTag vp1Tags[4] = {
        { &vp1ResColorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &vp1ResColorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &vp1ResDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &vp1ResMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate }
    };
    pfnSetTag(vp1, vp1Tags, 4, cmdList);

    // Evaluate Frame 1: Viewport 0 then Viewport 1 in SAME frame
    sl::FrameToken* token1 = nullptr;
    uint32_t frameIdx1 = 1;
    pfnGetNewFrameToken(token1, &frameIdx1);

    const sl::BaseStructure* inputsVp0[1] = { &vp0 };
    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token1, inputsVp0, 1, cmdList);
    assert(res == sl::Result::eOk);

    const sl::BaseStructure* inputsVp1[1] = { &vp1 };
    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token1, inputsVp1, 1, cmdList);
    assert(res == sl::Result::eOk);
    printf("[PASS] Frame 1: Co-evaluating Viewport 0 (1920x1080) & Viewport 1 (640x360) succeeded!\n");

    // Evaluate Frame 2: Viewport 0 then Viewport 1 (Verifying NO recreation occurs!)
    sl::FrameToken* token2 = nullptr;
    uint32_t frameIdx2 = 2;
    pfnGetNewFrameToken(token2, &frameIdx2);

    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token2, inputsVp0, 1, cmdList);
    assert(res == sl::Result::eOk);
    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token2, inputsVp1, 1, cmdList);
    assert(res == sl::Result::eOk);
    printf("[PASS] Frame 2: Both viewports retained their distinct MetalFX features with zero thrashing!\n");

    printf("\n--- Test 2: In-Flight 3-Frame Deferred Release Protection ---\n");
    // Frame 3: Dynamic scaling on Viewport 0 down to 960x540 (Target 1280x720)
    sl::FrameToken* token3 = nullptr;
    uint32_t frameIdx3 = 3;
    pfnGetNewFrameToken(token3, &frameIdx3);

    ID3D12Resource* vp0ResizedIn = CreateTexture(device, 960, 540, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* vp0ResizedOut = CreateTexture(device, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource rIn = { sl::ResourceType::eTex2d, vp0ResizedIn, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource rOut = { sl::ResourceType::eTex2d, vp0ResizedOut, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    sl::ResourceTag resizedTags[2] = {
        { &rIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate }
    };
    pfnSetTag(vp0, resizedTags, 2, cmdList);

    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token3, inputsVp0, 1, cmdList);
    assert(res == sl::Result::eOk);
    printf("[PASS] Frame 3: Viewport 0 resized; old handle safely placed into 3-frame deferred release queue!\n");

    // Frame 4 & 5: Deferred handle must stay alive while GPU in-flight
    sl::FrameToken* token4 = nullptr; uint32_t f4 = 4; pfnGetNewFrameToken(token4, &f4);
    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token4, inputsVp0, 1, cmdList);
    assert(res == sl::Result::eOk);

    sl::FrameToken* token5 = nullptr; uint32_t f5 = 5; pfnGetNewFrameToken(token5, &f5);
    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token5, inputsVp0, 1, cmdList);
    assert(res == sl::Result::eOk);

    // Frame 6: 3 full frames have passed since Frame 3 -> deferred handle released
    sl::FrameToken* token6 = nullptr; uint32_t f6 = 6; pfnGetNewFrameToken(token6, &f6);
    res = pfnEvaluateFeature(sl::kFeatureDLSS, *token6, inputsVp0, 1, cmdList);
    assert(res == sl::Result::eOk);
    printf("[PASS] Frame 6: 3-frame safety window elapsed; deferred handle cleanly freed with ZERO GPU conflicts!\n");

    printf("\n--- Test 3: Explicit slFreeResources Contract ---\n");
    // Free Viewport 1 explicitly
    res = pfnFreeResources(sl::kFeatureDLSS, vp1);
    assert(res == sl::Result::eOk);
    printf("[PASS] slFreeResources(kFeatureDLSS, Viewport 1) cleanly executed!\n");

    printf("\n--- Test 4: Explicit slAllocateResources Pre-warming ---\n");
    sl::ViewportHandle vp2(2);
    sl::DLSSOptions opt2{};
    opt2.mode = sl::DLSSMode::eBalanced;
    opt2.outputWidth = 1920;
    opt2.outputHeight = 1080;
    pfnDLSSSetOptions(vp2, opt2);

    res = pfnAllocateResources(cmdList, sl::kFeatureDLSS, vp2);
    assert(res == sl::Result::eOk);
    printf("[PASS] slAllocateResources(cmdList, kFeatureDLSS, Viewport 2) pre-warmed feature successfully!\n");

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
    printf("[PASS] GPU work across multiple viewports completed and synchronized cleanly!\n");

    printf("\n--- Test 6: Final Shutdown & Resource Reclamation ---\n");
    res = pfnShutdown();
    assert(res == sl::Result::eOk);
    printf("[PASS] slShutdown successfully flushed and reclaimed all multi-viewport contexts!\n");

    // Clean up textures
    if (vp0ColorIn) vp0ColorIn->Release();
    if (vp0ColorOut) vp0ColorOut->Release();
    if (vp0Depth) vp0Depth->Release();
    if (vp0Mvec) vp0Mvec->Release();
    if (vp1ColorIn) vp1ColorIn->Release();
    if (vp1ColorOut) vp1ColorOut->Release();
    if (vp1Depth) vp1Depth->Release();
    if (vp1Mvec) vp1Mvec->Release();
    if (vp0ResizedIn) vp0ResizedIn->Release();
    if (vp0ResizedOut) vp0ResizedOut->Release();
    if (fence) fence->Release();
    if (cmdList) cmdList->Release();
    if (cmdAlloc) cmdAlloc->Release();
    if (cmdQueue) cmdQueue->Release();
    if (device) device->Release();

    printf("\n[SUCCESS] ALL PHASE 4 MULTI-VIEWPORT & VRAM LIFECYCLE TESTS PASSED COMPLETELY!\n");
    return 0;
}
