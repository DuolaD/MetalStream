#include "nvngx_router.h"
#include "../common/logger.h"
#include <nvngx/nvsdk_ngx_defs.h>
#include <algorithm>
#include <cmath>

namespace metalstream {

static inline bool IsValidFiniteFloat(float v) {
    if (std::isnan(v) || std::isinf(v)) return false;
    if (v >= 3.0e38f || v <= -3.0e38f) return false;
    return true;
}

static inline int64_t QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static inline uint64_t TicksToUs(int64_t ticks) {
    static const int64_t s_freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    return ticks <= 0 ? 0 : (uint64_t)(ticks * 1000000 / s_freq);
}

static inline void AtomicMax(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t cur = target.load(std::memory_order_relaxed);
    while (value > cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

static constexpr uint64_t kStatsIntervalUs = 5000000;

NVNGXRouter& NVNGXRouter::Instance() {
    static NVNGXRouter s_instance;
    return s_instance;
}

NVNGXRouter::NVNGXRouter() = default;

NVNGXRouter::~NVNGXRouter() {
    Shutdown(true);
}

bool NVNGXRouter::LoadNVNGXLibrary() {
    if (m_nvngxModule) return true;

    // 1. Try local or standard DLL search
    m_nvngxModule = LoadLibraryW(L"nvngx.dll");
    if (!m_nvngxModule) {
        m_nvngxModule = LoadLibraryW(L"nvngx-on-metalfx.dll");
    }
    if (!m_nvngxModule) {
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring fullPath = std::wstring(sysDir) + L"\\nvngx.dll";
        m_nvngxModule = LoadLibraryW(fullPath.c_str());
    }

    if (!m_nvngxModule) {
        LOG_WARN("Could not load nvngx.dll or nvngx-on-metalfx.dll");
        return false;
    }

    m_pfnInit = (PFN_D3D12_Init)GetProcAddress(m_nvngxModule, "NVSDK_NGX_D3D12_Init");
    m_pfnShutdown = (PFN_D3D12_Shutdown)GetProcAddress(m_nvngxModule, "NVSDK_NGX_D3D12_Shutdown");
    m_pfnAllocateParameters = (PFN_D3D12_AllocateParameters)GetProcAddress(m_nvngxModule, "NVSDK_NGX_D3D12_AllocateParameters");
    m_pfnDestroyParameters = (PFN_D3D12_DestroyParameters)GetProcAddress(m_nvngxModule, "NVSDK_NGX_D3D12_DestroyParameters");
    m_pfnCreateFeature = (PFN_D3D12_CreateFeature)GetProcAddress(m_nvngxModule, "NVSDK_NGX_D3D12_CreateFeature");
    m_pfnReleaseFeature = (PFN_D3D12_ReleaseFeature)GetProcAddress(m_nvngxModule, "NVSDK_NGX_D3D12_ReleaseFeature");
    m_pfnEvaluateFeature = (PFN_D3D12_EvaluateFeature)GetProcAddress(m_nvngxModule, "NVSDK_NGX_D3D12_EvaluateFeature");

    if (!m_pfnInit || !m_pfnCreateFeature || !m_pfnEvaluateFeature) {
        LOG_ERROR("nvngx.dll missing required entry points");
        FreeLibrary(m_nvngxModule);
        m_nvngxModule = nullptr;
        return false;
    }

    LOG_INFO("Successfully loaded nvngx.dll with MetalFX support");
    return true;
}

bool NVNGXRouter::Init(ID3D12Device* device, unsigned long long applicationId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!device) return false;
    m_device = device;

    if (!LoadNVNGXLibrary()) {
        return false;
    }

    if (!m_ngxParameters && m_pfnAllocateParameters) {
        NVSDK_NGX_Result res = m_pfnAllocateParameters(&m_ngxParameters);
        if (NVSDK_NGX_FAILED(res)) {
            LOG_ERROR("NVSDK_NGX_D3D12_AllocateParameters failed: 0x%08X", res);
            return false;
        }
    }

    if (m_pfnInit) {
        unsigned long long appId = applicationId ? applicationId : 0x2401;
        NVSDK_NGX_Result res = m_pfnInit(appId, L".", m_device, nullptr, NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(res)) {
            LOG_WARN("NVSDK_NGX_D3D12_Init returned 0x%08X (appId=0x%llX)", res, appId);
        } else {
            LOG_INFO("NVSDK_NGX_D3D12_Init succeeded with appId=0x%llX", appId);
        }
    }

    return true;
}

void NVNGXRouter::Shutdown(bool isProcessTermination) {
    if (isProcessTermination) {
        LOG_INFO("NVNGXRouter::Shutdown: Fast exit bypass on process termination");
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    std::lock_guard<std::mutex> optLock(m_optimalSettingsMutex);

    // 1. Force flush all deferred releases
    ProcessDeferredReleases(true);

    // 2. Release all active viewport feature handles and their parameters
    for (auto& pair : m_viewportContexts) {
        if (pair.second.handle && m_pfnReleaseFeature) {
            m_pfnReleaseFeature(pair.second.handle);
            pair.second.handle = nullptr;
        }
        if (pair.second.params && m_pfnDestroyParameters) {
            m_pfnDestroyParameters(pair.second.params);
            pair.second.params = nullptr;
        }
    }
    m_viewportContexts.clear();
    m_currentFrame = 0;
    m_lastEvaluateTicks = 0;

    // 3. Destroy optimal settings parameters
    if (m_optimalSettingsParams && m_pfnDestroyParameters) {
        m_pfnDestroyParameters(m_optimalSettingsParams);
        m_optimalSettingsParams = nullptr;
    }

    // 4. Destroy global parameters and shutdown NVNGX library
    if (m_ngxParameters && m_pfnDestroyParameters) {
        m_pfnDestroyParameters(m_ngxParameters);
        m_ngxParameters = nullptr;
    }
    if (m_pfnShutdown && m_device) {
        m_pfnShutdown(m_device);
    }
    if (m_nvngxModule) {
        FreeLibrary(m_nvngxModule);
        m_nvngxModule = nullptr;
    }
    m_device = nullptr;
}

bool NVNGXRouter::GetOptimalSettings(
    const sl::DLSSOptions& options,
    sl::DLSSOptimalSettings& settings)
{
    {
        const int64_t waitStart = QpcNow();
        std::lock_guard<std::mutex> coreLock(m_mutex);
        RecordLockWait(waitStart);
        if (!m_nvngxModule) {
            LoadNVNGXLibrary();
        }
        if (m_nvngxModule && m_pfnAllocateParameters && !m_optimalSettingsParams) {
            m_pfnAllocateParameters(&m_optimalSettingsParams);
        }
    }

    std::lock_guard<std::mutex> lock(m_optimalSettingsMutex);

    if (m_optimalSettingsParams) {
        void* callback = nullptr;
        m_optimalSettingsParams->Get(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
        if (callback) {
            m_optimalSettingsParams->Set(NVSDK_NGX_Parameter_Width, options.outputWidth);
            m_optimalSettingsParams->Set(NVSDK_NGX_Parameter_Height, options.outputHeight);

            NVSDK_NGX_PerfQuality_Value perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
            switch (options.mode) {
                case sl::DLSSMode::eMaxPerformance:   perfQuality = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
                case sl::DLSSMode::eBalanced:         perfQuality = NVSDK_NGX_PerfQuality_Value_Balanced; break;
                case sl::DLSSMode::eMaxQuality:       perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
                case sl::DLSSMode::eUltraPerformance: perfQuality = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
                case sl::DLSSMode::eUltraQuality:     perfQuality = NVSDK_NGX_PerfQuality_Value_UltraQuality; break;
                case sl::DLSSMode::eDLAA:             perfQuality = NVSDK_NGX_PerfQuality_Value_DLAA; break;
                default:                              perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
            }
            m_optimalSettingsParams->Set(NVSDK_NGX_Parameter_PerfQualityValue, (unsigned int)perfQuality);
            m_optimalSettingsParams->Set(NVSDK_NGX_Parameter_RTXValue, 0);

            typedef NVSDK_NGX_Result (*PFN_DLSS_GetOptimalSettingsCallback)(NVSDK_NGX_Parameter*);
            auto pfnGetOptimal = (PFN_DLSS_GetOptimalSettingsCallback)callback;
            NVSDK_NGX_Result res = pfnGetOptimal(m_optimalSettingsParams);
            if (NVSDK_NGX_SUCCEED(res)) {
                uint32_t optW = 0, optH = 0;
                m_optimalSettingsParams->Get(NVSDK_NGX_Parameter_OutWidth, &optW);
                m_optimalSettingsParams->Get(NVSDK_NGX_Parameter_OutHeight, &optH);
                if (optW > 0 && optH > 0) {
                    settings.optimalRenderWidth = (optW + 1) & ~1u;
                    settings.optimalRenderHeight = (optH + 1) & ~1u;
                    m_optimalSettingsParams->Get(NVSDK_NGX_Parameter_Sharpness, &settings.optimalSharpness);
                    m_optimalSettingsParams->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &settings.renderWidthMax);
                    m_optimalSettingsParams->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &settings.renderHeightMax);
                    m_optimalSettingsParams->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &settings.renderWidthMin);
                    m_optimalSettingsParams->Get(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &settings.renderHeightMin);
                    LOG_DEBUG("nvngx GetOptimalSettings succeeded: %ux%u for out %ux%u, mode %u",
                             settings.optimalRenderWidth, settings.optimalRenderHeight,
                             options.outputWidth, options.outputHeight, (uint32_t)options.mode);
                    return true;
                }
            }
        }
    }
    return false;
}

void NVNGXRouter::QueueDeferredRelease(NVSDK_NGX_Handle* handle) {
    if (!handle) return;
    uint64_t releaseFrame = m_currentFrame + 3;
    m_deferredReleases.push_back({ handle, releaseFrame });
    LOG_INFO("NVNGXRouter: Queued handle %p for deferred release at frame %llu (current frame %llu)",
             handle, releaseFrame, m_currentFrame);
}

void NVNGXRouter::ProcessDeferredReleases(bool forceAll) {
    if (!m_pfnReleaseFeature) return;
    auto it = m_deferredReleases.begin();
    while (it != m_deferredReleases.end()) {
        if (forceAll || m_currentFrame >= it->releaseFrame) {
            LOG_DEBUG("Releasing deferred NVNGX handle %p (current frame %llu, target frame %llu)",
                      it->handle, m_currentFrame, it->releaseFrame);
            m_pfnReleaseFeature(it->handle);
            it = m_deferredReleases.erase(it);
        } else {
            ++it;
        }
    }
}

void NVNGXRouter::TickFrame(uint64_t currentFrame) {
    const int64_t waitStart = QpcNow();
    std::lock_guard<std::mutex> lock(m_mutex);
    RecordLockWait(waitStart);
    if (currentFrame > m_currentFrame) {
        m_currentFrame = currentFrame;
        ProcessDeferredReleases(false);
    }
}

void NVNGXRouter::ReleaseViewport(uint32_t viewport) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_viewportContexts.find(viewport);
    if (it != m_viewportContexts.end()) {
        if (it->second.handle) {
            QueueDeferredRelease(it->second.handle);
            it->second.handle = nullptr;
        }
        if (it->second.params && m_pfnDestroyParameters) {
            m_pfnDestroyParameters(it->second.params);
            it->second.params = nullptr;
        }
        m_viewportContexts.erase(it);
        LOG_INFO("NVNGXRouter: Released viewport %u (handle queued for deferred release, params destroyed)", viewport);
    }
}

bool NVNGXRouter::HasViewportContext(uint32_t viewport) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_viewportContexts.find(viewport) != m_viewportContexts.end();
}

size_t NVNGXRouter::GetActiveViewportCount() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_viewportContexts.size();
}

size_t NVNGXRouter::GetDeferredReleaseCount() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_deferredReleases.size();
}

bool NVNGXRouter::EnsureFeatureCreated(
    uint32_t viewport,
    ID3D12GraphicsCommandList* cmdList,
    uint32_t renderWidth,
    uint32_t renderHeight,
    uint32_t targetWidth,
    uint32_t targetHeight,
    sl::DLSSMode mode,
    int createFlags,
    bool* outRecreated)
{
    if (outRecreated) *outRecreated = false;
    if (!m_pfnCreateFeature || !cmdList) return false;

    auto& ctx = m_viewportContexts[viewport];

    // If DLSSMode is eOff, release any existing feature handle and do not create
    if (mode == sl::DLSSMode::eOff) {
        if (ctx.handle) {
            QueueDeferredRelease(ctx.handle);
            ctx.handle = nullptr;
            if (outRecreated) *outRecreated = true;
        }
        return false;
    }

    // Allocate per-viewport parameter block if not present
    if (!ctx.params && m_pfnAllocateParameters) {
        NVSDK_NGX_Result res = m_pfnAllocateParameters(&ctx.params);
        if (NVSDK_NGX_FAILED(res)) {
            LOG_WARN("Failed to allocate per-viewport NGX parameters for viewport %u: 0x%08X, fallback to global", viewport, res);
        }
    }
    NVSDK_NGX_Parameter* params = ctx.params ? ctx.params : m_ngxParameters;
    if (!params) return false;

    bool needsRecreate = false;
    if (ctx.handle) {
        if (ctx.currentTargetWidth != targetWidth || ctx.currentTargetHeight != targetHeight) {
            LOG_INFO("Target output resolution changed for viewport %u (%ux%u -> %ux%u), recreating DLSS feature",
                     viewport, ctx.currentTargetWidth, ctx.currentTargetHeight, targetWidth, targetHeight);
            needsRecreate = true;
        } else if (ctx.currentMode != mode) {
            LOG_INFO("DLSS mode changed for viewport %u (%u -> %u), recreating DLSS feature",
                     viewport, (uint32_t)ctx.currentMode, (uint32_t)mode);
            needsRecreate = true;
        } else if (ctx.currentCreateFlags != createFlags) {
            LOG_INFO("DLSS create flags changed for viewport %u (0x%X -> 0x%X), recreating DLSS feature",
                     viewport, ctx.currentCreateFlags, createFlags);
            needsRecreate = true;
        } else if (renderWidth > ctx.allocatedRenderWidth || renderHeight > ctx.allocatedRenderHeight) {
            LOG_INFO("Render resolution exceeded allocated capacity for viewport %u (%ux%u > %ux%u), recreating DLSS feature",
                     viewport, renderWidth, renderHeight, ctx.allocatedRenderWidth, ctx.allocatedRenderHeight);
            needsRecreate = true;
        }
    }

    if (ctx.handle && needsRecreate) {
        m_statFeatureRecreates.fetch_add(1, std::memory_order_relaxed);
        QueueDeferredRelease(ctx.handle);
        ctx.handle = nullptr;
        if (outRecreated) *outRecreated = true;
    }

    if (!ctx.handle) {
        ctx.allocatedRenderWidth = renderWidth;
        ctx.allocatedRenderHeight = renderHeight;
        ctx.currentRenderWidth = renderWidth;
        ctx.currentRenderHeight = renderHeight;
        ctx.currentTargetWidth = targetWidth;
        ctx.currentTargetHeight = targetHeight;
        ctx.currentMode = mode;
        ctx.currentCreateFlags = createFlags;

        NVSDK_NGX_PerfQuality_Value perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
        switch (mode) {
            case sl::DLSSMode::eMaxPerformance:   perfQuality = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
            case sl::DLSSMode::eBalanced:         perfQuality = NVSDK_NGX_PerfQuality_Value_Balanced; break;
            case sl::DLSSMode::eMaxQuality:       perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
            case sl::DLSSMode::eUltraPerformance: perfQuality = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
            case sl::DLSSMode::eUltraQuality:     perfQuality = NVSDK_NGX_PerfQuality_Value_UltraQuality; break;
            case sl::DLSSMode::eDLAA:             perfQuality = NVSDK_NGX_PerfQuality_Value_DLAA; break;
            default:                              perfQuality = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
        }

        params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1);
        params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
        params->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
        params->Set(NVSDK_NGX_Parameter_Width, renderWidth);
        params->Set(NVSDK_NGX_Parameter_Height, renderHeight);
        params->Set(NVSDK_NGX_Parameter_OutWidth, targetWidth);
        params->Set(NVSDK_NGX_Parameter_OutHeight, targetHeight);
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, (unsigned int)perfQuality);
        params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, createFlags);

        const int64_t createStart = QpcNow();
        NVSDK_NGX_Result res = m_pfnCreateFeature(
            cmdList,
            NVSDK_NGX_Feature_SuperSampling,
            params,
            &ctx.handle
        );
        const uint64_t createUs = TicksToUs(QpcNow() - createStart);

        if (NVSDK_NGX_FAILED(res)) {
            m_statFeatureCreateFailures.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("NVSDK_NGX_D3D12_CreateFeature failed for viewport %u: 0x%08X", viewport, res);
            return false;
        }
        m_statFeatureCreates.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("MetalFX DLSS feature created successfully for viewport %u (handle: %p, render: %ux%u, target: %ux%u, mode: %u, flags: 0x%X, took %.2f ms)",
                 viewport, ctx.handle, renderWidth, renderHeight, targetWidth, targetHeight, (uint32_t)mode, createFlags,
                 createUs / 1000.0);
    } else {
        // Reuse handle for DRS scaling within allocated capacity
        ctx.currentRenderWidth = renderWidth;
        ctx.currentRenderHeight = renderHeight;
    }

    return true;
}

bool NVNGXRouter::Evaluate(
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
    D3D12_RESOURCE_STATES depthState,
    D3D12_RESOURCE_STATES mvecState,
    const ExtraDLSSRenderArgs& extraArgs)
{
    const int64_t waitStart = QpcNow();
    std::lock_guard<std::mutex> lock(m_mutex);
    const int64_t evalStart = QpcNow();
    RecordLockWait(waitStart);

    if (!cmdList || !inputColor || !outputColor) {
        return false;
    }

    m_statEvaluateCalls.fetch_add(1, std::memory_order_relaxed);
    if (m_lastEvaluateTicks != 0) {
        // Interval between consecutive Evaluate calls approximates the render-thread frame time
        const uint64_t intervalUs = TicksToUs(evalStart - m_lastEvaluateTicks);
        m_frameIntervalSumUs += intervalUs;
        m_frameIntervalMaxUs = std::max(m_frameIntervalMaxUs, intervalUs);
        m_frameIntervalCount++;
    }
    m_lastEvaluateTicks = evalStart;
    MaybeLogPeriodicStats(evalStart);

    // Short-circuit when DLSSMode is eOff: bypass MetalFX entirely and perform blit copy
    if (mode == sl::DLSSMode::eOff) {
        LOG_DEBUG("DLSSMode::eOff active for viewport %u, short-circuiting to BlitGuard", viewport);
        BlitGuard(cmdList, inputColor, outputColor, inputState, outputState);
        return true;
    }

    if (frameIndex > 0) {
        m_currentFrame = (frameIndex > m_currentFrame) ? frameIndex : m_currentFrame;
    } else {
        m_currentFrame++;
    }

    // Process deferred releases for handles that have passed the 3-frame safety window
    ProcessDeferredReleases(false);

    D3D12_RESOURCE_DESC inDesc = inputColor->GetDesc();
    D3D12_RESOURCE_DESC outDesc = outputColor->GetDesc();

    uint32_t renderW = (drs.renderWidth > 0) ? drs.renderWidth : (uint32_t)inDesc.Width;
    uint32_t renderH = (drs.renderHeight > 0) ? drs.renderHeight : (uint32_t)inDesc.Height;
    uint32_t targetW = (drs.targetWidth > 0) ? drs.targetWidth : (uint32_t)outDesc.Width;
    uint32_t targetH = (drs.targetHeight > 0) ? drs.targetHeight : (uint32_t)outDesc.Height;

    bool featureRecreated = false;
    if (!EnsureFeatureCreated(viewport, cmdList, renderW, renderH, targetW, targetH, mode, createFlags, &featureRecreated)) {
        LOG_WARN("MetalFX feature creation failed for viewport %u, falling back to BlitGuard", viewport);
        BlitGuard(cmdList, inputColor, outputColor, inputState, outputState);
        return false;
    }

    if (featureRecreated) {
        // Force reset temporal history when reallocating feature on resolution change
        reset = true;
    }

    auto& ctx = m_viewportContexts[viewport];
    NVSDK_NGX_Parameter* params = ctx.params ? ctx.params : m_ngxParameters;
    if (!params) return false;

    // Sanitize floating-point parameters against INVALID_FLOAT / NaN / Inf
    float safeJitterX = IsValidFiniteFloat(jitterX) ? jitterX : 0.0f;
    float safeJitterY = IsValidFiniteFloat(jitterY) ? jitterY : 0.0f;
    float safeMvScaleX = (IsValidFiniteFloat(mvScaleX) && std::abs(mvScaleX) > 1e-6f) ? mvScaleX : (float)renderW;
    float safeMvScaleY = (IsValidFiniteFloat(mvScaleY) && std::abs(mvScaleY) > 1e-6f) ? mvScaleY : (float)renderH;
    float safeSharpness = (IsValidFiniteFloat(sharpness) && sharpness >= 0.0f) ? std::min(sharpness, 1.0f) : 0.0f;
    float safePreExposure = (IsValidFiniteFloat(preExposure) && preExposure > 0.0f && preExposure < 1000.0f) ? preExposure : 1.0f;
    float safeExposureScale = (IsValidFiniteFloat(exposureScale) && exposureScale > 0.0f && exposureScale < 1000.0f) ? exposureScale : 1.0f;

    // Set evaluate parameters
    params->Set(NVSDK_NGX_Parameter_Color, inputColor);
    params->Set(NVSDK_NGX_Parameter_Output, outputColor);
    params->Set(NVSDK_NGX_Parameter_Depth, depth);
    params->Set(NVSDK_NGX_Parameter_MotionVectors, mvec);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, safeJitterX);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, safeJitterY);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_X, safeMvScaleX);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, safeMvScaleY);
    params->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
    params->Set(NVSDK_NGX_Parameter_Width, renderW);
    params->Set(NVSDK_NGX_Parameter_Height, renderH);

    // Dynamic resolution (DRS) & Subrect coordinates mapping
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, renderW);
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, renderH);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, drs.colorInExt.left);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, drs.colorInExt.top);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, drs.depthExt.left);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, drs.depthExt.top);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, drs.mvecExt.left);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, drs.mvecExt.top);
    bool hasOutputSubrect = (drs.colorOutExt.left > 0 || drs.colorOutExt.top > 0 ||
                             (drs.colorOutExt.width > 0 && drs.colorOutExt.width != targetW) ||
                             (drs.colorOutExt.height > 0 && drs.colorOutExt.height != targetH));
    params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, hasOutputSubrect ? 1 : 0);
    params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, drs.colorOutExt.left);
    params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, drs.colorOutExt.top);

    // Exposure texture
    params->Set(NVSDK_NGX_Parameter_ExposureTexture, extraArgs.exposureTexture ? (void*)extraArgs.exposureTexture : nullptr);

    // Optional masks & hints (always clear slot when not present to prevent dangling pointers across frames)
    params->Set(NVSDK_NGX_Parameter_TransparencyMask, extraArgs.transparencyMask ? (void*)extraArgs.transparencyMask : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X, extraArgs.transparencyMask ? drs.transparencyExt.left : 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y, extraArgs.transparencyMask ? drs.transparencyExt.top : 0u);

    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, extraArgs.biasCurrentColorMask ? (void*)extraArgs.biasCurrentColorMask : nullptr);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, extraArgs.biasCurrentColorMask ? drs.biasExt.left : 0u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, extraArgs.biasCurrentColorMask ? drs.biasExt.top : 0u);

    params->Set(NVSDK_NGX_Parameter_AnimatedTextureMask, extraArgs.animatedTextureMask ? (void*)extraArgs.animatedTextureMask : nullptr);
    params->Set(NVSDK_NGX_Parameter_RayTracingHitDistance, extraArgs.rayTracingHitDistance ? (void*)extraArgs.rayTracingHitDistance : nullptr);
    params->Set(NVSDK_NGX_Parameter_MotionVectorsReflection, extraArgs.motionVectorsReflection ? (void*)extraArgs.motionVectorsReflection : nullptr);
    params->Set(NVSDK_NGX_Parameter_IsParticleMask, extraArgs.particleMask ? (void*)extraArgs.particleMask : nullptr);

    params->Set(NVSDK_NGX_Parameter_Sharpness, safeSharpness);
    params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, safePreExposure);
    params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, safeExposureScale);

    // Indicator Invert Axis
    params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, extraArgs.indicatorInvertAxisX ? 1 : 0);
    params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, extraArgs.indicatorInvertAxisY ? 1 : 0);

    // Presets
    params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, (unsigned int)extraArgs.dlaaPreset);
    params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, (unsigned int)extraArgs.qualityPreset);
    params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, (unsigned int)extraArgs.balancedPreset);
    params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, (unsigned int)extraArgs.performancePreset);
    params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, (unsigned int)extraArgs.ultraPerformancePreset);
    params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, (unsigned int)extraArgs.ultraQualityPreset);

    // Camera Motion Matrix: store in persistent ViewportContext so pointer never points to transient stack frame
    if (extraArgs.hasClipToPrevClip) {
        ctx.clipToPrevClip = extraArgs.clipToPrevClip;
        params->Set(NVSDK_NGX_Parameter_DLSS_CLIP_TO_PREV_CLIP_MATRIX, (void*)&ctx.clipToPrevClip);
    } else {
        params->Set(NVSDK_NGX_Parameter_DLSS_CLIP_TO_PREV_CLIP_MATRIX, (void*)nullptr);
    }

    // Direct dispatch into Apple MetalFX via D3DMetal NVSDK_NGX_D3D12_EvaluateFeature
    // (Zero-barrier design conforming to official Streamline sl.dlss, avoiding Command Encoder breaks)
    const int64_t ngxStart = QpcNow();
    NVSDK_NGX_Result res = m_pfnEvaluateFeature(cmdList, ctx.handle, params, nullptr);
    const uint64_t ngxUs = TicksToUs(QpcNow() - ngxStart);
    m_ngxEvalSumUs += ngxUs;
    m_ngxEvalMaxUs = std::max(m_ngxEvalMaxUs, ngxUs);
    m_ngxEvalCount++;

    if (NVSDK_NGX_FAILED(res)) {
        m_statEvaluateFailures.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("MetalFX NVSDK_NGX_D3D12_EvaluateFeature returned 0x%08X for viewport %u, invoking BlitGuard", res, viewport);
        BlitGuard(cmdList, inputColor, outputColor, inputState, outputState);
        return false;
    }

    m_statEvaluateDispatched.fetch_add(1, std::memory_order_relaxed);
    LOG_DEBUG("MetalFX DLSS evaluation dispatched successfully for viewport %u (frame %llu)", viewport, m_currentFrame);
    return true;
}

void NVNGXRouter::BlitGuard(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* inputColor,
    ID3D12Resource* outputColor,
    D3D12_RESOURCE_STATES inputState,
    D3D12_RESOURCE_STATES outputState)
{
    if (!cmdList || !inputColor || !outputColor || inputColor == outputColor) return;
    m_statBlitFallbacks.fetch_add(1, std::memory_order_relaxed);

    D3D12_RESOURCE_DESC inDesc = inputColor->GetDesc();
    D3D12_RESOURCE_DESC outDesc = outputColor->GetDesc();

    LOG_DEBUG("BlitGuard triggered: copying %llux%u to %llux%u",
              inDesc.Width, inDesc.Height, outDesc.Width, outDesc.Height);

    // 1. Transition barriers
    D3D12_RESOURCE_BARRIER preBarriers[2];
    UINT numPreBarriers = 0;

    if (inputState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        preBarriers[numPreBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        preBarriers[numPreBarriers].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        preBarriers[numPreBarriers].Transition.pResource = inputColor;
        preBarriers[numPreBarriers].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        preBarriers[numPreBarriers].Transition.StateBefore = (inputState == UINT_MAX) ? D3D12_RESOURCE_STATE_COMMON : inputState;
        preBarriers[numPreBarriers].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        numPreBarriers++;
    }

    if (outputState != D3D12_RESOURCE_STATE_COPY_DEST) {
        preBarriers[numPreBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        preBarriers[numPreBarriers].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        preBarriers[numPreBarriers].Transition.pResource = outputColor;
        preBarriers[numPreBarriers].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        preBarriers[numPreBarriers].Transition.StateBefore = (outputState == UINT_MAX) ? D3D12_RESOURCE_STATE_COMMON : outputState;
        preBarriers[numPreBarriers].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        numPreBarriers++;
    }

    if (numPreBarriers > 0) {
        cmdList->ResourceBarrier(numPreBarriers, preBarriers);
    }

    // 2. Perform copy
    if (inDesc.Width == outDesc.Width && inDesc.Height == outDesc.Height) {
        cmdList->CopyResource(outputColor, inputColor);
    } else {
        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = outputColor;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = inputColor;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        D3D12_BOX srcBox{};
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = (UINT)std::min<uint64_t>(inDesc.Width, outDesc.Width);
        srcBox.bottom = (UINT)std::min<UINT>(inDesc.Height, outDesc.Height);
        srcBox.back = 1;

        cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
    }

    // 3. Revert barriers
    D3D12_RESOURCE_BARRIER postBarriers[2];
    UINT numPostBarriers = 0;

    if (inputState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        postBarriers[numPostBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postBarriers[numPostBarriers].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        postBarriers[numPostBarriers].Transition.pResource = inputColor;
        postBarriers[numPostBarriers].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postBarriers[numPostBarriers].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        postBarriers[numPostBarriers].Transition.StateAfter = (inputState == UINT_MAX) ? D3D12_RESOURCE_STATE_COMMON : inputState;
        numPostBarriers++;
    }

    if (outputState != D3D12_RESOURCE_STATE_COPY_DEST) {
        postBarriers[numPostBarriers].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postBarriers[numPostBarriers].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        postBarriers[numPostBarriers].Transition.pResource = outputColor;
        postBarriers[numPostBarriers].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postBarriers[numPostBarriers].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        postBarriers[numPostBarriers].Transition.StateAfter = (outputState == UINT_MAX) ? D3D12_RESOURCE_STATE_COMMON : outputState;
        numPostBarriers++;
    }

    if (numPostBarriers > 0) {
        cmdList->ResourceBarrier(numPostBarriers, postBarriers);
    }
}

void NVNGXRouter::RecordLockWait(int64_t waitStartTicks) {
    const uint64_t waitUs = TicksToUs(QpcNow() - waitStartTicks);
    m_statLockWaitSamples.fetch_add(1, std::memory_order_relaxed);
    m_statLockWaitTotalUs.fetch_add(waitUs, std::memory_order_relaxed);
    AtomicMax(m_statLockWaitMaxUs, waitUs);
}

void NVNGXRouter::MaybeLogPeriodicStats(int64_t nowTicks) {
    if (m_lastSummaryTicks == 0) {
        m_lastSummaryTicks = nowTicks;
        return;
    }
    const uint64_t elapsedUs = TicksToUs(nowTicks - m_lastSummaryTicks);
    if (elapsedUs < kStatsIntervalUs) return;

    StatsSnapshot cur;
    cur.evaluateCalls = m_statEvaluateCalls.load(std::memory_order_relaxed);
    cur.evaluateFailures = m_statEvaluateFailures.load(std::memory_order_relaxed);
    cur.featureCreates = m_statFeatureCreates.load(std::memory_order_relaxed);
    cur.featureRecreates = m_statFeatureRecreates.load(std::memory_order_relaxed);
    cur.blitFallbacks = m_statBlitFallbacks.load(std::memory_order_relaxed);
    cur.lockWaitSamples = m_statLockWaitSamples.load(std::memory_order_relaxed);
    cur.lockWaitTotalUs = m_statLockWaitTotalUs.load(std::memory_order_relaxed);
    const uint64_t lockWaitMaxUs = m_statLockWaitMaxUs.exchange(0, std::memory_order_relaxed);

    if (Logger::Instance().IsEnabled(LogLevel::Info)) {
        const uint64_t evals = cur.evaluateCalls - m_lastSummary.evaluateCalls;
        const uint64_t lockSamples = cur.lockWaitSamples - m_lastSummary.lockWaitSamples;
        const uint64_t lockTotalUs = cur.lockWaitTotalUs - m_lastSummary.lockWaitTotalUs;
        LOG_INFO("[stats %.1fs] evals=%llu (%.1f/s) frame avg=%.2fms max=%.2fms | ngxEval avg=%.0fus max=%lluus | "
                 "creates=%llu recreates=%llu evalFail=%llu blits=%llu | lockWait n=%llu avg=%.1fus max=%lluus | deferred=%zu",
                 elapsedUs / 1e6,
                 evals, evals * 1e6 / (double)elapsedUs,
                 m_frameIntervalCount ? m_frameIntervalSumUs / 1000.0 / m_frameIntervalCount : 0.0,
                 m_frameIntervalMaxUs / 1000.0,
                 m_ngxEvalCount ? (double)m_ngxEvalSumUs / m_ngxEvalCount : 0.0,
                 m_ngxEvalMaxUs,
                 cur.featureCreates - m_lastSummary.featureCreates,
                 cur.featureRecreates - m_lastSummary.featureRecreates,
                 cur.evaluateFailures - m_lastSummary.evaluateFailures,
                 cur.blitFallbacks - m_lastSummary.blitFallbacks,
                 lockSamples,
                 lockSamples ? (double)lockTotalUs / lockSamples : 0.0,
                 lockWaitMaxUs,
                 m_deferredReleases.size());
    }

    m_lastSummary = cur;
    m_lastSummaryTicks = nowTicks;
    m_frameIntervalSumUs = 0;
    m_frameIntervalMaxUs = 0;
    m_frameIntervalCount = 0;
    m_ngxEvalSumUs = 0;
    m_ngxEvalMaxUs = 0;
    m_ngxEvalCount = 0;
}

void NVNGXRouter::GetDiagnostics(MetalStreamDiagnostics& out) {
    out.evaluateCalls = m_statEvaluateCalls.load(std::memory_order_relaxed);
    out.evaluateDispatched = m_statEvaluateDispatched.load(std::memory_order_relaxed);
    out.evaluateFailures = m_statEvaluateFailures.load(std::memory_order_relaxed);
    out.featureCreates = m_statFeatureCreates.load(std::memory_order_relaxed);
    out.featureCreateFailures = m_statFeatureCreateFailures.load(std::memory_order_relaxed);
    out.featureRecreates = m_statFeatureRecreates.load(std::memory_order_relaxed);
    out.blitFallbacks = m_statBlitFallbacks.load(std::memory_order_relaxed);
    out.lockWaitSamples = m_statLockWaitSamples.load(std::memory_order_relaxed);
    out.lockWaitTotalUs = m_statLockWaitTotalUs.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mutex);
    out.deferredReleasesPending = m_deferredReleases.size();
}

} // namespace metalstream

extern "C" __declspec(dllexport) int MetalStream_GetDiagnostics(MetalStreamDiagnostics* out) {
    if (!out || out->structSize < sizeof(MetalStreamDiagnostics)) return 0;
    const uint32_t size = out->structSize;
    *out = {};
    out->structSize = size;
    metalstream::NVNGXRouter::Instance().GetDiagnostics(*out);
    return 1;
}
