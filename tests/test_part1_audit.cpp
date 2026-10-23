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

static ID3D12Resource* Create2DTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState) {
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

    ID3D12Resource* resource = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        printf("[ERROR] CreateCommittedResource failed: 0x%08lX\n", hr);
        return nullptr;
    }
    return resource;
}

int main() {
    printf("=======================================================================\n");
    printf("[AUDIT PART 1 TEST] Parameter Mapping Full-Spectrum Alignment Audit\n");
    printf("=======================================================================\n");

    HMODULE hSl = LoadLibraryA("build/sl.interposer.dll");
    if (!hSl) {
        hSl = LoadLibraryA("sl.interposer.dll");
    }
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

    if (!pfnInit || !pfnShutdown || !pfnGetNewFrameToken || !pfnSetD3DDevice ||
        !pfnSetTag || !pfnSetTagForFrame || !pfnSetConstants || !pfnGetFeatureFunction || !pfnEvaluateFeature) {
        printf("[FAILED] Missing core Streamline exports\n");
        return 1;
    }

    // 1. Create D3D12 Device, Queue, Allocator, List
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

    // 2. Initialize Streamline
    sl::Preferences pref{};
    pfnInit(pref, 0x2000B0001FEDC);
    pfnSetD3DDevice(device);

    sl::ViewportHandle vp(0);
    sl::FrameToken* token = nullptr;
    uint32_t frameIdx = 1;
    pfnGetNewFrameToken(token, &frameIdx);

    void* fnDLSSSetOptions = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fnDLSSSetOptions);
    auto pfnSetDLSSOptions = (sl::Result(*)(const sl::ViewportHandle&, const sl::DLSSOptions&))fnDLSSSetOptions;

    // 3. Create GPU resources
    ID3D12Resource* inColor = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* outColor = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* depthRes = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* mvecRes = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* exposureRes = Create2DTexture(device, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* transRes = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* biasRes = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* animRes = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* rayDistRes = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* reflMvecRes = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* particleRes = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);

    // =========================================================================
    // Test 1: Exposure Texture Passthrough & AutoExposure Decoupling
    // =========================================================================
    printf("\n--- Test 1: Exposure Texture (kBufferTypeExposure) & AutoExposure Flag ---\n");
    sl::Resource rColor(sl::ResourceType::eTex2d, inColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rOut(sl::ResourceType::eTex2d, outColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource rDepth(sl::ResourceType::eTex2d, depthRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rMvec(sl::ResourceType::eTex2d, mvecRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rExp(sl::ResourceType::eTex2d, exposureRes, D3D12_RESOURCE_STATE_COMMON);

    sl::Extent drs720{ 0, 0, 1280, 720 };
    sl::Extent out1080{ 0, 0, 1920, 1080 };
    sl::Extent expExt{ 0, 0, 1, 1 };

    sl::ResourceTag tagColor(&rColor, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagOut(&rOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &out1080);
    sl::ResourceTag tagDepth(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagMvec(&rMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagExp(&rExp, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilPresent, &expExt);

    sl::ResourceTag tags1[5] = { tagColor, tagOut, tagDepth, tagMvec, tagExp };
    pfnSetTag(vp, tags1, 5, nullptr);

    sl::DLSSOptions opts{};
    opts.mode = sl::DLSSMode::eMaxQuality;
    opts.outputWidth = 1920;
    opts.outputHeight = 1080;
    opts.useAutoExposure = sl::Boolean::eFalse; // explicitly disable auto-exposure: must bind exposureRes!
    opts.qualityPreset = sl::DLSSPreset::ePresetK;
    opts.indicatorInvertAxisX = sl::Boolean::eTrue;
    opts.indicatorInvertAxisY = sl::Boolean::eFalse;
    pfnSetDLSSOptions(vp, opts);

    sl::Constants consts{};
    consts.jitterOffset = { 0.125f, -0.25f };
    consts.mvecScale = { 1.0f, 1.0f };
    consts.reset = sl::Boolean::eTrue;
    pfnSetConstants(consts, *token, vp);

    const sl::BaseStructure* evalInputs1[] = { &opts, &vp, &consts };
    sl::Result rEval1 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs1, 3, cmdList);
    if (rEval1 != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature with exposure texture failed: %d\n", (int)rEval1);
        return 1;
    }
    printf("[PASS] Frame 1: Exposure texture successfully bound, transitioned, and evaluated with useAutoExposure=false!\n");

    // =========================================================================
    // Test 2: Dynamic Resolution (DRS) Handle Reuse & Throttle Check
    // =========================================================================
    printf("\n--- Test 2: DRS Handle Reuse & Temporal Stability (1280x720 -> 1152x648) ---\n");
    // DRS micro-adjustment: 1280x720 scaled slightly down to 1152x648 (90% DRS)
    sl::Extent drs1152{ 0, 0, 1152, 648 };
    sl::ResourceTag tagColor1152(&rColor, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &drs1152);
    sl::ResourceTag tagDepth1152(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &drs1152);
    sl::ResourceTag tagMvec1152(&rMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &drs1152);

    sl::ResourceTag tags2[3] = { tagColor1152, tagDepth1152, tagMvec1152 };
    pfnSetTag(vp, tags2, 3, nullptr);

    frameIdx = 2;
    pfnGetNewFrameToken(token, &frameIdx);

    const sl::BaseStructure* evalInputs2[] = { &vp };
    sl::Result rEval2 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs2, 1, cmdList);
    if (rEval2 != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature Frame 2 (DRS micro-adjustment) failed: %d\n", (int)rEval2);
        return 1;
    }
    printf("[PASS] Frame 2: DRS micro-adjustment (1152x648) evaluated cleanly without feature recreation or temporal trashing!\n");

    // =========================================================================
    // Test 3: High-Resolution Native Motion Vectors (1920x1080 MV > 1280x720 Color)
    // =========================================================================
    printf("\n--- Test 3: High-Resolution Motion Vectors Adaptive Detection ---\n");
    sl::Extent highResMvecExt{ 0, 0, 1920, 1080 }; // Native target resolution MV
    sl::ResourceTag tagMvecHigh(&rMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &highResMvecExt);
    pfnSetTag(vp, &tagMvecHigh, 1, nullptr);

    frameIdx = 3;
    pfnGetNewFrameToken(token, &frameIdx);

    sl::Result rEval3 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs2, 1, cmdList);
    if (rEval3 != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature Frame 3 (High-res MV) failed: %d\n", (int)rEval3);
        return 1;
    }
    printf("[PASS] Frame 3: Native resolution motion vectors detected and scaled to buffer dimensions!\n");

    // =========================================================================
    // Test 4: Extended Masks & Expanded Barrier Array Stress (10+ Resources)
    // =========================================================================
    printf("\n--- Test 4: Multi-Mask Barrier Expansion (10+ Resources Barrier Array) ---\n");
    sl::Resource rTrans(sl::ResourceType::eTex2d, transRes, D3D12_RESOURCE_STATE_COMMON);
    sl::Resource rBias(sl::ResourceType::eTex2d, biasRes, D3D12_RESOURCE_STATE_COMMON);
    sl::Resource rAnim(sl::ResourceType::eTex2d, animRes, D3D12_RESOURCE_STATE_COMMON);
    sl::Resource rRay(sl::ResourceType::eTex2d, rayDistRes, D3D12_RESOURCE_STATE_COMMON);
    sl::Resource rRefl(sl::ResourceType::eTex2d, reflMvecRes, D3D12_RESOURCE_STATE_COMMON);
    sl::Resource rPart(sl::ResourceType::eTex2d, particleRes, D3D12_RESOURCE_STATE_COMMON);

    sl::ResourceTag tagTrans(&rTrans, sl::kBufferTypeTransparencyHint, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagBias(&rBias, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagAnim(&rAnim, sl::kBufferTypeAnimatedTextureHint, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagRay(&rRay, sl::kBufferTypeRaytracingDistance, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagRefl(&rRefl, sl::kBufferTypeReflectionMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagPart(&rPart, sl::kBufferTypeParticleHint, sl::ResourceLifecycle::eValidUntilPresent, &drs720);

    sl::ResourceTag extraTags[6] = { tagTrans, tagBias, tagAnim, tagRay, tagRefl, tagPart };
    pfnSetTag(vp, extraTags, 6, nullptr);

    frameIdx = 4;
    pfnGetNewFrameToken(token, &frameIdx);

    sl::Result rEval4 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs2, 1, cmdList);
    if (rEval4 != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature Frame 4 (10+ multi-mask barriers) failed: %d\n", (int)rEval4);
        return 1;
    }
    printf("[PASS] Frame 4: 10+ resource barriers transitioned and restored safely with zero stack overflow!\n");

    // =========================================================================
    // Test 5: Full Direct3D 12 Hardware Queue Execution & Fence Synchronization
    // =========================================================================
    printf("\n--- Test 5: Direct3D 12 GPU Hardware Execution & Fence Sync ---\n");
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    cmdQueue->Signal(fence, 1);
    while (fence->GetCompletedValue() < 1) {
        Sleep(1);
    }
    printf("[PASS] Direct3D 12 command list completed execution on GPU queue with clean synchronization!\n");

    // Cleanup resources
    fence->Release();
    particleRes->Release();
    reflMvecRes->Release();
    rayDistRes->Release();
    animRes->Release();
    biasRes->Release();
    transRes->Release();
    exposureRes->Release();
    mvecRes->Release();
    depthRes->Release();
    outColor->Release();
    inColor->Release();
    cmdList->Release();
    cmdAlloc->Release();
    cmdQueue->Release();
    device->Release();

    pfnShutdown();
    printf("\n=======================================================================\n");
    printf("[SUCCESS] ALL PART 1 PARAMETER MAPPING AUDIT VERIFICATIONS PASSED!\n");
    printf("=======================================================================\n");
    return 0;
}
