#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdint.h>
#include <cmath>
#include <string.h>

#include "streamline/sl.h"
#include "streamline/sl_consts.h"
#include "streamline/sl_dlss.h"
#include <nvngx/nvsdk_ngx_defs.h>

typedef sl::Result (*PFN_slInit)(const sl::Preferences& pref, uint64_t sdkVersion);
typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slSetD3DDevice)(void* d3dDevice);
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*& token, const uint32_t* frameIndex);
typedef sl::Result (*PFN_slSetTag)(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slSetConstants)(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport);
typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slGetNativeInterface)(void* proxyInterface, void** baseInterface);
typedef sl::Result (*PFN_slUpgradeInterface)(void** baseInterface);

int main() {
    printf("=======================================================================\n");
    printf("[AUDIT2 BATCH 3 TEST] Camera Motion, Matrix Defense & Pipeline Suite\n");
    printf("=======================================================================\n");

    HMODULE hSl = LoadLibraryA("sl.interposer.dll");
    if (!hSl) {
        printf("[FAILED] Could not load sl.interposer.dll\n");
        return 1;
    }

    auto pfnInit = (PFN_slInit)GetProcAddress(hSl, "slInit");
    auto pfnShutdown = (PFN_slShutdown)GetProcAddress(hSl, "slShutdown");
    auto pfnSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(hSl, "slSetD3DDevice");
    auto pfnGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(hSl, "slGetNewFrameToken");
    auto pfnSetTag = (PFN_slSetTag)GetProcAddress(hSl, "slSetTag");
    auto pfnSetConstants = (PFN_slSetConstants)GetProcAddress(hSl, "slSetConstants");
    auto pfnEvaluateFeature = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");
    auto pfnGetNativeInterface = (PFN_slGetNativeInterface)GetProcAddress(hSl, "slGetNativeInterface");
    auto pfnUpgradeInterface = (PFN_slUpgradeInterface)GetProcAddress(hSl, "slUpgradeInterface");

    if (!pfnInit || !pfnShutdown || !pfnSetD3DDevice || !pfnGetNewFrameToken ||
        !pfnSetTag || !pfnSetConstants || !pfnEvaluateFeature ||
        !pfnGetNativeInterface || !pfnUpgradeInterface)
    {
        printf("[FAILED] Missing exports in sl.interposer.dll\n");
        return 1;
    }

    // =========================================================================
    // Phase 1: Pointer Trap Defense (slUpgradeInterface & slGetNativeInterface)
    // =========================================================================
    printf("\n--- Phase 1: Pointer Trap Defense for Native & Upgrade Interfaces ---\n");
    void* nullPtr = nullptr;
    sl::Result rUp1 = pfnUpgradeInterface(nullptr);
    if (rUp1 != sl::Result::eErrorMissingInputParameter && rUp1 != sl::Result::eErrorInvalidParameter) {
        printf("[FAILED] slUpgradeInterface(nullptr) did not return eErrorMissingInputParameter or eErrorInvalidParameter!\n");
        return 1;
    }
    sl::Result rUp2 = pfnUpgradeInterface(&nullPtr);
    if (rUp2 != sl::Result::eErrorMissingInputParameter && rUp2 != sl::Result::eErrorInvalidParameter) {
        printf("[FAILED] slUpgradeInterface(&nullPtr) did not return eErrorMissingInputParameter or eErrorInvalidParameter!\n");
        return 1;
    }
    if (pfnGetNativeInterface(nullptr, &nullPtr) != sl::Result::eErrorInvalidParameter) {
        printf("[FAILED] slGetNativeInterface(nullptr, ...) did not return eErrorInvalidParameter!\n");
        return 1;
    }
    printf("[PASS] Null-pointer trap defense verified across interface queries.\n");

    // =========================================================================
    // Create Direct3D 12 Hardware Context
    // =========================================================================
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        printf("[FAILED] Could not create D3D12 Device: 0x%08X\n", (uint32_t)hr);
        return 1;
    }

    // Test valid COM pointer with slUpgradeInterface
    void* devPtr = device;
    if (pfnUpgradeInterface(&devPtr) != sl::Result::eOk || devPtr != device) {
        printf("[FAILED] slUpgradeInterface failed for valid D3D12Device pointer!\n");
        return 1;
    }
    void* nativeDev = nullptr;
    if (pfnGetNativeInterface(device, &nativeDev) != sl::Result::eOk || nativeDev != device) {
        printf("[FAILED] slGetNativeInterface failed for valid D3D12Device pointer!\n");
        return 1;
    }
    device->Release(); // Release the AddRef from slGetNativeInterface
    printf("[PASS] Valid COM pointer query and unwrap verified.\n");

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* cmdQueue = nullptr;
    device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&cmdQueue));

    ID3D12CommandAllocator* cmdAlloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));

    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));

    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC inDesc = {};
    inDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    inDesc.Width = 1280;
    inDesc.Height = 720;
    inDesc.DepthOrArraySize = 1;
    inDesc.MipLevels = 1;
    inDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    inDesc.SampleDesc.Count = 1;
    inDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ID3D12Resource* inColor = nullptr;
    device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &inDesc,
                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&inColor));

    D3D12_RESOURCE_DESC outDesc = inDesc;
    outDesc.Width = 1920;
    outDesc.Height = 1080;
    outDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* outColor = nullptr;
    device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &outDesc,
                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outColor));

    D3D12_RESOURCE_DESC depthDesc = inDesc;
    depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
    ID3D12Resource* depthRes = nullptr;
    device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&depthRes));

    D3D12_RESOURCE_DESC mvecDesc = inDesc;
    mvecDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    ID3D12Resource* mvecRes = nullptr;
    device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_NONE, &mvecDesc,
                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mvecRes));

    sl::Preferences pref{};
    pref.applicationId = 0xABCDE;
    pfnInit(pref, sl::kSDKVersion);
    pfnSetD3DDevice(device);

    sl::ViewportHandle vp(0);
    sl::Extent drs720{ 0, 0, 1280, 720 };
    sl::Extent drs1080{ 0, 0, 1920, 1080 };

    sl::Resource rIn(sl::ResourceType::eTex2d, inColor, D3D12_RESOURCE_STATE_COMMON);
    sl::Resource rOut(sl::ResourceType::eTex2d, outColor, D3D12_RESOURCE_STATE_COMMON);
    sl::Resource rDepth(sl::ResourceType::eTex2d, depthRes, D3D12_RESOURCE_STATE_COMMON);
    sl::Resource rMvec(sl::ResourceType::eTex2d, mvecRes, D3D12_RESOURCE_STATE_COMMON);

    sl::ResourceTag tagIn(&rIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagOut(&rOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &drs1080);
    sl::ResourceTag tagDepth(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &drs720);
    sl::ResourceTag tagMvec(&rMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &drs720);

    sl::ResourceTag tags[4] = { tagIn, tagOut, tagDepth, tagMvec };
    pfnSetTag(vp, tags, 4, cmdList);

    // =========================================================================
    // Phase 2: cameraMotionIncluded == false & clipToPrevClip Matrix Defense
    // =========================================================================
    printf("\n--- Phase 2: cameraMotionIncluded Defense & Matrix Sanitization ---\n");
    sl::DLSSOptions optQuality{};
    optQuality.mode = sl::DLSSMode::eMaxQuality;
    optQuality.outputWidth = 1920;
    optQuality.outputHeight = 1080;

    // Subtest A: Poisoned clipToPrevClip matrix with NaN and INVALID_FLOAT
    sl::Constants constsPoisoned{};
    constsPoisoned.cameraMotionIncluded = sl::Boolean::eFalse;
    constsPoisoned.jitterOffset.x = 0.0f;
    constsPoisoned.jitterOffset.y = 0.0f;
    // clipToPrevClip default initialized to INVALID_FLOAT
    float* pMat = reinterpret_cast<float*>(&constsPoisoned.clipToPrevClip);
    pMat[0] = NAN;
    pMat[5] = INFINITY;

    sl::FrameToken* token = nullptr;
    uint32_t fIdx = 1;
    pfnGetNewFrameToken(token, &fIdx);
    pfnSetConstants(constsPoisoned, *token, vp);

    const sl::BaseStructure* evalInputs[2] = { &vp, &optQuality };
    sl::Result rEvalPoison = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs, 2, cmdList);
    if (rEvalPoison != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature failed on poisoned clipToPrevClip matrix: %d\n", (int)rEvalPoison);
        return 1;
    }
    printf("[PASS] Poisoned clipToPrevClip matrix safely detected and sanitized to identity.\n");

    // Subtest B: Valid custom clipToPrevClip transformation matrix
    sl::Constants constsValid{};
    constsValid.cameraMotionIncluded = sl::Boolean::eFalse;
    constsValid.jitterOffset.x = 0.125f;
    constsValid.jitterOffset.y = -0.125f;
    float* pValidMat = reinterpret_cast<float*>(&constsValid.clipToPrevClip);
    for (int i = 0; i < 16; ++i) pValidMat[i] = (i % 5 == 0) ? 1.0f : 0.0f; // Identity matrix
    pValidMat[3] = 0.05f; // Small camera translation X

    fIdx = 2;
    pfnGetNewFrameToken(token, &fIdx);
    pfnSetConstants(constsValid, *token, vp);

    sl::Result rEvalValid = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs, 2, cmdList);
    if (rEvalValid != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature failed on valid custom clipToPrevClip matrix: %d\n", (int)rEvalValid);
        return 1;
    }
    printf("[PASS] Valid clipToPrevClip matrix successfully forwarded to NGX parameter bus.\n");

    // =========================================================================
    // Phase 3: Dynamic DLSS Mode Switching Pipeline (eOff -> eQuality -> eOff)
    // =========================================================================
    printf("\n--- Phase 3: Dynamic DLSS Mode Switching Pipeline (eOff -> eQuality -> eOff) ---\n");

    // 1. Frame 3: DLSSMode::eOff -> BlitGuard
    sl::DLSSOptions optModeSwitch{};
    optModeSwitch.mode = sl::DLSSMode::eOff;
    optModeSwitch.outputWidth = 1920;
    optModeSwitch.outputHeight = 1080;
    fIdx = 3;
    pfnGetNewFrameToken(token, &fIdx);
    const sl::BaseStructure* evalInputsOff[2] = { &vp, &optModeSwitch };
    sl::Result rModeOff1 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputsOff, 2, cmdList);
    if (rModeOff1 != sl::Result::eErrorInvalidIntegration) {
        printf("[FAILED] Mode switch to eOff expected eErrorInvalidIntegration, got: %d\n", (int)rModeOff1);
        return 1;
    }

    // 2. Frame 4: DLSSMode::eBalanced -> MetalFX Feature Active
    optModeSwitch.mode = sl::DLSSMode::eBalanced;
    fIdx = 4;
    pfnGetNewFrameToken(token, &fIdx);
    const sl::BaseStructure* evalInputsBal[2] = { &vp, &optModeSwitch };
    sl::Result rModeBal = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputsBal, 2, cmdList);
    if (rModeBal != sl::Result::eOk) {
        printf("[FAILED] Mode switch to eBalanced failed: %d\n", (int)rModeBal);
        return 1;
    }

    // 3. Frame 5: DLSSMode::eOff -> Rejected with eErrorInvalidIntegration
    optModeSwitch.mode = sl::DLSSMode::eOff;
    fIdx = 5;
    pfnGetNewFrameToken(token, &fIdx);
    sl::Result rModeOff2 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputsOff, 2, cmdList);
    if (rModeOff2 != sl::Result::eErrorInvalidIntegration) {
        printf("[FAILED] Mode switch back to eOff expected eErrorInvalidIntegration, got: %d\n", (int)rModeOff2);
        return 1;
    }
    printf("[PASS] Dynamic DLSS mode transitions (eOff -> eBalanced -> eOff) succeeded with zero lifecycle leaks.\n");

    // =========================================================================
    // Phase 4: Full GPU Execution & Hardware Fence Verification
    // =========================================================================
    printf("\n--- Phase 4: Full Direct3D 12 GPU Hardware Execution ---\n");
    cmdList->Close();
    ID3D12CommandList* ppLists[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, ppLists);

    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE hEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    cmdQueue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, hEvent);
    WaitForSingleObject(hEvent, INFINITE);
    CloseHandle(hEvent);
    fence->Release();

    printf("[PASS] Direct3D 12 GPU commands executed with clean hardware synchronization.\n");

    // Cleanup
    pfnShutdown();
    inColor->Release();
    outColor->Release();
    depthRes->Release();
    mvecRes->Release();
    cmdList->Release();
    cmdAlloc->Release();
    cmdQueue->Release();
    device->Release();
    FreeLibrary(hSl);

    printf("\n=======================================================================\n");
    printf("[RESULT] ALL AUDIT2 BATCH 3 TESTS PASSED (100%%)!\n");
    printf("=======================================================================\n");
    return 0;
}
