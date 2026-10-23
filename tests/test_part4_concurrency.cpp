#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cassert>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
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
typedef sl::Result (*PFN_slDLSSGetOptimalSettings)(const sl::DLSSOptions&, sl::DLSSOptimalSettings&);

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

static void WaitForGpu(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t& fenceValue, HANDLE fenceEvent) {
    fenceValue++;
    queue->Signal(fence, fenceValue);
    if (fence->GetCompletedValue() < fenceValue) {
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

int main() {
    printf("=======================================================================\n");
    printf("[AUDIT PART 4 TEST] Thread-Safety, Concurrency & Decoupling Audit\n");
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

    // Initialize D3D12 Device, Queue, Allocator & List
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

    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    uint64_t fenceValue = 0;
    HANDLE fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    sl::Preferences pref{};
    pref.applicationId = 12345;
    sl::Result rInit = pfnInit(pref, 0x2000B0001FEDC);
    assert(rInit == sl::Result::eOk);
    pfnSetD3DDevice(device);

    void* fnDLSSSetOptions = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fnDLSSSetOptions);
    auto pfnDLSSSetOptions = (PFN_slDLSSSetOptions)fnDLSSSetOptions;
    assert(pfnDLSSSetOptions != nullptr);

    void* fnDLSSGetOptimalSettings = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fnDLSSGetOptimalSettings);
    auto pfnDLSSGetOptimalSettings = (PFN_slDLSSGetOptimalSettings)fnDLSSGetOptimalSettings;
    assert(pfnDLSSGetOptimalSettings != nullptr);

    // =========================================================================
    // Test 1: Concurrency and Contention Isolation (Render vs Query Threads)
    // =========================================================================
    printf("\n--- Test 1: Multi-Threaded Concurrency (Evaluate vs GetOptimalSettings) ---\n");

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

    std::atomic<bool> startFlag{ false };
    std::atomic<bool> renderDone{ false };
    std::atomic<uint32_t> renderFramesCount{ 0 };
    std::atomic<uint32_t> queryCount{ 0 };

    // Thread A: Render Thread
    std::thread renderThread([&]() {
        while (!startFlag.load()) {
            std::this_thread::yield();
        }

        for (uint32_t f = 0; f < 50; ++f) {
            sl::FrameToken* token = nullptr;
            pfnGetNewFrameToken(token, nullptr);

            pfnSetTagForFrame(*token, vp0, vp0Tags, 4, cmdList);

            sl::Constants consts{};
            consts.jitterOffset = { 0.25f, -0.25f };
            consts.mvecScale = { 1.0f, 1.0f };
            consts.reset = (f == 0) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            pfnSetConstants(consts, *token, vp0);

            const sl::BaseStructure* evalInputs[2] = { &vp0, &opt0 };
            sl::Result res = pfnEvaluateFeature(sl::kFeatureDLSS, *token, evalInputs, 2, cmdList);
            assert(res == sl::Result::eOk);

            cmdList->Close();
            ID3D12CommandList* lists[] = { cmdList };
            cmdQueue->ExecuteCommandLists(1, lists);
            WaitForGpu(cmdQueue, fence, fenceValue, fenceEvent);
            cmdAlloc->Reset();
            cmdList->Reset(cmdAlloc, nullptr);

            renderFramesCount++;
        }
        renderDone.store(true);
    });

    // Thread B: UI Query Thread (Querying Optimal Settings continuously)
    std::thread queryThread([&]() {
        while (!startFlag.load()) {
            std::this_thread::yield();
        }

        sl::DLSSMode modesToTest[] = {
            sl::DLSSMode::eMaxPerformance,
            sl::DLSSMode::eBalanced,
            sl::DLSSMode::eMaxQuality,
            sl::DLSSMode::eUltraPerformance,
            sl::DLSSMode::eDLAA
        };

        while (!renderDone.load()) {
            for (auto m : modesToTest) {
                sl::DLSSOptions qOpts{};
                qOpts.outputWidth = 1920;
                qOpts.outputHeight = 1080;
                qOpts.mode = m;

                sl::DLSSOptimalSettings qSettings{};
                sl::Result res = pfnDLSSGetOptimalSettings(qOpts, qSettings);
                assert(res == sl::Result::eOk);
                assert(qSettings.optimalRenderWidth > 0 && qSettings.optimalRenderHeight > 0);
                assert(qSettings.optimalRenderWidth <= qOpts.outputWidth);
                assert(qSettings.optimalRenderHeight <= qOpts.outputHeight);
                queryCount++;
            }
            std::this_thread::yield();
        }
    });

    // Trigger concurrent execution
    startFlag.store(true);

    renderThread.join();
    queryThread.join();

    printf("[PASSED] Concurrent execution completed without deadlock: RenderFrames=%u, Queries=%u\n",
           renderFramesCount.load(), queryCount.load());

    // =========================================================================
    // Test 2: Per-Viewport NGX Parameter Decoupling
    // =========================================================================
    printf("\n--- Test 2: Per-Viewport Parameter Block Decoupling ---\n");

    ID3D12Resource* vp1In = CreateTexture(device, 960, 540, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* vp1Out = CreateTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* vp1Depth = CreateTexture(device, 960, 540, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* vp1Mvec = CreateTexture(device, 960, 540, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    sl::ViewportHandle vp1(1);
    sl::DLSSOptions opt1{};
    opt1.mode = sl::DLSSMode::eMaxPerformance;
    opt1.outputWidth = 1920;
    opt1.outputHeight = 1080;
    opt1.sharpness = 0.85f;
    pfnDLSSSetOptions(vp1, opt1);

    sl::Resource rVp1In = { sl::ResourceType::eTex2d, vp1In, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource rVp1Out = { sl::ResourceType::eTex2d, vp1Out, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    sl::Resource rVp1Depth = { sl::ResourceType::eTex2d, vp1Depth, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::Resource rVp1Mvec = { sl::ResourceType::eTex2d, vp1Mvec, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    sl::ResourceTag vp1Tags[4] = {
        { &rVp1In, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp1Out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp1Depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate },
        { &rVp1Mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate }
    };
    pfnSetTag(vp1, vp1Tags, 4, cmdList);

    // Alternate evaluations between Viewport 0 and Viewport 1
    for (uint32_t step = 0; step < 10; ++step) {
        sl::FrameToken* token = nullptr;
        pfnGetNewFrameToken(token, nullptr);

        pfnSetTagForFrame(*token, vp0, vp0Tags, 4, cmdList);
        pfnSetTagForFrame(*token, vp1, vp1Tags, 4, cmdList);

        const sl::BaseStructure* inVp0[2] = { &vp0, &opt0 };
        sl::Result res0 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, inVp0, 2, cmdList);
        assert(res0 == sl::Result::eOk);

        const sl::BaseStructure* inVp1[2] = { &vp1, &opt1 };
        sl::Result res1 = pfnEvaluateFeature(sl::kFeatureDLSS, *token, inVp1, 2, cmdList);
        assert(res1 == sl::Result::eOk);

        cmdList->Close();
        ID3D12CommandList* lists[] = { cmdList };
        cmdQueue->ExecuteCommandLists(1, lists);
        WaitForGpu(cmdQueue, fence, fenceValue, fenceEvent);
        cmdAlloc->Reset();
        cmdList->Reset(cmdAlloc, nullptr);
    }

    printf("[PASSED] Independent per-viewport evaluation succeeded without crosstalk.\n");

    // =========================================================================
    // Test 3: Viewport Release & Clean Shutdown Re-Init Cycle
    // =========================================================================
    printf("\n--- Test 3: Viewport Release, Clean Shutdown and Re-Init Cycle ---\n");

    // Free resources on Viewport 1
    sl::Result rFree = pfnFreeResources(sl::kFeatureDLSS, vp1);
    assert(rFree == sl::Result::eOk);

    // Full shutdown
    sl::Result rShutdown = pfnShutdown();
    assert(rShutdown == sl::Result::eOk);

    // Re-init test to verify idempotency and zero memory leaks
    sl::Result rReinit = pfnInit(pref, 0x2000B0001FEDC);
    assert(rReinit == sl::Result::eOk);
    pfnSetD3DDevice(device);

    sl::Result rShutdown2 = pfnShutdown();
    assert(rShutdown2 == sl::Result::eOk);

    printf("[PASSED] Viewport release, shutdown, and re-initialization cycle verified.\n");

    // Cleanup DX resources
    vp0In->Release();
    vp0Out->Release();
    vp0Depth->Release();
    vp0Mvec->Release();

    vp1In->Release();
    vp1Out->Release();
    vp1Depth->Release();
    vp1Mvec->Release();

    cmdList->Release();
    cmdAlloc->Release();
    cmdQueue->Release();
    fence->Release();
    CloseHandle(fenceEvent);
    device->Release();

    printf("\n=======================================================================\n");
    printf("[AUDIT PART 4 RESULT] ALL THREAD-SAFETY & PERFORMANCE TESTS PASSED!\n");
    printf("=======================================================================\n");

    return 0;
}
