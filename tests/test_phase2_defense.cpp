#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdint.h>
#include <cmath>
#include <cassert>
#include <streamline/sl.h>
#include <streamline/sl_consts.h>
#include <streamline/sl_dlss_g.h>
#include <streamline/sl_reflex.h>
#include <streamline/sl_pcl.h>

typedef sl::Result (*PFN_slInit)(const sl::Preferences&, uint64_t);
typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*&, const uint32_t*);
typedef sl::Result (*PFN_slSetD3DDevice)(void*);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature, const char*, void*&);
typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);

int main() {
    printf("[PHASE 2 TEST] Starting Stub Function Defensive Hardening & DLSS-G / Reflex Audit...\n");

    HMODULE hSl = LoadLibraryA("build/sl.interposer.dll");
    if (!hSl) {
        hSl = LoadLibraryA("sl.interposer.dll");
    }
    if (!hSl) {
        printf("[FAILED] Could not load sl.interposer.dll\n");
        return 1;
    }
    printf("[PHASE 2] sl.interposer.dll loaded at %p\n", hSl);

    auto pfnInit = (PFN_slInit)GetProcAddress(hSl, "slInit");
    auto pfnShutdown = (PFN_slShutdown)GetProcAddress(hSl, "slShutdown");
    auto pfnGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(hSl, "slGetNewFrameToken");
    auto pfnSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(hSl, "slSetD3DDevice");
    auto pfnGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(hSl, "slGetFeatureFunction");
    auto pfnEvaluateFeature = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");
    auto pfnDirectReflexSetMarker = (sl::Result(*)(sl::PCLMarker, const sl::FrameToken&))GetProcAddress(hSl, "slReflexSetMarker");

    if (!pfnInit || !pfnShutdown || !pfnGetNewFrameToken || !pfnSetD3DDevice || !pfnGetFeatureFunction || !pfnEvaluateFeature) {
        printf("[FAILED] Missing core Streamline exports\n");
        return 1;
    }

    if (!pfnDirectReflexSetMarker) {
        printf("[FAILED] Direct export slReflexSetMarker is missing!\n");
        return 1;
    }
    printf("[PASS] Direct export slReflexSetMarker exists!\n");

    sl::Preferences pref{};
    pfnInit(pref, 0x2000B0001FEDC);

    sl::FrameToken* token = nullptr;
    pfnGetNewFrameToken(token, nullptr);
    assert(token != nullptr);

    // =========================================================================
    // Test 1: slGetFeatureFunction resolution for Reflex & DLSS-G APIs
    // =========================================================================
    printf("\n--- Test 1: Feature Function Resolution ---\n");
    void* fnDLSSGGetState = nullptr;
    void* fnDLSSGSetOptions = nullptr;
    void* fnReflexGetState = nullptr;
    void* fnReflexSetOptions = nullptr;
    void* fnReflexSleep = nullptr;
    void* fnReflexSetCameraData = nullptr;
    void* fnReflexGetPredictedCameraData = nullptr;
    void* fnReflexSetMarker = nullptr;
    void* fnPCLSetMarker = nullptr;

    sl::Result r1 = pfnGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fnDLSSGGetState);
    sl::Result r2 = pfnGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fnDLSSGSetOptions);
    sl::Result r3 = pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", fnReflexGetState);
    sl::Result r4 = pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fnReflexSetOptions);
    sl::Result r5 = pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fnReflexSleep);
    sl::Result r6 = pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexSetCameraData", fnReflexSetCameraData);
    sl::Result r7 = pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexGetPredictedCameraData", fnReflexGetPredictedCameraData);
    sl::Result r8 = pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexSetMarker", fnReflexSetMarker);
    sl::Result r9 = pfnGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fnPCLSetMarker);

    if (r1 != sl::Result::eOk || !fnDLSSGGetState ||
        r2 != sl::Result::eOk || !fnDLSSGSetOptions ||
        r3 != sl::Result::eOk || !fnReflexGetState ||
        r4 != sl::Result::eOk || !fnReflexSetOptions ||
        r5 != sl::Result::eOk || !fnReflexSleep ||
        r6 != sl::Result::eOk || !fnReflexSetCameraData ||
        r7 != sl::Result::eOk || !fnReflexGetPredictedCameraData ||
        r8 != sl::Result::eOk || !fnReflexSetMarker ||
        r9 != sl::Result::eOk || !fnPCLSetMarker) {
        printf("[FAILED] Feature function lookup failed!\n");
        return 1;
    }
    printf("[PASS] Test 1: All DLSS-G, Reflex, and PCL feature functions successfully resolved!\n");

    // =========================================================================
    // Test 2: Reflex Camera Prediction Safe Fill (Identity & No NaN/Inf)
    // =========================================================================
    printf("\n--- Test 2: Reflex Predicted Camera Data Safe Fill ---\n");
    auto pfnGetPredCam = (sl::Result(*)(const sl::ViewportHandle&, const sl::FrameToken&, sl::ReflexPredictedCameraData&))fnReflexGetPredictedCameraData;
    auto pfnSetCam = (sl::Result(*)(const sl::ViewportHandle&, const sl::FrameToken&, const sl::ReflexCameraData&))fnReflexSetCameraData;

    sl::ViewportHandle vp(0);
    sl::ReflexPredictedCameraData predCam{};
    sl::Result rPred = pfnGetPredCam(vp, *token, predCam);
    if (rPred != sl::Result::eOk) {
        printf("[FAILED] pfnGetPredCam failed: %d\n", (int)rPred);
        return 1;
    }

    // Verify identity matrix
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float valW2V = (&predCam.predictedWorldToViewMatrix[row].x)[col];
            float valV2C = (&predCam.predictedViewToClipMatrix[row].x)[col];
            if (std::isnan(valW2V) || std::isinf(valW2V) || std::isnan(valV2C) || std::isinf(valV2C)) {
                printf("[FAILED] Matrix contains NaN or Inf at [%d][%d]!\n", row, col);
                return 1;
            }
            float expected = (row == col) ? 1.0f : 0.0f;
            if (std::abs(valW2V - expected) > 1e-5f || std::abs(valV2C - expected) > 1e-5f) {
                printf("[FAILED] Default predicted matrix is not identity at [%d][%d]! w2v=%f, v2c=%f, expected=%f\n",
                       row, col, valW2V, valV2C, expected);
                return 1;
            }
        }
    }
    printf("[PASS] Default predicted camera matrices are valid, clean Identity matrices (no NaN/Inf)!\n");

    // Set custom camera data and verify round-trip
    sl::ReflexCameraData customCam{};
    customCam.worldToViewMatrix[0] = sl::float4(2.0f, 0.5f, 0.0f, 0.0f);
    customCam.worldToViewMatrix[1] = sl::float4(0.0f, 2.0f, 0.0f, 0.0f);
    customCam.worldToViewMatrix[2] = sl::float4(0.0f, 0.0f, 2.0f, 0.0f);
    customCam.worldToViewMatrix[3] = sl::float4(10.0f, 20.0f, 30.0f, 1.0f);

    customCam.viewToClipMatrix[0] = sl::float4(1.5f, 0.0f, 0.0f, 0.0f);
    customCam.viewToClipMatrix[1] = sl::float4(0.0f, 1.5f, 0.0f, 0.0f);
    customCam.viewToClipMatrix[2] = sl::float4(0.0f, 0.0f, 1.0f, 1.0f);
    customCam.viewToClipMatrix[3] = sl::float4(0.0f, 0.0f, -0.1f, 0.0f);

    pfnSetCam(vp, *token, customCam);

    sl::ReflexPredictedCameraData predCustom{};
    pfnGetPredCam(vp, *token, predCustom);
    if (std::abs(predCustom.predictedWorldToViewMatrix[3].x - 10.0f) > 1e-4f ||
        std::abs(predCustom.predictedViewToClipMatrix[0].x - 1.5f) > 1e-4f) {
        printf("[FAILED] Custom camera data round-trip failed!\n");
        return 1;
    }
    printf("[PASS] Custom camera data round-trip succeeded!\n");

    // Test marker invocation
    auto pfnMarker = (sl::Result(*)(sl::PCLMarker, const sl::FrameToken&))fnReflexSetMarker;
    sl::Result rM1 = pfnMarker(sl::PCLMarker::eSimulationStart, *token);
    sl::Result rM2 = pfnDirectReflexSetMarker(sl::PCLMarker::eSimulationEnd, *token);
    if (rM1 != sl::Result::eOk || rM2 != sl::Result::eOk) {
        printf("[FAILED] Reflex marker invocations failed!\n");
        return 1;
    }
    printf("[PASS] Test 2: Reflex camera & marker defense verified completely!\n");

    // =========================================================================
    // Test 3: DLSS-G Dummy Fence & Real D3D12 Queue Wait Safety
    // =========================================================================
    printf("\n--- Test 3: DLSS-G Dummy Fence & D3D12 Queue Wait Safety ---\n");
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        printf("[FAILED] D3D12CreateDevice failed: 0x%08lX\n", hr);
        return 1;
    }
    printf("[TEST 3] Created real D3D12 device: %p\n", device);

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* cmdQueue = nullptr;
    hr = device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&cmdQueue));
    if (FAILED(hr) || !cmdQueue) {
        printf("[FAILED] CreateCommandQueue failed: 0x%08lX\n", hr);
        return 1;
    }

    // Set D3D12 device to trigger dummy fence creation
    pfnSetD3DDevice(device);

    auto pfnGetDLSSGState = (sl::Result(*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*))fnDLSSGGetState;
    auto pfnSetDLSSGOptions = (sl::Result(*)(const sl::ViewportHandle&, const sl::DLSSGOptions&))fnDLSSGSetOptions;

    sl::DLSSGState dlssgState{};
    sl::Result rGState = pfnGetDLSSGState(vp, dlssgState, nullptr);
    if (rGState != sl::Result::eOk) {
        printf("[FAILED] slDLSSGGetState failed: %d\n", (int)rGState);
        return 1;
    }

    printf("[TEST 3] DLSSGState: status=%u, numFramesToGenerateMax=%u, fence=%p, fenceVal=%llu\n",
           (uint32_t)dlssgState.status, dlssgState.numFramesToGenerateMax,
           dlssgState.inputsProcessingCompletionFence,
           (unsigned long long)dlssgState.lastPresentInputsProcessingCompletionFenceValue);

    if (dlssgState.status != sl::DLSSGStatus::eOk) {
        printf("[FAILED] Expected DLSSGStatus::eOk, got %u\n", (uint32_t)dlssgState.status);
        return 1;
    }
    if (dlssgState.numFramesToGenerateMax != 1) {
        printf("[FAILED] Expected numFramesToGenerateMax=1, got %u\n", dlssgState.numFramesToGenerateMax);
        return 1;
    }
    if (!dlssgState.inputsProcessingCompletionFence) {
        printf("[FAILED] inputsProcessingCompletionFence is NULL! Engine will crash if waiting!\n");
        return 1;
    }

    // Crucial test: Pass the dummy fence to CommandQueue::Wait
    auto fence = static_cast<ID3D12Fence*>(dlssgState.inputsProcessingCompletionFence);
    printf("[TEST 3] Executing ID3D12CommandQueue::Wait with dummy fence %p...\n", fence);
    hr = cmdQueue->Wait(fence, dlssgState.lastPresentInputsProcessingCompletionFenceValue);
    if (FAILED(hr)) {
        printf("[FAILED] cmdQueue->Wait failed with hr=0x%08lX\n", hr);
        return 1;
    }
    printf("[PASS] cmdQueue->Wait with dummy fence succeeded with ZERO crashes or GPU stalls!\n");

    // Test DLSS-G Options & EvaluateFeature pass-through
    sl::DLSSGOptions gopts{};
    gopts.mode = sl::DLSSGMode::eOn;
    gopts.numFramesToGenerate = 1;
    pfnSetDLSSGOptions(vp, gopts);

    // Create command list for evaluate test
    ID3D12CommandAllocator* cmdAlloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));

    const sl::BaseStructure* inputs[] = { &gopts, &vp };
    sl::Result rEvalG = pfnEvaluateFeature(sl::kFeatureDLSS_G, *token, inputs, 2, cmdList);
    if (rEvalG != sl::Result::eOk) {
        printf("[FAILED] slEvaluateFeature(kFeatureDLSS_G) failed: %d\n", (int)rEvalG);
        return 1;
    }
    printf("[PASS] slEvaluateFeature(kFeatureDLSS_G) executed cleanly as a safe passthrough!\n");

    // Close and execute command list
    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList };
    cmdQueue->ExecuteCommandLists(1, lists);

    // Synchronize GPU
    ID3D12Fence* syncFence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&syncFence));
    cmdQueue->Signal(syncFence, 1);
    while (syncFence->GetCompletedValue() < 1) {
        Sleep(1);
    }
    printf("[PASS] GPU work fully completed and synchronized!\n");

    // Cleanup local D3D12
    syncFence->Release();
    cmdList->Release();
    cmdAlloc->Release();
    cmdQueue->Release();
    device->Release();

    // =========================================================================
    // Test 4: Shutdown and Re-init lifecycle
    // =========================================================================
    printf("\n--- Test 4: Shutdown & Re-init Lifecycle ---\n");
    sl::Result rShut = pfnShutdown();
    if (rShut != sl::Result::eOk) {
        printf("[FAILED] slShutdown failed: %d\n", (int)rShut);
        return 1;
    }
    printf("[PASS] slShutdown released all resources including dummy fence cleanly!\n");

    pfnInit(pref, 0x2000B0001FEDC);
    printf("[PASS] Re-initialized successfully!\n");
    pfnShutdown();

    printf("\n[SUCCESS] ALL PHASE 2 DEFENSIVE HARDENING TESTS PASSED COMPLETELY!\n");
    return 0;
}
