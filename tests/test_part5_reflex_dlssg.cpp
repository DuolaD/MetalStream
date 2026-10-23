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
#include <streamline/sl_reflex.h>
#include <streamline/sl_pcl.h>

typedef sl::Result (*PFN_slInit)(const sl::Preferences&, uint64_t);
typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*&, const uint32_t*);
typedef sl::Result (*PFN_slSetD3DDevice)(void*);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature, const char*, void*&);
typedef sl::Result (*PFN_slSetData)(const sl::BaseStructure*, sl::CommandBuffer*);
typedef sl::Result (*PFN_slGetData)(const sl::BaseStructure*, sl::BaseStructure*, sl::CommandBuffer*);

typedef sl::Result (*PFN_slDLSSGGetState)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);
typedef sl::Result (*PFN_slDLSSGSetOptions)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
typedef sl::Result (*PFN_slReflexGetState)(sl::ReflexState&);
typedef sl::Result (*PFN_slReflexSetOptions)(const sl::ReflexOptions&);
typedef sl::Result (*PFN_slReflexSleep)(const sl::FrameToken&);
typedef sl::Result (*PFN_slReflexSetMarker)(sl::PCLMarker, const sl::FrameToken&);
typedef sl::Result (*PFN_slReflexSetCameraData)(const sl::ViewportHandle&, const sl::FrameToken&, const sl::ReflexCameraData&);
typedef sl::Result (*PFN_slReflexGetPredictedCameraData)(const sl::ViewportHandle&, const sl::FrameToken&, sl::ReflexPredictedCameraData&);
typedef sl::Result (*PFN_slPCLGetState)(sl::PCLState&);
typedef sl::Result (*PFN_slPCLSetOptions)(const sl::PCLOptions&);
typedef sl::Result (*PFN_slPCLSetMarker)(sl::PCLMarker, const sl::FrameToken&);

static bool IsIdentityMatrix(const sl::float4x4& m) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float expected = (r == c) ? 1.0f : 0.0f;
            float val = 0.0f;
            if (c == 0) val = m[r].x;
            else if (c == 1) val = m[r].y;
            else if (c == 2) val = m[r].z;
            else if (c == 3) val = m[r].w;
            if (val != expected) return false;
        }
    }
    return true;
}

int main() {
    printf("=======================================================================\n");
    printf("[AUDIT PART 5 TEST] Reflex, PCL & DLSS-G Anti-Explosion Safety Audit\n");
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
    auto pfnGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(hSl, "slGetFeatureFunction");
    auto pfnSetData = (PFN_slSetData)GetProcAddress(hSl, "slSetData");
    auto pfnGetData = (PFN_slGetData)GetProcAddress(hSl, "slGetData");

    if (!pfnInit || !pfnShutdown || !pfnGetNewFrameToken || !pfnSetD3DDevice ||
        !pfnGetFeatureFunction || !pfnSetData || !pfnGetData) {
        printf("[FAILED] Missing core Streamline exports\n");
        return 1;
    }

    sl::Preferences pref{};
    pref.applicationId = 12345;
    sl::Result rInit = pfnInit(pref, 0x2000B0001FEDC);
    assert(rInit == sl::Result::eOk);

    // Retrieve feature functions via slGetFeatureFunction
    void* fnDLSSGGetState = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fnDLSSGGetState);
    auto pfnDLSSGGetState = (PFN_slDLSSGGetState)fnDLSSGGetState;
    assert(pfnDLSSGGetState != nullptr);

    void* fnDLSSGSetOptions = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fnDLSSGSetOptions);
    auto pfnDLSSGSetOptions = (PFN_slDLSSGSetOptions)fnDLSSGSetOptions;
    assert(pfnDLSSGSetOptions != nullptr);

    void* fnReflexGetState = nullptr;
    pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", fnReflexGetState);
    auto pfnReflexGetState = (PFN_slReflexGetState)fnReflexGetState;
    assert(pfnReflexGetState != nullptr);

    void* fnReflexSetOptions = nullptr;
    pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fnReflexSetOptions);
    auto pfnReflexSetOptions = (PFN_slReflexSetOptions)fnReflexSetOptions;
    assert(pfnReflexSetOptions != nullptr);

    void* fnReflexSleep = nullptr;
    pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fnReflexSleep);
    auto pfnReflexSleep = (PFN_slReflexSleep)fnReflexSleep;
    assert(pfnReflexSleep != nullptr);

    void* fnReflexSetMarker = nullptr;
    pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexSetMarker", fnReflexSetMarker);
    auto pfnReflexSetMarker = (PFN_slReflexSetMarker)fnReflexSetMarker;
    assert(pfnReflexSetMarker != nullptr);

    void* fnReflexSetCameraData = nullptr;
    pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexSetCameraData", fnReflexSetCameraData);
    auto pfnReflexSetCameraData = (PFN_slReflexSetCameraData)fnReflexSetCameraData;
    assert(pfnReflexSetCameraData != nullptr);

    void* fnReflexGetPredictedCameraData = nullptr;
    pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexGetPredictedCameraData", fnReflexGetPredictedCameraData);
    auto pfnReflexGetPredictedCameraData = (PFN_slReflexGetPredictedCameraData)fnReflexGetPredictedCameraData;
    assert(pfnReflexGetPredictedCameraData != nullptr);

    void* fnPCLGetState = nullptr;
    pfnGetFeatureFunction(sl::kFeaturePCL, "slPCLGetState", fnPCLGetState);
    auto pfnPCLGetState = (PFN_slPCLGetState)fnPCLGetState;
    assert(pfnPCLGetState != nullptr);

    void* fnPCLSetOptions = nullptr;
    pfnGetFeatureFunction(sl::kFeaturePCL, "slPCLSetOptions", fnPCLSetOptions);
    auto pfnPCLSetOptions = (PFN_slPCLSetOptions)fnPCLSetOptions;
    assert(pfnPCLSetOptions != nullptr);

    void* fnPCLSetMarker = nullptr;
    pfnGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fnPCLSetMarker);
    auto pfnPCLSetMarker = (PFN_slPCLSetMarker)fnPCLSetMarker;
    assert(pfnPCLSetMarker != nullptr);

    // Initialize D3D12 Device & Command Queue
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

    pfnSetD3DDevice(device);

    sl::FrameToken* token = nullptr;
    pfnGetNewFrameToken(token, nullptr);
    assert(token != nullptr);

    // =========================================================================
    // Test 1: PCL & Reflex State Zeroing & Message ID Leak Elimination
    // =========================================================================
    printf("\n--- Test 1: PCL & Reflex State Zeroing & Dirty Stack Defense ---\n");

    // Poison stack with 0xFE
    sl::PCLState pclState;
    memset(&pclState, 0xFE, sizeof(pclState));
    sl::Result rPCL = pfnPCLGetState(pclState);
    assert(rPCL == sl::Result::eOk);
    assert(pclState.statsWindowMessage == 0);
    printf("[PASS] slPCLGetState wiped poisoned stack (statsWindowMessage == 0).\n");

    // Poison stack with 0xAA
    sl::ReflexState reflexState;
    memset(&reflexState, 0xAA, sizeof(reflexState));
    sl::Result rReflex = pfnReflexGetState(reflexState);
    assert(rReflex == sl::Result::eOk);
    assert(reflexState.lowLatencyAvailable == true);
    assert(reflexState.latencyReportAvailable == false);
    assert(reflexState.flashIndicatorDriverControlled == false);
    assert(reflexState.statsWindowMessage == 0);

    for (int i = 0; i < sl::kReflexFrameReportCount; ++i) {
        assert(reflexState.frameReport[i].frameID == 0);
        assert(reflexState.frameReport[i].gpuActiveRenderTimeUs == 0);
        assert(reflexState.frameReport2[i].cameraConstructedTime == 0);
    }
    printf("[PASS] slReflexGetState completely zeroed latency arrays and eliminated message leak.\n");

    // Test Reflex Sleep and Marker no-op safety
    sl::Result rSleep = pfnReflexSleep(*token);
    assert(rSleep == sl::Result::eOk);
    sl::Result rMarker = pfnReflexSetMarker(sl::PCLMarker::eSimulationStart, *token);
    assert(rMarker == sl::Result::eOk);
    printf("[PASS] slReflexSleep and slReflexSetMarker executed cleanly as non-blocking no-ops.\n");

    // =========================================================================
    // Test 2: Reflex Camera Data & Anti-Assert Degeneracy Defense
    // =========================================================================
    printf("\n--- Test 2: Reflex Camera Data Anti-Degeneracy & Assertion Defense ---\n");

    sl::ViewportHandle vp0(0);
    sl::ReflexPredictedCameraData predCam{};
    memset(&predCam, 0xCC, sizeof(predCam));

    // Case A: No camera data set yet -> Must return Identity Matrix
    sl::Result rPredEmpty = pfnReflexGetPredictedCameraData(vp0, *token, predCam);
    assert(rPredEmpty == sl::Result::eOk);
    assert(IsIdentityMatrix(predCam.predictedWorldToViewMatrix));
    assert(IsIdentityMatrix(predCam.predictedViewToClipMatrix));
    printf("[PASS] Empty viewport camera data safely yielded Identity Matrix.\n");

    // Case B: Camera data set with all-zero matrix -> Must fallback to Identity Matrix to prevent singular asserts
    sl::ReflexCameraData zeroCam{};
    memset(&zeroCam, 0, sizeof(zeroCam));
    pfnReflexSetCameraData(vp0, *token, zeroCam);

    memset(&predCam, 0xCC, sizeof(predCam));
    sl::Result rPredZero = pfnReflexGetPredictedCameraData(vp0, *token, predCam);
    assert(rPredZero == sl::Result::eOk);
    assert(IsIdentityMatrix(predCam.predictedWorldToViewMatrix));
    assert(IsIdentityMatrix(predCam.predictedViewToClipMatrix));
    printf("[PASS] All-zero degenerate camera matrix successfully detected and substituted with Identity Matrix!\n");

    // Case C: Valid custom camera matrix
    sl::ReflexCameraData validCam{};
    validCam.worldToViewMatrix[0] = sl::float4(2.0f, 0.0f, 0.0f, 0.0f);
    validCam.worldToViewMatrix[1] = sl::float4(0.0f, 2.0f, 0.0f, 0.0f);
    validCam.worldToViewMatrix[2] = sl::float4(0.0f, 0.0f, 2.0f, 0.0f);
    validCam.worldToViewMatrix[3] = sl::float4(0.0f, 0.0f, 0.0f, 1.0f);
    validCam.viewToClipMatrix = validCam.worldToViewMatrix;
    pfnReflexSetCameraData(vp0, *token, validCam);

    memset(&predCam, 0xCC, sizeof(predCam));
    sl::Result rPredValid = pfnReflexGetPredictedCameraData(vp0, *token, predCam);
    assert(rPredValid == sl::Result::eOk);
    assert(predCam.predictedWorldToViewMatrix[0].x == 2.0f);
    assert(predCam.predictedViewToClipMatrix[1].y == 2.0f);
    printf("[PASS] Valid custom camera matrix faithfully retrieved.\n");

    // =========================================================================
    // Test 3: DLSS-G Frame Generation State & D3D12 Dummy Fence Queue Wait
    // =========================================================================
    printf("\n--- Test 3: DLSS-G State & D3D12 Hardware Dummy Fence Validation ---\n");

    sl::DLSSGState gState;
    memset(&gState, 0xCD, sizeof(gState));

    sl::DLSSGOptions gOpts{};
    gOpts.mode = sl::DLSSGMode::eOn;
    gOpts.numFramesToGenerate = 1;

    sl::Result rGState = pfnDLSSGGetState(vp0, gState, &gOpts);
    assert(rGState == sl::Result::eOk);
    assert(gState.status == sl::DLSSGStatus::eOk);
    assert(gState.minWidthOrHeight == 64);
    assert(gState.numFramesActuallyPresented == 1);
    assert(gState.numFramesToGenerateMax == 1);
    assert(gState.estimatedVRAMUsageInBytes == 120 * 1024 * 1024);
    assert(gState.bIsVsyncSupportAvailable == sl::Boolean::eTrue);
    assert(gState.bIsDynamicMFGSupported == sl::Boolean::eFalse);
    assert(gState.inputsProcessingCompletionFence != nullptr);
    printf("[PASS] DLSSGState correctly populated with status=eOk and valid dummy fence %p\n",
           gState.inputsProcessingCompletionFence);

    // Hardware GPU wait verification: queue wait on dummy fence must execute cleanly without driver faults
    ID3D12Fence* fencePtr = static_cast<ID3D12Fence*>(gState.inputsProcessingCompletionFence);
    hr = cmdQueue->Wait(fencePtr, gState.lastPresentInputsProcessingCompletionFenceValue);
    assert(SUCCEEDED(hr));
    printf("[PASS] D3D12CommandQueue::Wait on inputsProcessingCompletionFence succeeded with 0x0!\n");

    // =========================================================================
    // Test 4: Unified Bus (slSetData / slGetData) Full Feature Interoperability
    // =========================================================================
    printf("\n--- Test 4: Unified Bus (slSetData & slGetData) Complete Dispatch ---\n");

    // 1. Pass ReflexOptions and DLSSGOptions via slSetData
    sl::ReflexOptions setReflexOpt{};
    setReflexOpt.mode = sl::ReflexMode::eLowLatencyWithBoost;
    setReflexOpt.frameLimitUs = 16666;

    sl::DLSSGOptions setDLSSGOpt{};
    setDLSSGOpt.mode = sl::DLSSGMode::eAuto;
    setDLSSGOpt.numFramesToGenerate = 1;

    setReflexOpt.next = &setDLSSGOpt;
    sl::Result rSetBus = pfnSetData(&setReflexOpt, nullptr);
    assert(rSetBus == sl::Result::eOk);
    printf("[PASS] slSetData accepted chained ReflexOptions & DLSSGOptions.\n");

    // 2. Query ReflexState, DLSSGState and PCLState via slGetData
    sl::ReflexState busReflexState{};
    sl::DLSSGState busDLSSGState{};
    sl::PCLState busPCLState{};

    busReflexState.next = &busDLSSGState;
    busDLSSGState.next = &busPCLState;

    sl::Result rGetBus = pfnGetData(nullptr, &busReflexState, nullptr);
    assert(rGetBus == sl::Result::eOk);
    assert(busReflexState.lowLatencyAvailable == true);
    assert(busDLSSGState.status == sl::DLSSGStatus::eOk);
    assert(busDLSSGState.inputsProcessingCompletionFence != nullptr);
    assert(busPCLState.statsWindowMessage == 0);
    printf("[PASS] slGetData populated chained ReflexState, DLSSGState & PCLState via Unified Bus!\n");

    // =========================================================================
    // Test 5: D3D12 Device Re-Capture & Clean Shutdown
    // =========================================================================
    printf("\n--- Test 5: Device Re-Capture Lifecycle & Idempotent Shutdown ---\n");

    // Create secondary D3D12 device & queue
    ID3D12Device* device2 = nullptr;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device2));
    assert(SUCCEEDED(hr) && device2 != nullptr);

    ID3D12CommandQueue* cmdQueue2 = nullptr;
    device2->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&cmdQueue2));

    // Call slSetD3DDevice with new device -> Old fence released, new fence created for device2
    pfnSetD3DDevice(device2);

    sl::DLSSGState gState2{};
    pfnDLSSGGetState(vp0, gState2, nullptr);
    assert(gState2.inputsProcessingCompletionFence != nullptr);

    // Verify the new fence works on device2's command queue
    hr = cmdQueue2->Wait(static_cast<ID3D12Fence*>(gState2.inputsProcessingCompletionFence), gState2.lastPresentInputsProcessingCompletionFenceValue);
    assert(SUCCEEDED(hr));
    printf("[PASS] slSetD3DDevice recreated fence for new device %p and executed queue wait with 0x0!\n", device2);

    cmdQueue2->Release();

    // Shutdown
    sl::Result rShut = pfnShutdown();
    assert(rShut == sl::Result::eOk);
    printf("[PASS] Final slShutdown released all fences and context resources.\n");

    // DX cleanup
    cmdQueue->Release();
    device->Release();
    device2->Release();

    printf("\n=======================================================================\n");
    printf("[AUDIT PART 5 RESULT] ALL REFLEX & DLSS-G SAFETY TESTS PASSED!\n");
    printf("=======================================================================\n");

    return 0;
}
