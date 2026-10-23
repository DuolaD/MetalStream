#pragma once

#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <nvngx/nvsdk_ngx.h>
#include <nvngx/nvsdk_ngx_params.h>
#include <streamline/sl.h>
#include <streamline/sl_dlss.h>
#include "metalstream_diag.h"

namespace metalstream {

struct DRSCoordinates {
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    sl::Extent colorInExt{};
    sl::Extent colorOutExt{};
    sl::Extent depthExt{};
    sl::Extent mvecExt{};
    sl::Extent transparencyExt{};
    sl::Extent biasExt{};
    sl::Extent exposureExt{};
    sl::Extent animTextureExt{};
};

struct ExtraDLSSRenderArgs {
    ID3D12Resource* transparencyMask = nullptr;
    ID3D12Resource* biasCurrentColorMask = nullptr;
    ID3D12Resource* exposureTexture = nullptr;
    ID3D12Resource* animatedTextureMask = nullptr;
    ID3D12Resource* rayTracingHitDistance = nullptr;
    ID3D12Resource* motionVectorsReflection = nullptr;
    ID3D12Resource* particleMask = nullptr;

    D3D12_RESOURCE_STATES transparencyState = (D3D12_RESOURCE_STATES)UINT_MAX;
    D3D12_RESOURCE_STATES biasState = (D3D12_RESOURCE_STATES)UINT_MAX;
    D3D12_RESOURCE_STATES exposureState = (D3D12_RESOURCE_STATES)UINT_MAX;
    D3D12_RESOURCE_STATES animatedTextureState = (D3D12_RESOURCE_STATES)UINT_MAX;
    D3D12_RESOURCE_STATES rayTraceDistState = (D3D12_RESOURCE_STATES)UINT_MAX;
    D3D12_RESOURCE_STATES mvecReflectionsState = (D3D12_RESOURCE_STATES)UINT_MAX;
    D3D12_RESOURCE_STATES particleMaskState = (D3D12_RESOURCE_STATES)UINT_MAX;

    // Presets
    sl::DLSSPreset dlaaPreset = sl::DLSSPreset::eDefault;
    sl::DLSSPreset qualityPreset = sl::DLSSPreset::eDefault;
    sl::DLSSPreset balancedPreset = sl::DLSSPreset::eDefault;
    sl::DLSSPreset performancePreset = sl::DLSSPreset::eDefault;
    sl::DLSSPreset ultraPerformancePreset = sl::DLSSPreset::eDefault;
    sl::DLSSPreset ultraQualityPreset = sl::DLSSPreset::eDefault;

    // Indicator Axis Invert
    bool indicatorInvertAxisX = false;
    bool indicatorInvertAxisY = false;

    // Camera Motion & Clip-to-Prev-Clip Matrix
    bool cameraMotionIncluded = true;
    bool hasClipToPrevClip = false;
    sl::float4x4 clipToPrevClip{};
};

class NVNGXRouter {
public:
    static NVNGXRouter& Instance();

    bool Init(ID3D12Device* device, unsigned long long applicationId = 0);
    void Shutdown(bool isProcessTermination = false);

    bool GetOptimalSettings(
        const sl::DLSSOptions& options,
        sl::DLSSOptimalSettings& settings
    );

    bool EnsureFeatureCreated(
        uint32_t viewport,
        ID3D12GraphicsCommandList* cmdList,
        uint32_t renderWidth,
        uint32_t renderHeight,
        uint32_t targetWidth,
        uint32_t targetHeight,
        sl::DLSSMode mode = sl::DLSSMode::eMaxQuality,
        int createFlags = 0,
        bool* outRecreated = nullptr
    );

    // Backward-compatible overload for default viewport 0
    bool EnsureFeatureCreated(
        ID3D12GraphicsCommandList* cmdList,
        uint32_t renderWidth,
        uint32_t renderHeight,
        uint32_t targetWidth,
        uint32_t targetHeight,
        sl::DLSSMode mode = sl::DLSSMode::eMaxQuality,
        int createFlags = 0,
        bool* outRecreated = nullptr
    ) {
        return EnsureFeatureCreated(0, cmdList, renderWidth, renderHeight, targetWidth, targetHeight, mode, createFlags, outRecreated);
    }

    bool Evaluate(
        uint32_t viewport,
        uint64_t frameIndex,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* inputColor,
        ID3D12Resource* outputColor,
        ID3D12Resource* depth,
        ID3D12Resource* mvec,
        float jitterX,
        float jitterY,
        float mvScaleX,
        float mvScaleY,
        bool reset,
        float sharpness,
        float preExposure,
        float exposureScale,
        D3D12_RESOURCE_STATES inputState,
        D3D12_RESOURCE_STATES outputState,
        sl::DLSSMode mode = sl::DLSSMode::eMaxQuality,
        int createFlags = 0,
        const DRSCoordinates& drs = {},
        D3D12_RESOURCE_STATES depthState = (D3D12_RESOURCE_STATES)UINT_MAX,
        D3D12_RESOURCE_STATES mvecState = (D3D12_RESOURCE_STATES)UINT_MAX,
        const ExtraDLSSRenderArgs& extraArgs = {}
    );

    // Backward-compatible overload with transparencyMask & biasCurrentColorMask
    bool Evaluate(
        uint32_t viewport,
        uint64_t frameIndex,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* inputColor,
        ID3D12Resource* outputColor,
        ID3D12Resource* depth,
        ID3D12Resource* mvec,
        float jitterX,
        float jitterY,
        float mvScaleX,
        float mvScaleY,
        bool reset,
        float sharpness,
        float preExposure,
        float exposureScale,
        D3D12_RESOURCE_STATES inputState,
        D3D12_RESOURCE_STATES outputState,
        sl::DLSSMode mode,
        int createFlags,
        const DRSCoordinates& drs,
        ID3D12Resource* transparencyMask,
        ID3D12Resource* biasCurrentColorMask,
        D3D12_RESOURCE_STATES depthState = (D3D12_RESOURCE_STATES)UINT_MAX,
        D3D12_RESOURCE_STATES mvecState = (D3D12_RESOURCE_STATES)UINT_MAX
    ) {
        ExtraDLSSRenderArgs extra{};
        extra.transparencyMask = transparencyMask;
        extra.biasCurrentColorMask = biasCurrentColorMask;
        return Evaluate(viewport, frameIndex, cmdList, inputColor, outputColor, depth, mvec,
                        jitterX, jitterY, mvScaleX, mvScaleY, reset, sharpness, preExposure,
                        exposureScale, inputState, outputState, mode, createFlags, drs,
                        depthState, mvecState, extra);
    }

    // Backward-compatible overload
    bool Evaluate(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* inputColor,
        ID3D12Resource* outputColor,
        ID3D12Resource* depth,
        ID3D12Resource* mvec,
        float jitterX,
        float jitterY,
        float mvScaleX,
        float mvScaleY,
        bool reset,
        float sharpness,
        float preExposure,
        float exposureScale,
        D3D12_RESOURCE_STATES inputState,
        D3D12_RESOURCE_STATES outputState,
        sl::DLSSMode mode = sl::DLSSMode::eMaxQuality,
        int createFlags = 0,
        const DRSCoordinates& drs = {},
        ID3D12Resource* transparencyMask = nullptr,
        ID3D12Resource* biasCurrentColorMask = nullptr,
        D3D12_RESOURCE_STATES depthState = (D3D12_RESOURCE_STATES)UINT_MAX,
        D3D12_RESOURCE_STATES mvecState = (D3D12_RESOURCE_STATES)UINT_MAX
    ) {
        return Evaluate(0, 0, cmdList, inputColor, outputColor, depth, mvec, jitterX, jitterY, mvScaleX, mvScaleY, reset, sharpness, preExposure, exposureScale, inputState, outputState, mode, createFlags, drs, transparencyMask, biasCurrentColorMask, depthState, mvecState);
    }

    void BlitGuard(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* inputColor,
        ID3D12Resource* outputColor,
        D3D12_RESOURCE_STATES inputState,
        D3D12_RESOURCE_STATES outputState
    );

    void ReleaseViewport(uint32_t viewport);
    bool HasViewportContext(uint32_t viewport);
    size_t GetActiveViewportCount();
    size_t GetDeferredReleaseCount();
    void SetCurrentFrame(uint64_t frame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_currentFrame = frame;
    }
    void TickFrame(uint64_t currentFrame = 0);

    bool IsLoaded() const { return m_nvngxModule != nullptr; }
    ID3D12Device* GetDevice() const { return m_device; }
    // Loaded nvngx module (NGX already initialized on GetDevice()); used by the frame generator
    HMODULE GetNVNGXModule() const { return m_nvngxModule; }

    void GetDiagnostics(MetalStreamDiagnostics& out);

private:
    NVNGXRouter();
    ~NVNGXRouter();

    bool LoadNVNGXLibrary();
    void QueueDeferredRelease(NVSDK_NGX_Handle* handle);
    void ProcessDeferredReleases(bool forceAll = false);

    // Diagnostics (cheap enough for the per-frame path: a few QPC reads and relaxed atomics)
    void RecordLockWait(int64_t waitStartTicks);
    void MaybeLogPeriodicStats(int64_t nowTicks); // caller holds m_mutex

    struct ViewportContext {
        NVSDK_NGX_Handle* handle = nullptr;
        NVSDK_NGX_Parameter* params = nullptr;
        uint32_t currentRenderWidth = 0;
        uint32_t currentRenderHeight = 0;
        uint32_t currentTargetWidth = 0;
        uint32_t currentTargetHeight = 0;
        uint32_t allocatedRenderWidth = 0;
        uint32_t allocatedRenderHeight = 0;
        sl::DLSSMode currentMode = sl::DLSSMode::eOff;
        int currentCreateFlags = 0;
        sl::float4x4 clipToPrevClip{};
    };

    struct DeferredReleaseItem {
        NVSDK_NGX_Handle* handle = nullptr;
        uint64_t releaseFrame = 0;
    };

    HMODULE m_nvngxModule = nullptr;
    ID3D12Device* m_device = nullptr;
    NVSDK_NGX_Parameter* m_ngxParameters = nullptr;
    NVSDK_NGX_Parameter* m_optimalSettingsParams = nullptr;

    std::unordered_map<uint32_t, ViewportContext> m_viewportContexts;
    std::vector<DeferredReleaseItem> m_deferredReleases;
    uint64_t m_currentFrame = 0;

    std::mutex m_mutex;
    std::mutex m_optimalSettingsMutex;

    // Cumulative counters (exported via MetalStream_GetDiagnostics)
    std::atomic<uint64_t> m_statEvaluateCalls{0};
    std::atomic<uint64_t> m_statEvaluateDispatched{0};
    std::atomic<uint64_t> m_statEvaluateFailures{0};
    std::atomic<uint64_t> m_statFeatureCreates{0};
    std::atomic<uint64_t> m_statFeatureCreateFailures{0};
    std::atomic<uint64_t> m_statFeatureRecreates{0};
    std::atomic<uint64_t> m_statBlitFallbacks{0};
    std::atomic<uint64_t> m_statLockWaitSamples{0};
    std::atomic<uint64_t> m_statLockWaitTotalUs{0};
    std::atomic<uint64_t> m_statLockWaitMaxUs{0};    // reset at each periodic summary

    // Periodic summary state (guarded by m_mutex)
    struct StatsSnapshot {
        uint64_t evaluateCalls = 0;
        uint64_t evaluateFailures = 0;
        uint64_t featureCreates = 0;
        uint64_t featureRecreates = 0;
        uint64_t blitFallbacks = 0;
        uint64_t lockWaitSamples = 0;
        uint64_t lockWaitTotalUs = 0;
    };
    StatsSnapshot m_lastSummary;
    int64_t m_lastSummaryTicks = 0;
    int64_t m_lastEvaluateTicks = 0;
    uint64_t m_frameIntervalSumUs = 0;
    uint64_t m_frameIntervalMaxUs = 0;
    uint64_t m_frameIntervalCount = 0;
    uint64_t m_ngxEvalSumUs = 0;
    uint64_t m_ngxEvalMaxUs = 0;
    uint64_t m_ngxEvalCount = 0;

    // Function pointers
    typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_D3D12_Init)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, NVSDK_NGX_Version);
    typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_D3D12_Shutdown)(ID3D12Device*);
    typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_D3D12_AllocateParameters)(NVSDK_NGX_Parameter**);
    typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_D3D12_DestroyParameters)(NVSDK_NGX_Parameter*);
    typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_D3D12_CreateFeature)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_D3D12_ReleaseFeature)(NVSDK_NGX_Handle*);
    typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_D3D12_EvaluateFeature)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);

    PFN_D3D12_Init m_pfnInit = nullptr;
    PFN_D3D12_Shutdown m_pfnShutdown = nullptr;
    PFN_D3D12_AllocateParameters m_pfnAllocateParameters = nullptr;
    PFN_D3D12_DestroyParameters m_pfnDestroyParameters = nullptr;
    PFN_D3D12_CreateFeature m_pfnCreateFeature = nullptr;
    PFN_D3D12_ReleaseFeature m_pfnReleaseFeature = nullptr;
    PFN_D3D12_EvaluateFeature m_pfnEvaluateFeature = nullptr;
};

} // namespace metalstream
