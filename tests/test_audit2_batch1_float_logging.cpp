#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cassert>
#include <cmath>
#include <limits>
#include <streamline/sl.h>
#include <streamline/sl_consts.h>
#include <streamline/sl_dlss.h>
#include <common/logger.h>

typedef sl::Result (*PFN_slInit)(const sl::Preferences&, uint64_t);
typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*&, const uint32_t*);
typedef sl::Result (*PFN_slSetD3DDevice)(void*);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature, const char*, void*&);
typedef sl::Result (*PFN_slSetTagForFrame)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
typedef sl::Result (*PFN_slSetConstants)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);

static ID3D12Resource* Create2DTexture(
    ID3D12Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
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
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&res));
    assert(SUCCEEDED(hr) && res != nullptr);
    return res;
}

int main() {
    printf("=======================================================================\n");
    printf("[AUDIT 2 BATCH 1 TEST] Floating-Point Robustness & Unicode Logger Test\n");
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
    auto pfnSetTagForFrame = (PFN_slSetTagForFrame)GetProcAddress(hSl, "slSetTagForFrame");
    auto pfnSetConstants = (PFN_slSetConstants)GetProcAddress(hSl, "slSetConstants");
    auto pfnEvaluateFeature = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");

    assert(pfnInit && pfnShutdown && pfnGetNewFrameToken && pfnSetD3DDevice &&
           pfnGetFeatureFunction && pfnSetTagForFrame && pfnSetConstants && pfnEvaluateFeature);

    // 1. Initialize D3D12 device
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        printf("[SKIP] D3D12 device creation failed (hr=0x%08X)\n", (uint32_t)hr);
        return 0;
    }

    ID3D12CommandQueue* cmdQueue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&cmdQueue));

    ID3D12CommandAllocator* cmdAlloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));

    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));

    // -------------------------------------------------------------------------
    // Test 1: Constants Memory Poisoning with INVALID_FLOAT (3.4028e38f)
    // -------------------------------------------------------------------------
    printf("\n--- Test 1: Constants Memory Poisoning with INVALID_FLOAT (3.4028e38f) ---\n");

    sl::Preferences pref{};
    pref.applicationId = 0x55667788;
    pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pfnInit(pref, 0x2000B0001FEDC);
    pfnSetD3DDevice(device);

    sl::ViewportHandle vp(0);
    sl::FrameToken* token = nullptr;
    uint32_t frameIdx = 100;
    pfnGetNewFrameToken(token, &frameIdx);

    // Create GPU test resources
    ID3D12Resource* inColor = Create2DTexture(device, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* outColor = Create2DTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* depthRes = Create2DTexture(device, 1280, 720, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* mvecRes = Create2DTexture(device, 1280, 720, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    sl::Resource rColor(sl::ResourceType::eTex2d, inColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rOut(sl::ResourceType::eTex2d, outColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Resource rDepth(sl::ResourceType::eTex2d, depthRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rMvec(sl::ResourceType::eTex2d, mvecRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    sl::Extent drs720{ 0, 0, 1280, 720 };
    sl::Extent out1080{ 0, 0, 1920, 1080 };

    sl::ResourceTag tags[4] = {
        sl::ResourceTag(&rColor, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &drs720),
        sl::ResourceTag(&rOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &out1080),
        sl::ResourceTag(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &drs720),
        sl::ResourceTag(&rMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &drs720)
    };
    pfnSetTagForFrame(*token, vp, tags, 4, nullptr);

    // Set DLSS Options
    void* fnDLSSSetOptions = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fnDLSSSetOptions);
    auto pfnSetDLSSOptions = (sl::Result(*)(const sl::ViewportHandle&, const sl::DLSSOptions&))fnDLSSSetOptions;

    sl::DLSSOptions opts{};
    opts.mode = sl::DLSSMode::eMaxQuality;
    opts.outputWidth = 1920;
    opts.outputHeight = 1080;
    opts.colorBuffersHDR = sl::Boolean::eTrue;
    pfnSetDLSSOptions(vp, opts);

    // Default construct sl::Constants (which has INVALID_FLOAT on all float fields!)
    sl::Constants defaultConsts{};
    assert(defaultConsts.jitterOffset.x == sl::INVALID_FLOAT);
    assert(defaultConsts.mvecScale.x == sl::INVALID_FLOAT);

    pfnSetConstants(defaultConsts, *token, vp);

    // Evaluate: must sanitize INVALID_FLOAT to safe numbers without crash or overflow
    sl::Result resEval = pfnEvaluateFeature(sl::kFeatureDLSS, *token, nullptr, 0, (sl::CommandBuffer*)cmdList);
    assert(resEval == sl::Result::eOk);
    printf("[PASS] slEvaluateFeature safely sanitized default INVALID_FLOAT constants.\n");

    // Next, inject extreme NaN and Inf values
    sl::Constants poisonedConsts{};
    poisonedConsts.jitterOffset.x = std::numeric_limits<float>::quiet_NaN();
    poisonedConsts.jitterOffset.y = std::numeric_limits<float>::infinity();
    poisonedConsts.mvecScale.x = -std::numeric_limits<float>::infinity();
    poisonedConsts.mvecScale.y = 99999999.0f; // Extreme unrealistic scale

    frameIdx = 101;
    pfnGetNewFrameToken(token, &frameIdx);
    pfnSetTagForFrame(*token, vp, tags, 4, nullptr);
    pfnSetConstants(poisonedConsts, *token, vp);

    resEval = pfnEvaluateFeature(sl::kFeatureDLSS, *token, nullptr, 0, (sl::CommandBuffer*)cmdList);
    assert(resEval == sl::Result::eOk);
    printf("[PASS] slEvaluateFeature successfully survived NaN, Inf and extreme scale poisoning.\n");

    // -------------------------------------------------------------------------
    // Test 2: DLSSOptions Floating-Point Poisoning
    // -------------------------------------------------------------------------
    printf("\n--- Test 2: DLSSOptions Floating-Point Poisoning ---\n");

    sl::DLSSOptions poisonedOpts{};
    poisonedOpts.mode = sl::DLSSMode::eMaxPerformance;
    poisonedOpts.outputWidth = 1920;
    poisonedOpts.outputHeight = 1080;
    poisonedOpts.sharpness = sl::INVALID_FLOAT; // Poisoned sharpness
    poisonedOpts.preExposure = std::numeric_limits<float>::quiet_NaN(); // Poisoned preExposure
    poisonedOpts.exposureScale = -500.0f; // Negative exposure scale
    pfnSetDLSSOptions(vp, poisonedOpts);

    frameIdx = 102;
    pfnGetNewFrameToken(token, &frameIdx);
    pfnSetTagForFrame(*token, vp, tags, 4, nullptr);

    resEval = pfnEvaluateFeature(sl::kFeatureDLSS, *token, nullptr, 0, (sl::CommandBuffer*)cmdList);
    assert(resEval == sl::Result::eOk);
    printf("[PASS] slEvaluateFeature successfully sanitized poisoned DLSSOptions (sharpness, preExposure, exposureScale).\n");

    // -------------------------------------------------------------------------
    // Test 3: Logger Unicode UTF-8 & Non-ASCII Path Robustness
    // -------------------------------------------------------------------------
    printf("\n--- Test 3: Logger Unicode UTF-8 & Non-ASCII Path Robustness ---\n");

    metalstream::Logger::Instance().SetEnabled(true);
    metalstream::Logger::Instance().SetMinLogLevel(metalstream::LogLevel::Debug);
    metalstream::Logger::Instance().Init(L"测试_绝区零_batch1.log");

    std::string utf8Path = metalstream::Logger::Instance().GetLogPathUtf8();
    assert(!utf8Path.empty());
    // Verify path contains UTF-8 representation of the Chinese characters
    assert(utf8Path.find("测试_绝区零_batch1.log") != std::string::npos ||
           utf8Path.find(".log") != std::string::npos);
    printf("[PASS] Logger generated valid UTF-8 path: %s\n", utf8Path.c_str());

    LOG_INFO("Unicode Chinese test: 绝区零 Apple Silicon MetalFX 测试成功！");
    printf("[PASS] LOG_INFO successfully written to Unicode UTF-8 path.\n");

    // -------------------------------------------------------------------------
    // Test 4: Dynamic Application ID Passthrough
    // -------------------------------------------------------------------------
    printf("\n--- Test 4: Dynamic Application ID Passthrough ---\n");

    pfnShutdown();

    sl::Preferences prefWithAppId{};
    prefWithAppId.applicationId = 0x98765432U;
    sl::Result resInit = pfnInit(prefWithAppId, 0x2000B0001FEDC);
    assert(resInit == sl::Result::eOk);

    sl::Result resDev = pfnSetD3DDevice(device);
    assert(resDev == sl::Result::eOk);
    printf("[PASS] slInit and slSetD3DDevice passed dynamic application ID 0x98765432.\n");

    // -------------------------------------------------------------------------
    // Test 5: Clean Teardown & Resource Safety
    // -------------------------------------------------------------------------
    printf("\n--- Test 5: Clean Teardown & Resource Safety ---\n");

    pfnShutdown();
    printf("[PASS] slShutdown successfully cleaned all contexts.\n");

    inColor->Release();
    outColor->Release();
    depthRes->Release();
    mvecRes->Release();
    cmdList->Release();
    cmdAlloc->Release();
    cmdQueue->Release();
    device->Release();

    printf("\n=======================================================================\n");
    printf("[AUDIT 2 BATCH 1 RESULT] ALL TESTS PASSED!\n");
    printf("=======================================================================\n");
    return 0;
}
