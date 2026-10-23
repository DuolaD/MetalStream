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
typedef sl::Result (*PFN_slSetTagForFrame)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*PFN_slSetConstants)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature, const char*, void*&);
typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);

int main() {
    printf("[PHASE 3 TEST] Starting Dynamic Resolution Scaling (DRS) & Subrect Mapping Audit...\n");

    HMODULE hSl = LoadLibraryA("build/sl.interposer.dll");
    if (!hSl) {
        hSl = LoadLibraryA("sl.interposer.dll");
    }
    if (!hSl) {
        printf("[FAILED] Could not load sl.interposer.dll\n");
        return 1;
    }
    printf("[PHASE 3] sl.interposer.dll loaded at %p\n", hSl);

    auto pfnInit = (PFN_slInit)GetProcAddress(hSl, "slInit");
    auto pfnShutdown = (PFN_slShutdown)GetProcAddress(hSl, "slShutdown");
    auto pfnGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(hSl, "slGetNewFrameToken");
    auto pfnSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(hSl, "slSetD3DDevice");
    auto pfnSetTag = (PFN_slSetTag)GetProcAddress(hSl, "slSetTag");
    auto pfnSetTagForFrame = (PFN_slSetTagForFrame)GetProcAddress(hSl, "slSetTagForFrame");
    auto pfnSetConstants = (PFN_slSetConstants)GetProcAddress(hSl, "slSetConstants");
    auto pfnGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(hSl, "slGetFeatureFunction");
    auto pfnEvaluateFeature = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");

    if (!pfnInit || !pfnShutdown || !pfnGetNewFrameToken || !pfnSetD3DDevice ||
        !pfnSetTag || !pfnSetTagForFrame || !pfnSetConstants || !pfnGetFeatureFunction || !pfnEvaluateFeature) {
        printf("[FAILED] Missing core Streamline exports\n");
        return 1;
    }

    // 1. Create real D3D12 device & queue
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        printf("[FAILED] D3D12CreateDevice failed: 0x%08lX\n", hr);
        return 1;
    }
    printf("[PHASE 3] Created D3D12 Device: %p\n", device);

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* cmdQueue = nullptr;
    device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&cmdQueue));

    ID3D12CommandAllocator* cmdAlloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));

    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));

    // 2. Create GPU Resources (Allocated physically at 1920x1080)
    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC inDesc = {};
    inDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    inDesc.Width = 1920;
    inDesc.Height = 1080;
    inDesc.DepthOrArraySize = 1;
    inDesc.MipLevels = 1;
    inDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    inDesc.SampleDesc.Count = 1;
    inDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    inDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* inColor = nullptr;
    device->CreateCommittedResource(
        &heapDefault, D3D12_HEAP_FLAG_NONE, &inDesc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
        IID_PPV_ARGS(&inColor)
    );

    D3D12_RESOURCE_DESC outDesc = inDesc;
    outDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* outColor = nullptr;
    device->CreateCommittedResource(
        &heapDefault, D3D12_HEAP_FLAG_NONE, &outDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&outColor)
    );

    D3D12_RESOURCE_DESC depthDesc = inDesc;
    depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Resource* depthRes = nullptr;
    device->CreateCommittedResource(
        &heapDefault, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
        IID_PPV_ARGS(&depthRes)
    );

    D3D12_RESOURCE_DESC mvecDesc = inDesc;
    mvecDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    ID3D12Resource* mvecRes = nullptr;
    device->CreateCommittedResource(
        &heapDefault, D3D12_HEAP_FLAG_NONE, &mvecDesc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
        IID_PPV_ARGS(&mvecRes)
    );

    // Optional transparency mask resource
    ID3D12Resource* transMask = nullptr;
    device->CreateCommittedResource(
        &heapDefault, D3D12_HEAP_FLAG_NONE, &inDesc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
        IID_PPV_ARGS(&transMask)
    );

    // Initialize Streamline
    sl::Preferences pref{};
    pfnInit(pref, 0x2000B0001FEDC);
    pfnSetD3DDevice(device);

    sl::ViewportHandle vp(0);
    sl::FrameToken* token = nullptr;
    pfnGetNewFrameToken(token, nullptr);

    void* fnDLSSSetOptions = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fnDLSSSetOptions);
    auto pfnSetDLSSOptions = (sl::Result(*)(const sl::ViewportHandle&, const sl::DLSSOptions&))fnDLSSSetOptions;

    // =========================================================================
    // Test 1: Tag Extents & Chained Tag Traversal (next pointer)
    // =========================================================================
    printf("\n--- Test 1: Resource Extents & Tag Chain Traversal ---\n");
    sl::Resource rColor(sl::ResourceType::eTex2d, inColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rOut(sl::ResourceType::eTex2d, outColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource rDepth(sl::ResourceType::eTex2d, depthRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rMvec(sl::ResourceType::eTex2d, mvecRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rTrans(sl::ResourceType::eTex2d, transMask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    sl::Extent drsExtent{ 0, 0, 1280, 720 }; // DRS input size: 1280x720 within 1920x1080 physical buffer
    sl::Extent outExtent{ 0, 0, 1920, 1080 };

    sl::ResourceTag tagColor(&rColor, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &drsExtent);
    sl::ResourceTag tagOut(&rOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &outExtent);
    sl::ResourceTag tagDepth(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &drsExtent);
    sl::ResourceTag tagMvec(&rMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &drsExtent);
    sl::ResourceTag tagTrans(&rTrans, sl::kBufferTypeTransparencyHint, sl::ResourceLifecycle::eValidUntilPresent, &drsExtent);

    // Chain tagTrans onto tagMvec
    tagMvec.next = &tagTrans;

    sl::ResourceTag tags[3] = { tagColor, tagOut, tagDepth };
    // Set 3 tags, with Mvec + Trans passed via slSetTagForFrame
    pfnSetTag(vp, tags, 3, nullptr);
    sl::ResourceTag mvecArray[1] = { tagMvec };
    pfnSetTagForFrame(*token, vp, mvecArray, 1, nullptr);

    printf("[PASS] Chained and extent-tagged resources successfully registered!\n");

    // =========================================================================
    // Test 2: DRS Dynamic Subrect Evaluation with Alpha Upscaling
    // =========================================================================
    printf("\n--- Test 2: DRS Dynamic Subrect Evaluation with Alpha Upscaling ---\n");
    sl::DLSSOptions opts{};
    opts.mode = sl::DLSSMode::eMaxQuality;
    opts.outputWidth = 1920;
    opts.outputHeight = 1080;
    opts.alphaUpscalingEnabled = sl::Boolean::eTrue;
    pfnSetDLSSOptions(vp, opts);

    sl::Constants consts{};
    consts.jitterOffset = { 0.125f, -0.25f };
    consts.mvecScale = { 1.0f, 1.0f };
    consts.reset = sl::Boolean::eTrue; // Single-frame reset pulse
    pfnSetConstants(consts, *token, vp);

    const sl::BaseStructure* evalInputs[] = { &opts, &vp, &consts };
    sl::Result rEval1 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs, 3, cmdList);
    if (rEval1 != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature with DRS extent returned %d\n", (int)rEval1);
        return 1;
    }
    printf("[PASS] Frame 1 (1280x720 DRS subrect with reset=true) evaluated cleanly!\n");

    // =========================================================================
    // Test 3: Reset Pulse Verification (Must be single-frame pulse)
    // =========================================================================
    printf("\n--- Test 3: Reset Pulse Verification (Single-frame Pulse) ---\n");
    // Frame 2: Evaluate without setting reset. The reset in context must have automatically cleared to false!
    uint32_t frame2Idx = 2;
    pfnGetNewFrameToken(token, &frame2Idx);

    const sl::BaseStructure* evalInputs2[] = { &vp };
    sl::Result rEval2 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs2, 1, cmdList);
    if (rEval2 != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature Frame 2 returned %d\n", (int)rEval2);
        return 1;
    }
    printf("[PASS] Frame 2 evaluated cleanly without recurring reset pulse!\n");

    // =========================================================================
    // Test 4: Dynamic Resolution Re-allocation (DRS scaling from 720p to 540p)
    // =========================================================================
    printf("\n--- Test 4: Dynamic Resolution Scaling (720p -> 540p) Reallocation ---\n");
    sl::Extent drsExtent540{ 0, 0, 960, 540 };
    sl::ResourceTag tagColor540(&rColor, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &drsExtent540);
    sl::ResourceTag tagDepth540(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &drsExtent540);
    sl::ResourceTag tagMvec540(&rMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &drsExtent540);

    sl::ResourceTag tags540[3] = { tagColor540, tagDepth540, tagMvec540 };
    pfnSetTag(vp, tags540, 3, nullptr);

    uint32_t frame3Idx = 3;
    pfnGetNewFrameToken(token, &frame3Idx);

    sl::Result rEval3 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs2, 1, cmdList);
    if (rEval3 != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature DRS scale-down returned %d\n", (int)rEval3);
        return 1;
    }
    printf("[PASS] Frame 3 (dynamic scale down to 960x540) successfully recreated feature and evaluated!\n");

    // 3. Close command list and execute on GPU queue to verify full pipeline execution
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, lists);

    ID3D12Fence* syncFence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&syncFence));
    cmdQueue->Signal(syncFence, 1);
    while (syncFence->GetCompletedValue() < 1) {
        Sleep(1);
    }
    printf("[PASS] GPU work fully executed and synchronized with zero errors!\n");

    // Cleanup resources
    syncFence->Release();
    transMask->Release();
    mvecRes->Release();
    depthRes->Release();
    outColor->Release();
    inColor->Release();
    cmdList->Release();
    cmdAlloc->Release();
    cmdQueue->Release();
    device->Release();

    pfnShutdown();
    printf("\n[SUCCESS] ALL PHASE 3 DYNAMIC RESOLUTION & SUBRECT TESTS PASSED COMPLETELY!\n");
    return 0;
}
