#define SL_INTERPOSER 1

#include "../common/logger.h"
#include "../common/config.h"
#include "nvngx_router.h"
#include "framegen.h"
#include <streamline/sl.h>
#include <streamline/sl_dlss.h>
#include <streamline/sl_dlss_g.h>
#include <streamline/sl_reflex.h>
#include <streamline/sl_pcl.h>
#include <streamline/sl_helpers.h>

#include <unordered_map>
#include <vector>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cmath>
#include <d3d12.h>
#include <d3d11.h>

namespace sl {
SL_STRUCT_BEGIN(VulkanInfo, StructType({ 0xeed6fd5, 0x82cd, 0x43a9, { 0xbd, 0xb5, 0x47, 0xa5, 0xba, 0x2f, 0x45, 0xd6 } }), kStructVersion3)
    void* device{};
    void* instance{};
    void* physicalDevice{};
    uint32_t computeQueueIndex{};
    uint32_t computeQueueFamily{};
    uint32_t graphicsQueueIndex{};
    uint32_t graphicsQueueFamily{};
    uint32_t opticalFlowQueueIndex{};
    uint32_t opticalFlowQueueFamily{};
    bool useNativeOpticalFlowMode = false;
    uint32_t computeQueueCreateFlags{};
    uint32_t graphicsQueueCreateFlags{};
    uint32_t opticalFlowQueueCreateFlags{};
SL_STRUCT_END()
}

SL_API sl::Result slSetVulkanInfo(const sl::VulkanInfo& info);

namespace metalstream {

static inline bool IsValidFiniteFloat(float v) {
    if (std::isnan(v) || std::isinf(v)) return false;
    if (v >= 3.0e38f || v <= -3.0e38f) return false;
    return true;
}

struct FrameTokenImpl : public sl::FrameToken {
    std::atomic<uint32_t> counter{0};
    FrameTokenImpl(uint32_t idx = 0) : FrameToken(), counter(idx) {}
    operator uint32_t() const override { return counter.load(); }
};

struct TaggedResourceEntry {
    sl::Resource resource{};
    sl::Extent extent{};
    sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eValidUntilPresent;
    bool preservedCopy = false; // resource.native is MetalStream's copy of an eOnlyValidNow tag
};

struct FrameResourceContainer {
    uint32_t frameIndex = UINT_MAX;
    std::unordered_map<uint32_t, std::unordered_map<sl::BufferType, TaggedResourceEntry>> taggedResources;
    std::unordered_map<uint32_t, sl::Constants> viewportConstants;
};

static inline sl::float4x4 MakeIdentityMatrix() {
    sl::float4x4 m;
    m[0] = sl::float4(1.0f, 0.0f, 0.0f, 0.0f);
    m[1] = sl::float4(0.0f, 1.0f, 0.0f, 0.0f);
    m[2] = sl::float4(0.0f, 0.0f, 1.0f, 0.0f);
    m[3] = sl::float4(0.0f, 0.0f, 0.0f, 1.0f);
    return m;
}

class StreamlineContext {
public:
    static StreamlineContext& Instance() {
        static StreamlineContext s_instance;
        return s_instance;
    }

    bool initialized = false;
    sl::Preferences preferences{};

    // Multi-frame token ring buffer (conforms to sl::MAX_FRAMES_IN_FLIGHT)
    std::mutex mtxFrameHandle;
    uint32_t frameCounter = 0;
    uint32_t frameHandleIndex = 0;
    FrameTokenImpl frameHandles[sl::MAX_FRAMES_IN_FLIGHT];

    // Frame-aware resource tagging container ring buffer
    static constexpr size_t NUM_FRAME_SLOTS = 32;
    FrameResourceContainer frameSlots[NUM_FRAME_SLOTS];

    ID3D12Device* d3d12Device = nullptr;
    ID3D12Fence* m_dummyFence = nullptr;

    std::unordered_map<sl::Feature, bool> loadedFeatures;
    std::unordered_map<uint32_t, sl::Constants> viewportConstants;
    std::unordered_map<uint32_t, sl::DLSSOptions> dlssOptions;
    std::unordered_map<uint32_t, sl::DLSSGOptions> dlssgOptions;
    std::unordered_map<uint32_t, sl::ReflexCameraData> reflexCameraData;
    sl::ReflexOptions reflexOptions{};
    sl::PCLOptions pclOptions{};

    // Fallback unversioned resource tags per viewport: viewport -> (bufferType -> entry)
    std::unordered_map<uint32_t, std::unordered_map<sl::BufferType, TaggedResourceEntry>> taggedResources;
    std::unordered_map<uint32_t, uint32_t> lastResetFrame;
    // Monotonic counter bumped on every slSetTag/slSetTagForFrame; tells which viewport is live
    uint64_t tagSequence = 0;
    std::unordered_map<uint32_t, uint64_t> viewportTagSequence;

    std::recursive_mutex stateMutex;
};

void SetInternalD3D12Device(void* d3dDevice) {
    if (!d3dDevice) return;
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    auto* unk = static_cast<IUnknown*>(d3dDevice);
    ID3D12Device* dev12 = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&dev12))) && dev12) {
        ctx.d3d12Device = dev12;
        if (ctx.initialized) {
            NVNGXRouter::Instance().Init(dev12, ctx.preferences.applicationId);
        }
        dev12->Release();
    }
}

// Caller holds ctx.stateMutex
static bool IsFrameGenRequested(StreamlineContext& ctx, uint32_t* outViewport = nullptr) {
    auto loadedIt = ctx.loadedFeatures.find(sl::kFeatureDLSS_G);
    if (loadedIt == ctx.loadedFeatures.end() || !loadedIt->second) return false;
    for (const auto& kv : ctx.dlssgOptions) {
        if (kv.second.mode == sl::DLSSGMode::eOn || kv.second.mode == sl::DLSSGMode::eAuto) {
            if (outViewport) *outViewport = kv.first;
            return true;
        }
    }
    return false;
}

// Caller holds ctx.stateMutex. Builds the stored tag entry; for eOnlyValidNow frame-generation
// inputs, copies the resource so it survives until Present.
static TaggedResourceEntry MakeTagEntry(StreamlineContext& ctx, const sl::ResourceTag* tag, sl::CommandBuffer* cmdBuffer, uint32_t viewport) {
    TaggedResourceEntry entry;
    entry.resource = *tag->resource;
    entry.extent = tag->extent;
    entry.lifecycle = tag->lifecycle;
    const bool fgInput = tag->type == sl::kBufferTypeHUDLessColor || tag->type == sl::kBufferTypeDepth ||
                         tag->type == sl::kBufferTypeMotionVectors;
    if (fgInput && tag->lifecycle == sl::ResourceLifecycle::eOnlyValidNow && cmdBuffer &&
        tag->resource->native && IsFrameGenRequested(ctx)) {
        D3D12_RESOURCE_STATES copyState{};
        ID3D12Resource* copy = framegen::PreserveVolatileTag(
            static_cast<ID3D12GraphicsCommandList*>(cmdBuffer), tag->type,
            static_cast<ID3D12Resource*>(tag->resource->native),
            static_cast<D3D12_RESOURCE_STATES>(tag->resource->state), &copyState);
        if (copy) {
            entry.resource.native = copy;
            entry.resource.state = copyState;
            entry.preservedCopy = true;
        }
    }
    return entry;
}

// A null tag (no resource, or no native resource except for the backbuffer, whose extent alone is
// meaningful) means the game's inputs are invalid (loading, menus, cutscenes) or the resource is about to
// be destroyed (ProgrammingGuide "set the tag to null in order to release the reference"): forget it.
static bool IsNullTag(const sl::ResourceTag* tag) {
    return !tag->resource || (!tag->resource->native && tag->type != sl::kBufferTypeBackbuffer);
}

namespace framegen {

// Caller holds ctx.stateMutex. Periodic dump of what the game hands us for frame generation.
static void LogFrameGenInputs(StreamlineContext& ctx, const Inputs& in) {
    static int64_t s_lastTicks = 0;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (s_lastTicks && now.QuadPart - s_lastTicks < freq.QuadPart * 10) return;
    s_lastTicks = now.QuadPart;

    LOG_INFO("framegen inputs: chosen viewport %u (tag seq %llu, global seq %llu)", in.viewport,
             (unsigned long long)ctx.viewportTagSequence[in.viewport], (unsigned long long)ctx.tagSequence);
    for (const auto& vp : ctx.taggedResources) {
        auto optIt = ctx.dlssgOptions.find(vp.first);
        LOG_INFO("  viewport %u: dlssg mode=%d tagSeq=%llu", vp.first,
                 optIt != ctx.dlssgOptions.end() ? (int)optIt->second.mode : -1,
                 (unsigned long long)ctx.viewportTagSequence[vp.first]);
        for (const auto& tag : vp.second) {
            auto* res = static_cast<ID3D12Resource*>(tag.second.resource.native);
            // Only the chosen viewport's tags are known to be alive; a stale viewport may hold resources
            // the game has already freed, so never call into those
            const bool alive = res && vp.first == in.viewport &&
                               (!tag.second.preservedCopy || IsPreservedCopyAlive(res));
            D3D12_RESOURCE_DESC d = alive ? res->GetDesc() : D3D12_RESOURCE_DESC{};
            LOG_INFO("    tag %u: %p %llux%u fmt=%d state=0x%X lifecycle=%d extent=(%u,%u %ux%u)",
                     tag.first, res, (unsigned long long)d.Width, d.Height, (int)d.Format, tag.second.resource.state,
                     (int)tag.second.lifecycle, tag.second.extent.left, tag.second.extent.top,
                     tag.second.extent.width, tag.second.extent.height);
        }
    }
    if (in.hasConstants) {
        const auto& c = in.constants;
        LOG_INFO("  constants: mvecScale=(%.5f,%.5f) jitter=(%.4f,%.4f) depthInv=%d mvecJittered=%d camMotion=%d mvec3D=%d ortho=%d "
                 "near=%.4f far=%.1f fov=%.4f aspect=%.4f fwd=(%.3f,%.3f,%.3f) pos=(%.2f,%.2f,%.2f)",
                 c.mvecScale.x, c.mvecScale.y, c.jitterOffset.x, c.jitterOffset.y, (int)c.depthInverted,
                 (int)c.motionVectorsJittered, (int)c.cameraMotionIncluded, (int)c.motionVectors3D, (int)c.orthographicProjection,
                 c.cameraNear, c.cameraFar, c.cameraFOV, c.cameraAspectRatio,
                 c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z, c.cameraPos.x, c.cameraPos.y, c.cameraPos.z);
    } else {
        LOG_INFO("  constants: none for viewport %u", in.viewport);
    }
}

bool CollectInputs(Inputs& out) {
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    out = {};
    if (!ctx.initialized || !IsFrameGenRequested(ctx)) return false;
    out.requested = true;

    // Pick the DLSS-G viewport that was tagged most recently: stale viewports (e.g. after the game
    // switched from viewport 2 to 3) keep old, possibly freed, resources around
    uint64_t bestSeq = 0;
    bool found = false;
    for (const auto& kv : ctx.dlssgOptions) {
        if (kv.second.mode != sl::DLSSGMode::eOn && kv.second.mode != sl::DLSSGMode::eAuto) continue;
        auto seqIt = ctx.viewportTagSequence.find(kv.first);
        uint64_t seq = seqIt != ctx.viewportTagSequence.end() ? seqIt->second : 0;
        if (!found || seq > bestSeq) {
            found = true;
            bestSeq = seq;
            out.viewport = kv.first;
        }
    }
    if (bestSeq == 0) {
        // DLSS-G options and tags use different viewport ids: fall back to the most recently tagged viewport
        for (const auto& kv : ctx.viewportTagSequence) {
            if (kv.second > bestSeq) {
                bestSeq = kv.second;
                out.viewport = kv.first;
            }
        }
    }

    auto vpIt = ctx.taggedResources.find(out.viewport);
    if (vpIt != ctx.taggedResources.end()) {
        auto fill = [&](sl::BufferType type, TagInput& dst) {
            auto it = vpIt->second.find(type);
            if (it == vpIt->second.end() || !it->second.resource.native) return;
            // A copy evicted since this tag was stored (the game stopped tagging this resource)
            if (it->second.preservedCopy &&
                !framegen::IsPreservedCopyAlive(static_cast<ID3D12Resource*>(it->second.resource.native))) return;
            dst.resource = static_cast<ID3D12Resource*>(it->second.resource.native);
            dst.state = static_cast<D3D12_RESOURCE_STATES>(it->second.resource.state);
            dst.extent = it->second.extent;
        };
        fill(sl::kBufferTypeHUDLessColor, out.hudless);
        fill(sl::kBufferTypeDepth, out.depth);
        if (!out.depth.resource) fill(sl::kBufferTypeLinearDepth, out.depth);
        fill(sl::kBufferTypeMotionVectors, out.mvecs);
    }

    auto cIt = ctx.viewportConstants.find(out.viewport);
    if (cIt != ctx.viewportConstants.end()) {
        out.hasConstants = true;
        out.constants = cIt->second;
    }
    LogFrameGenInputs(ctx, out);
    return true;
}

} // namespace framegen

} // namespace metalstream

using namespace metalstream;

// -----------------------------------------------------------------------------
// Unified Streamline State & Feature Validation Macros
// Aligned with official Streamline slValidateState() and slValidateFeatureContext()
// -----------------------------------------------------------------------------

#define SL_REQUIRE_INITIALIZED() \
    do { \
        if (!ctx.initialized) { \
            LOG_ERROR("%s: Streamline not initialized", __FUNCTION__); \
            return sl::Result::eErrorNotInitialized; \
        } \
    } while (0)

#define SL_VALIDATE_FEATURE(f) \
    do { \
        if ((f) != sl::kFeatureDLSS && (f) != sl::kFeatureDLSS_G && \
            (f) != sl::kFeatureReflex && (f) != sl::kFeaturePCL) { \
            LOG_WARN("%s: Feature %u is missing or not supported", __FUNCTION__, (uint32_t)(f)); \
            return sl::Result::eErrorFeatureMissing; \
        } \
    } while (0)

// -----------------------------------------------------------------------------
// Feature function implementations (exposed via slGetFeatureFunction & DLL export)
// -----------------------------------------------------------------------------

extern "C" {

SL_API sl::Result impl_slDLSSGetOptimalSettings(const sl::DLSSOptions& options, sl::DLSSOptimalSettings& settings) {
    // 1. Try MetalFX nvngx callback
    if (NVNGXRouter::Instance().GetOptimalSettings(options, settings)) {
        return sl::Result::eOk;
    }

    // 2. Fallback to standard mathematical optimal settings calculation
    float scale = 1.0f;
    switch (options.mode) {
        case sl::DLSSMode::eMaxPerformance:     scale = 0.50f;     break; // 2.0x
        case sl::DLSSMode::eBalanced:           scale = 0.58f;     break; // 1.72x
        case sl::DLSSMode::eMaxQuality:         scale = 0.666667f; break; // 1.5x
        case sl::DLSSMode::eUltraPerformance:   scale = 0.333333f; break; // 3.0x
        case sl::DLSSMode::eUltraQuality:       scale = 0.77f;     break;
        case sl::DLSSMode::eDLAA:
        case sl::DLSSMode::eOff:
        default:                                scale = 1.0f;      break;
    }

    uint32_t rw = (uint32_t)(options.outputWidth * scale + 0.5f);
    uint32_t rh = (uint32_t)(options.outputHeight * scale + 0.5f);
    rw = (rw + 1) & ~1u;
    rh = (rh + 1) & ~1u;
    if (rw == 0) rw = 2;
    if (rh == 0) rh = 2;

    settings.optimalRenderWidth = rw;
    settings.optimalRenderHeight = rh;
    settings.optimalSharpness = 0.0f;
    settings.renderWidthMin = rw;
    settings.renderHeightMin = rh;
    settings.renderWidthMax = options.outputWidth;
    settings.renderHeightMax = options.outputHeight;

    LOG_DEBUG("slDLSSGetOptimalSettings: mode=%u, out=%ux%u -> render=%ux%u",
             (uint32_t)options.mode, options.outputWidth, options.outputHeight, rw, rh);
    return sl::Result::eOk;
}

SL_API sl::Result impl_slDLSSGetState(const sl::ViewportHandle& viewport, sl::DLSSState& state) {
    LOG_DEBUG("slDLSSGetState called for viewport %u", (uint32_t)viewport);
    sl::BaseStructure* savedNext = state.next;
    state = {};
    state.next = savedNext;
    state.estimatedVRAMUsageInBytes = 87 * 1024 * 1024;
    return sl::Result::eOk;
}

SL_API sl::Result impl_slDLSSSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options) {
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    // Games commonly call this every frame; only log at Info level when something relevant changed
    auto prevIt = ctx.dlssOptions.find((uint32_t)viewport);
    const bool changed = prevIt == ctx.dlssOptions.end() ||
                         prevIt->second.mode != options.mode ||
                         prevIt->second.outputWidth != options.outputWidth ||
                         prevIt->second.outputHeight != options.outputHeight ||
                         prevIt->second.sharpness != options.sharpness ||
                         prevIt->second.useAutoExposure != options.useAutoExposure ||
                         prevIt->second.colorBuffersHDR != options.colorBuffersHDR;
    if (changed) {
        LOG_INFO("slDLSSSetOptions: viewport=%u, mode=%u, out=%ux%u, sharp=%.2f, autoExp=%d, hdr=%d",
                 (uint32_t)viewport, (uint32_t)options.mode, options.outputWidth, options.outputHeight,
                 options.sharpness, (int)options.useAutoExposure, (int)options.colorBuffersHDR);
    }
    ctx.dlssOptions[(uint32_t)viewport] = options;
    return sl::Result::eOk;
}

SL_API sl::Result impl_slDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state, const sl::DLSSGOptions* options) {
    LOG_DEBUG("slDLSSGGetState called for viewport %u", (uint32_t)viewport);
    sl::BaseStructure* savedNext = state.next;
    state = {}; // Zero entire structure to eliminate dirty memory
    state.next = savedNext; // Preserve caller's chained list pointer
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);

    if (options) {
        ctx.dlssgOptions[(uint32_t)viewport] = *options;
    }

    if (!ctx.m_dummyFence) {
        ID3D12Device* dev = ctx.d3d12Device ? ctx.d3d12Device : NVNGXRouter::Instance().GetDevice();
        if (dev) {
            ctx.d3d12Device = dev;
            HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx.m_dummyFence));
            if (SUCCEEDED(hr)) {
                LOG_INFO("impl_slDLSSGGetState: Created DLSS-G dummy fence %p", ctx.m_dummyFence);
            } else {
                LOG_WARN("impl_slDLSSGGetState: Failed to create dummy fence, hr=0x%08x", (uint32_t)hr);
            }
        }
    }

    state.status = sl::DLSSGStatus::eOk;
    state.minWidthOrHeight = 64;
    state.numFramesActuallyPresented = framegen::FramesPresentedPerGameFrame();
    state.numFramesToGenerateMax = 1;
    state.estimatedVRAMUsageInBytes = 120 * 1024 * 1024;
    state.bReserved4 = sl::Boolean::eFalse;
    state.bIsVsyncSupportAvailable = sl::Boolean::eTrue;
    state.bIsDynamicMFGSupported = sl::Boolean::eFalse;

    // Safety net: assign real D3D12 dummy fence so GPU queue waits never crash on null pointer
    state.inputsProcessingCompletionFence = ctx.m_dummyFence;
    state.lastPresentInputsProcessingCompletionFenceValue = 0;
    return sl::Result::eOk;
}

SL_API sl::Result impl_slDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    auto prevIt = ctx.dlssgOptions.find((uint32_t)viewport);
    if (prevIt == ctx.dlssgOptions.end() || prevIt->second.mode != options.mode ||
        prevIt->second.numFramesToGenerate != options.numFramesToGenerate) {
        LOG_INFO("slDLSSGSetOptions: viewport=%u, mode=%u, numFramesToGenerate=%u",
                 (uint32_t)viewport, (uint32_t)options.mode, options.numFramesToGenerate);
    }
    ctx.dlssgOptions[(uint32_t)viewport] = options;
    return sl::Result::eOk;
}

SL_API sl::Result impl_slReflexSleep(const sl::FrameToken& frame) {
    // Non-blocking no-op to ensure high framerate on macOS / Wine
    return sl::Result::eOk;
}

SL_API sl::Result impl_slReflexGetState(sl::ReflexState& state) {
    LOG_DEBUG("slReflexGetState called");
    sl::BaseStructure* savedNext = state.next;
    state = {};
    state.next = savedNext;
    state.lowLatencyAvailable = true;
    state.latencyReportAvailable = false;
    state.flashIndicatorDriverControlled = false;
    state.statsWindowMessage = 0;
    return sl::Result::eOk;
}

SL_API sl::Result impl_slReflexSetOptions(const sl::ReflexOptions& options) {
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    // frameLimitUs matters for diagnosing frame pacing (Reflex sleep is a no-op here), so log every change
    static bool s_loggedOnce = false;
    if (!s_loggedOnce || ctx.reflexOptions.mode != options.mode || ctx.reflexOptions.frameLimitUs != options.frameLimitUs) {
        s_loggedOnce = true;
        LOG_INFO("slReflexSetOptions: mode=%u, frameLimitUs=%u",
                 (uint32_t)options.mode, options.frameLimitUs);
    }
    ctx.reflexOptions = options;
    return sl::Result::eOk;
}

SL_API sl::Result impl_slReflexSetCameraData(const sl::ViewportHandle& viewport, const sl::FrameToken& frame, const sl::ReflexCameraData& inCameraData) {
    LOG_DEBUG("slReflexSetCameraData called for viewport %u", (uint32_t)viewport);
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    ctx.reflexCameraData[(uint32_t)viewport] = inCameraData;
    return sl::Result::eOk;
}

SL_API sl::Result impl_slReflexGetPredictedCameraData(const sl::ViewportHandle& viewport, const sl::FrameToken& frame, sl::ReflexPredictedCameraData& outCameraData) {
    LOG_DEBUG("slReflexGetPredictedCameraData called for viewport %u", (uint32_t)viewport);
    sl::BaseStructure* savedNext = outCameraData.next;
    outCameraData = {}; // Clear output structure
    outCameraData.next = savedNext;
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);

    auto IsValidMatrix = [](const sl::float4x4& m) -> bool {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float val = 0.0f;
                if (c == 0) val = m[r].x;
                else if (c == 1) val = m[r].y;
                else if (c == 2) val = m[r].z;
                else if (c == 3) val = m[r].w;
                if (std::isnan(val) || std::isinf(val) || val >= 3.0e38f || val <= -3.0e38f) {
                    return false;
                }
            }
        }
        bool isZero = (m[0].x == 0.0f && m[1].y == 0.0f && m[2].z == 0.0f && m[3].w == 0.0f);
        return !isZero;
    };

    auto it = ctx.reflexCameraData.find((uint32_t)viewport);
    if (it != ctx.reflexCameraData.end()) {
        const auto& cam = it->second;
        outCameraData.predictedWorldToViewMatrix = IsValidMatrix(cam.worldToViewMatrix) ? cam.worldToViewMatrix : MakeIdentityMatrix();
        outCameraData.predictedViewToClipMatrix  = IsValidMatrix(cam.viewToClipMatrix)  ? cam.viewToClipMatrix  : MakeIdentityMatrix();
    } else {
        outCameraData.predictedWorldToViewMatrix = MakeIdentityMatrix();
        outCameraData.predictedViewToClipMatrix  = MakeIdentityMatrix();
    }
    return sl::Result::eOk;
}

// Forward PCL/Reflex markers to GPTk's nvapi64.dll (NvAPI_D3D_SetLatencyMarker). D3DMetal's
// FrameGeneration derives the interpolator's deltaTime from the last two eSimulationStart markers
// and falls back to a fixed 1/30s without them.
namespace {
struct NvLatencyMarkerParams {
    uint32_t version;
    uint64_t frameID;
    uint32_t markerType;
    uint8_t rsvd[64];
};
typedef void* (__cdecl* PFN_nvapi_QueryInterface)(uint32_t id);
typedef int (__cdecl* PFN_NvAPI_Initialize)();
typedef int (__cdecl* PFN_NvAPI_D3D_SetLatencyMarker)(IUnknown* device, NvLatencyMarkerParams* params);

PFN_NvAPI_D3D_SetLatencyMarker ResolveSetLatencyMarker() {
    static PFN_NvAPI_D3D_SetLatencyMarker s_fn = [] () -> PFN_NvAPI_D3D_SetLatencyMarker {
        HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
        auto query = nvapi ? (PFN_nvapi_QueryInterface)GetProcAddress(nvapi, "nvapi_QueryInterface") : nullptr;
        if (!query) {
            LOG_WARN("PCL markers: nvapi64.dll / nvapi_QueryInterface unavailable; markers not forwarded");
            return nullptr;
        }
        auto init = (PFN_NvAPI_Initialize)query(0x0150E828);
        int initStatus = init ? init() : -1;
        auto fn = (PFN_NvAPI_D3D_SetLatencyMarker)query(0xD9984C05);
        LOG_INFO("PCL markers: NvAPI_Initialize=%d, NvAPI_D3D_SetLatencyMarker=%p", initStatus, (void*)fn);
        return fn;
    }();
    return s_fn;
}
} // namespace

SL_API sl::Result impl_slPCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame) {
    // Invoked several times per frame by Unity: keep it cheap
    if ((uint32_t)marker > (uint32_t)sl::PCLMarker::eOutOfBandPresentEnd) return sl::Result::eOk;
    ID3D12Device* device = StreamlineContext::Instance().d3d12Device;
    auto setMarker = device ? ResolveSetLatencyMarker() : nullptr;
    if (!setMarker) return sl::Result::eOk;
    NvLatencyMarkerParams params{};
    params.version = (uint32_t)sizeof(NvLatencyMarkerParams) | (1u << 16);
    params.frameID = (uint32_t)frame;
    params.markerType = (uint32_t)marker;
    int status = setMarker(device, &params);
    static std::atomic<int> s_logged{0};
    if (status != 0 && s_logged.fetch_add(1) < 3) {
        LOG_WARN("PCL markers: NvAPI_D3D_SetLatencyMarker(type=%u) returned %d", (uint32_t)marker, status);
    }
    return sl::Result::eOk;
}

SL_API sl::Result impl_slReflexSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame) {
    return impl_slPCLSetMarker(marker, frame);
}

SL_API sl::Result slReflexSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame) {
    return impl_slPCLSetMarker(marker, frame);
}

SL_API sl::Result impl_slPCLGetState(sl::PCLState& state) {
    sl::BaseStructure* savedNext = state.next;
    state = {};
    state.next = savedNext;
    state.statsWindowMessage = 0;
    return sl::Result::eOk;
}

SL_API sl::Result impl_slPCLSetOptions(const sl::PCLOptions& options) {
    return sl::Result::eOk;
}

SL_API sl::Result impl_slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport) {
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    if (!ctx.initialized) {
        LOG_ERROR("slAllocateResources: Streamline not initialized");
        return sl::Result::eErrorNotInitialized;
    }
    if (feature != sl::kFeatureDLSS && feature != sl::kFeatureDLSS_G &&
        feature != sl::kFeatureReflex && feature != sl::kFeaturePCL)
    {
        LOG_ERROR("slAllocateResources: unsupported or missing feature %u", feature);
        return sl::Result::eErrorFeatureMissing;
    }
    if (feature == sl::kFeatureReflex || feature == sl::kFeaturePCL) {
        LOG_WARN("slAllocateResources: feature %u does not support explicit resource allocation", feature);
        return sl::Result::eErrorMissingOrInvalidAPI;
    }

    LOG_INFO("slAllocateResources: feature=%u, viewport=%u", feature, (uint32_t)viewport);
    if (feature == sl::kFeatureDLSS && cmdBuffer) {
        uint32_t vp = (uint32_t)viewport;
        auto optIt = ctx.dlssOptions.find(vp);
        if (optIt != ctx.dlssOptions.end()) {
            uint32_t outW = optIt->second.outputWidth;
            uint32_t outH = optIt->second.outputHeight;
            if (outW > 0 && outH > 0) {
                sl::DLSSOptimalSettings optSettings{};
                impl_slDLSSGetOptimalSettings(optIt->second, optSettings);
                uint32_t renW = optSettings.optimalRenderWidth;
                uint32_t renH = optSettings.optimalRenderHeight;
                auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(cmdBuffer);
                NVNGXRouter::Instance().EnsureFeatureCreated(vp, cmdList, renW, renH, outW, outH, optIt->second.mode);
            }
        }
    }
    return sl::Result::eOk;
}

SL_API sl::Result impl_slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport) {
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    if (!ctx.initialized) {
        LOG_ERROR("slFreeResources: Streamline not initialized");
        return sl::Result::eErrorNotInitialized;
    }
    if (feature != sl::kFeatureDLSS && feature != sl::kFeatureDLSS_G &&
        feature != sl::kFeatureReflex && feature != sl::kFeaturePCL)
    {
        LOG_ERROR("slFreeResources: unsupported or missing feature %u", feature);
        return sl::Result::eErrorFeatureMissing;
    }
    if (feature == sl::kFeatureReflex || feature == sl::kFeaturePCL) {
        LOG_WARN("slFreeResources: feature %u does not support explicit resource deallocation", feature);
        return sl::Result::eErrorMissingOrInvalidAPI;
    }

    LOG_INFO("slFreeResources: feature=%u, viewport=%u", feature, (uint32_t)viewport);
    uint32_t vp = (uint32_t)viewport;

    if (feature == sl::kFeatureDLSS) {
        NVNGXRouter::Instance().ReleaseViewport(vp);
    }

    ctx.taggedResources.erase(vp);
    ctx.viewportTagSequence.erase(vp);
    ctx.viewportConstants.erase(vp);
    ctx.lastResetFrame.erase(vp);
    ctx.dlssOptions.erase(vp);
    ctx.dlssgOptions.erase(vp);
    ctx.reflexCameraData.erase(vp);

    for (size_t i = 0; i < StreamlineContext::NUM_FRAME_SLOTS; ++i) {
        ctx.frameSlots[i].taggedResources.erase(vp);
        ctx.frameSlots[i].viewportConstants.erase(vp);
    }

    return sl::Result::eOk;
}

SL_API sl::Result impl_slSetData(const sl::BaseStructure* inputs, sl::CommandBuffer* cmdBuffer) {
    if (!inputs) return sl::Result::eErrorMissingInputParameter;

    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();

    // Pass 1: Resolve targetViewport across the entire input linked list first
    uint32_t targetViewport = 0;
    const sl::BaseStructure* curr = inputs;
    while (curr) {
        if (curr->structType == sl::ViewportHandle::s_structType) {
            targetViewport = *static_cast<const sl::ViewportHandle*>(curr);
            break;
        }
        curr = curr->next;
    }

    // Pass 2: Unpack data under the resolved targetViewport
    curr = inputs;
    while (curr) {
        if (curr->structType == sl::DLSSOptions::s_structType) {
            const auto* opts = static_cast<const sl::DLSSOptions*>(curr);
            impl_slDLSSSetOptions(sl::ViewportHandle(targetViewport), *opts);
        } else if (curr->structType == sl::DLSSGOptions::s_structType) {
            const auto* gopts = static_cast<const sl::DLSSGOptions*>(curr);
            ctx.dlssgOptions[targetViewport] = *gopts;
        } else if (curr->structType == sl::ReflexOptions::s_structType) {
            const auto* ropts = static_cast<const sl::ReflexOptions*>(curr);
            ctx.reflexOptions = *ropts;
        } else if (curr->structType == sl::ReflexCameraData::s_structType) {
            const auto* cam = static_cast<const sl::ReflexCameraData*>(curr);
            ctx.reflexCameraData[targetViewport] = *cam;
        } else if (curr->structType == sl::PCLOptions::s_structType) {
            const auto* popts = static_cast<const sl::PCLOptions*>(curr);
            ctx.pclOptions = *popts;
        } else if (curr->structType == sl::Constants::s_structType) {
            const auto* consts = static_cast<const sl::Constants*>(curr);
            ctx.viewportConstants[targetViewport] = *consts;
        } else if (curr->structType == sl::ResourceTag::s_structType) {
            const auto* tag = static_cast<const sl::ResourceTag*>(curr);
            if (IsNullTag(tag)) {
                ctx.taggedResources[targetViewport].erase(tag->type);
            } else {
                TaggedResourceEntry entry;
                entry.resource = *tag->resource;
                entry.extent = tag->extent;
                ctx.taggedResources[targetViewport][tag->type] = entry;
            }
        }
        curr = curr->next;
    }
    return sl::Result::eOk;
}

SL_API sl::Result impl_slGetData(const sl::BaseStructure* inputs, sl::BaseStructure* output, sl::CommandBuffer* cmdBuffer) {
    if (!output) return sl::Result::eErrorMissingInputParameter;

    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();

    uint32_t targetViewport = 0;
    const sl::DLSSOptions* opts = nullptr;
    const sl::DLSSGOptions* gopts = nullptr;

    // Pass 1: Resolve targetViewport first
    const sl::BaseStructure* currIn = inputs;
    while (currIn) {
        if (currIn->structType == sl::ViewportHandle::s_structType) {
            targetViewport = *static_cast<const sl::ViewportHandle*>(currIn);
            break;
        }
        currIn = currIn->next;
    }

    // Pass 2: Find opts & gopts
    currIn = inputs;
    while (currIn) {
        if (currIn->structType == sl::DLSSOptions::s_structType) {
            opts = static_cast<const sl::DLSSOptions*>(currIn);
        } else if (currIn->structType == sl::DLSSGOptions::s_structType) {
            gopts = static_cast<const sl::DLSSGOptions*>(currIn);
        }
        currIn = currIn->next;
    }

    if (!opts) {
        auto optIt = ctx.dlssOptions.find(targetViewport);
        if (optIt != ctx.dlssOptions.end()) {
            opts = &optIt->second;
        }
    }
    if (!gopts) {
        auto gIt = ctx.dlssgOptions.find(targetViewport);
        if (gIt != ctx.dlssgOptions.end()) {
            gopts = &gIt->second;
        }
    }

    bool resolvedAny = false;
    sl::BaseStructure* currOut = output;
    while (currOut) {
        sl::BaseStructure* nextOut = currOut->next;
        if (currOut->structType == sl::DLSSOptimalSettings::s_structType) {
            auto* settings = static_cast<sl::DLSSOptimalSettings*>(currOut);
            if (opts) {
                impl_slDLSSGetOptimalSettings(*opts, *settings);
            }
            resolvedAny = true;
        } else if (currOut->structType == sl::DLSSState::s_structType) {
            auto* state = static_cast<sl::DLSSState*>(currOut);
            impl_slDLSSGetState(sl::ViewportHandle(targetViewport), *state);
            resolvedAny = true;
        } else if (currOut->structType == sl::DLSSGState::s_structType) {
            auto* gstate = static_cast<sl::DLSSGState*>(currOut);
            impl_slDLSSGGetState(sl::ViewportHandle(targetViewport), *gstate, gopts);
            resolvedAny = true;
        } else if (currOut->structType == sl::ReflexState::s_structType) {
            auto* rstate = static_cast<sl::ReflexState*>(currOut);
            impl_slReflexGetState(*rstate);
            resolvedAny = true;
        } else if (currOut->structType == sl::PCLState::s_structType) {
            auto* pstate = static_cast<sl::PCLState*>(currOut);
            impl_slPCLGetState(*pstate);
            resolvedAny = true;
        } else if (currOut->structType == sl::ReflexPredictedCameraData::s_structType) {
            auto* predCam = static_cast<sl::ReflexPredictedCameraData*>(currOut);
            FrameTokenImpl dummyToken{};
            impl_slReflexGetPredictedCameraData(sl::ViewportHandle(targetViewport), dummyToken, *predCam);
            resolvedAny = true;
        }
        currOut->next = nextOut;
        currOut = nextOut;
    }

    if (!resolvedAny) {
        LOG_WARN("impl_slGetData: No recognized output structure provided");
        return sl::Result::eErrorMissingInputParameter;
    }

    return sl::Result::eOk;
}

SL_API sl::Result slSetData(const sl::BaseStructure* inputs, sl::CommandBuffer* cmdBuffer) {
    return impl_slSetData(inputs, cmdBuffer);
}

SL_API sl::Result slGetData(const sl::BaseStructure* inputs, sl::BaseStructure* output, sl::CommandBuffer* cmdBuffer) {
    return impl_slGetData(inputs, output, cmdBuffer);
}

} // extern "C"

// -----------------------------------------------------------------------------
// Streamline Core API
// -----------------------------------------------------------------------------

SL_API sl::Result slInit(const sl::Preferences &pref, uint64_t sdkVersion) {
    if ((sdkVersion & sl::kSDKVersionMagic) != sl::kSDKVersionMagic) {
        LOG_ERROR("slInit: Invalid host SDK version (0x%llX) - did you forget to pass in 'kSDKVersion' on slInit?", sdkVersion);
        return sl::Result::eErrorInvalidParameter;
    }

    if (pref.pathToLogsAndData && pref.pathToLogsAndData[0] != L'\0') {
        std::wstring path = pref.pathToLogsAndData;
        if (path.back() == L'\\' || path.back() == L'/') {
            path += L"metalstream.log";
        }
        Logger::Instance().Init(path);
    } else {
        Logger::Instance().Init();
    }

    if (pref.logMessageCallback) {
        Logger::Instance().SetLogCallback(pref.logMessageCallback);
    }

    if (pref.logLevel == sl::LogLevel::eOff) {
        Logger::Instance().SetEnabled(false);
    } else if (pref.logLevel == sl::LogLevel::eVerbose) {
        Logger::Instance().SetEnabled(true);
        Logger::Instance().SetMinLogLevel(LogLevel::Debug);
    }

    LOG_INFO("slInit called. SDK Version: 0x%llX, Application ID: %u, Engine: %u, Flags: 0x%llX, RenderAPI: %u",
             sdkVersion, pref.applicationId, (uint32_t)pref.engine, (unsigned long long)pref.flags, (uint32_t)pref.renderAPI);
    Config::LogSummary();

    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    ctx.preferences = pref;
    ctx.initialized = true;

    // Feature loading whitelist based on preferences
    ctx.loadedFeatures.clear();
    if (pref.featuresToLoad && pref.numFeaturesToLoad > 0) {
        for (uint32_t i = 0; i < pref.numFeaturesToLoad; ++i) {
            sl::Feature feat = pref.featuresToLoad[i];
            if (feat == sl::kFeatureDLSS || feat == sl::kFeatureDLSS_G ||
                feat == sl::kFeatureReflex || feat == sl::kFeaturePCL)
            {
                ctx.loadedFeatures[feat] = true;
                LOG_INFO("slInit: Whitelisted feature %u loaded", feat);
            } else {
                ctx.loadedFeatures[feat] = false;
                LOG_WARN("slInit: Requested feature %u is not supported by MetalStream", feat);
            }
        }
    } else {
        // Default core features loaded for games omitting featuresToLoad list
        ctx.loadedFeatures[sl::kFeatureDLSS] = true;
        ctx.loadedFeatures[sl::kFeatureDLSS_G] = true;
        ctx.loadedFeatures[sl::kFeatureReflex] = true;
        ctx.loadedFeatures[sl::kFeaturePCL] = true;
        LOG_INFO("slInit: Default core features (DLSS, DLSS-G, Reflex, PCL) loaded");
    }

    // Hook the DXGI factory/swapchain vtables so DLSS-G can intercept Present
    framegen::InstallGlobalHooks();

    // If D3D12 device is already captured, ensure NVNGXRouter is initialized with application ID
    if (ctx.d3d12Device) {
        NVNGXRouter::Instance().Init(ctx.d3d12Device, pref.applicationId);
    }

    {
        std::lock_guard<std::mutex> tokenLock(ctx.mtxFrameHandle);
        ctx.frameCounter = 0;
        ctx.frameHandleIndex = 0;
        for (uint32_t i = 0; i < sl::MAX_FRAMES_IN_FLIGHT; ++i) {
            ctx.frameHandles[i].counter.store(0);
        }
    }

    // Reset frame-aware resource tagging container ring buffer
    for (size_t i = 0; i < StreamlineContext::NUM_FRAME_SLOTS; ++i) {
        ctx.frameSlots[i].frameIndex = UINT_MAX;
        ctx.frameSlots[i].taggedResources.clear();
        ctx.frameSlots[i].viewportConstants.clear();
    }

    return sl::Result::eOk;
}

SL_API sl::Result slShutdown() {
    LOG_INFO("slShutdown called");
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();
    framegen::Shutdown();
    // Acquire stateMutex before shutting down NVNGXRouter to strictly maintain stateMutex -> m_mutex order
    NVNGXRouter::Instance().Shutdown();
    ctx.initialized = false;
    if (ctx.m_dummyFence) {
        ctx.m_dummyFence->Release();
        ctx.m_dummyFence = nullptr;
    }
    ctx.d3d12Device = nullptr;
    ctx.loadedFeatures.clear();
    ctx.viewportConstants.clear();
    ctx.dlssOptions.clear();
    ctx.dlssgOptions.clear();
    ctx.reflexCameraData.clear();
    ctx.taggedResources.clear();
    ctx.viewportTagSequence.clear();
    ctx.lastResetFrame.clear();
    // Clear all frame slots to prevent stale ID3D12Resource* pointers across sessions
    for (size_t i = 0; i < StreamlineContext::NUM_FRAME_SLOTS; ++i) {
        ctx.frameSlots[i].frameIndex = UINT_MAX;
        ctx.frameSlots[i].taggedResources.clear();
        ctx.frameSlots[i].viewportConstants.clear();
    }
    {
        std::lock_guard<std::mutex> tokenLock(ctx.mtxFrameHandle);
        ctx.frameCounter = 0;
        ctx.frameHandleIndex = 0;
        for (uint32_t i = 0; i < sl::MAX_FRAMES_IN_FLIGHT; ++i) {
            ctx.frameHandles[i].counter.store(0);
        }
    }
    metalstream::Logger::Instance().SetLogCallback(nullptr);
    return sl::Result::eOk;
}

SL_API sl::Result slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo) {
    LOG_DEBUG("slIsFeatureSupported called for feature %u", feature);
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();

    if (ctx.preferences.renderAPI != sl::RenderAPI::eD3D11 &&
        ctx.preferences.renderAPI != sl::RenderAPI::eD3D12 &&
        ctx.preferences.renderAPI != sl::RenderAPI::eVulkan)
    {
        LOG_ERROR("slIsFeatureSupported: Unexpected renderAPI value %u in sl::Preferences", (uint32_t)ctx.preferences.renderAPI);
        return sl::Result::eErrorInvalidParameter;
    }

    switch (feature) {
        case sl::kFeatureDLSS:
        case sl::kFeatureDLSS_G:
        case sl::kFeatureReflex:
        case sl::kFeaturePCL:
            return sl::Result::eOk;
        case (sl::Feature)1001: // kFeatureDLSS_RR
        case (sl::Feature)2:    // kFeatureNIS
        case (sl::Feature)5:    // kFeatureDeepDVC
        case (sl::Feature)1003: // kFeatureDirectSR
            LOG_WARN("slIsFeatureSupported: Feature %u is not supported on this platform", feature);
            return sl::Result::eErrorFeatureNotSupported;
        default:
            LOG_WARN("slIsFeatureSupported: Feature %u is missing or unknown", feature);
            return sl::Result::eErrorFeatureMissing;
    }
}

SL_API sl::Result slIsFeatureLoaded(sl::Feature feature, bool& loaded) {
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();
    SL_VALIDATE_FEATURE(feature);
    auto it = ctx.loadedFeatures.find(feature);
    if (it != ctx.loadedFeatures.end()) {
        loaded = it->second;
    } else {
        loaded = false; // Unsupported or unloaded features report false
    }
    LOG_DEBUG("slIsFeatureLoaded called for feature %u -> %d", feature, loaded);
    return sl::Result::eOk;
}

SL_API sl::Result slSetFeatureLoaded(sl::Feature feature, bool loaded) {
    LOG_INFO("slSetFeatureLoaded called for feature %u -> %d", feature, loaded);
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();
    SL_VALIDATE_FEATURE(feature);
    ctx.loadedFeatures[feature] = loaded;
    return sl::Result::eOk;
}

SL_API sl::Result slEvaluateFeature(
    sl::Feature feature,
    const sl::FrameToken& frame,
    const sl::BaseStructure** inputs,
    uint32_t numInputs,
    sl::CommandBuffer* cmdBuffer)
{
    LOG_DEBUG("slEvaluateFeature called for feature %u, numInputs: %u, cmdBuffer: %p",
              feature, numInputs, cmdBuffer);

    auto& ctx = StreamlineContext::Instance();
    {
        std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
        if (!ctx.initialized) {
            LOG_ERROR("slEvaluateFeature: Streamline not initialized");
            return sl::Result::eErrorNotInitialized;
        }
    }

    if (feature != sl::kFeatureDLSS && feature != sl::kFeatureDLSS_G &&
        feature != sl::kFeatureReflex && feature != sl::kFeaturePCL)
    {
        LOG_ERROR("slEvaluateFeature: feature %u is missing or not supported", feature);
        return sl::Result::eErrorFeatureMissing;
    }
    
    if (feature == sl::kFeatureDLSS) {
        if (!cmdBuffer) {
            LOG_ERROR("slEvaluateFeature(DLSS): Missing cmdBuffer");
            return sl::Result::eErrorMissingInputParameter;
        }

        auto cmdList = static_cast<ID3D12GraphicsCommandList*>(cmdBuffer);

        // Local snapshot variables to dispatch Evaluate outside stateMutex
        ID3D12Resource* inRes = nullptr;
        ID3D12Resource* outRes = nullptr;
        ID3D12Resource* depthRes = nullptr;
        ID3D12Resource* mvecRes = nullptr;
        ID3D12Resource* transparencyRes = nullptr;
        ID3D12Resource* biasRes = nullptr;
        DRSCoordinates drs{};
        ExtraDLSSRenderArgs extraArgs{};
        float jitterX = 0.0f;
        float jitterY = 0.0f;
        float mvScaleX = 0.0f;
        float mvScaleY = 0.0f;
        bool reset = false;
        float sharpness = 0.0f;
        float preExposure = 1.0f;
        float exposureScale = 1.0f;
        D3D12_RESOURCE_STATES inState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES outState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES depthState = (D3D12_RESOURCE_STATES)UINT_MAX;
        D3D12_RESOURCE_STATES mvecState = (D3D12_RESOURCE_STATES)UINT_MAX;
        sl::DLSSMode mode = sl::DLSSMode::eMaxQuality;
        int createFlags = 0;
        uint32_t targetViewport = 0;
        bool canEvaluate = false;
        uint32_t fIdx = (uint32_t)frame;

        {
            std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);

            // Pass 1: Resolve targetViewport across all input chains first
            if (inputs && numInputs > 0) {
                for (uint32_t i = 0; i < numInputs; ++i) {
                    const sl::BaseStructure* base = inputs[i];
                    while (base) {
                        if (base->structType == sl::ViewportHandle::s_structType) {
                            targetViewport = *static_cast<const sl::ViewportHandle*>(base);
                            break;
                        }
                        base = base->next;
                    }
                }
            }

            // Pass 2: Unpack input structures into the resolved targetViewport
            if (inputs && numInputs > 0) {
                for (uint32_t i = 0; i < numInputs; ++i) {
                    const sl::BaseStructure* base = inputs[i];
                    while (base) {
                        if (base->structType == sl::ResourceTag::s_structType) {
                            const auto* tag = static_cast<const sl::ResourceTag*>(base);
                            if (tag->resource) {
                                TaggedResourceEntry entry;
                                entry.resource = *tag->resource;
                                entry.extent = tag->extent;
                                // Local tag per Streamline specification: stored in frame slot only, NOT in ctx.taggedResources

                                auto& slot = ctx.frameSlots[fIdx % StreamlineContext::NUM_FRAME_SLOTS];
                                if (slot.frameIndex != fIdx) {
                                    slot.frameIndex = fIdx;
                                    slot.taggedResources.clear();
                                    slot.viewportConstants.clear();
                                }
                                slot.taggedResources[targetViewport][tag->type] = entry;
                            }
                        } else if (base->structType == sl::DLSSOptions::s_structType) {
                            const auto* opts = static_cast<const sl::DLSSOptions*>(base);
                            impl_slDLSSSetOptions(sl::ViewportHandle(targetViewport), *opts);
                        } else if (base->structType == sl::DLSSGOptions::s_structType) {
                            const auto* gopts = static_cast<const sl::DLSSGOptions*>(base);
                            ctx.dlssgOptions[targetViewport] = *gopts;
                        } else if (base->structType == sl::ReflexCameraData::s_structType) {
                            const auto* cam = static_cast<const sl::ReflexCameraData*>(base);
                            ctx.reflexCameraData[targetViewport] = *cam;
                        } else if (base->structType == sl::Constants::s_structType) {
                            const auto* consts = static_cast<const sl::Constants*>(base);
                            sl::Constants merged = *consts;
                            // Preserve pending reset if already set by slSetConstants
                            if (merged.reset != sl::Boolean::eTrue) {
                                auto globalIt = ctx.viewportConstants.find(targetViewport);
                                if (globalIt != ctx.viewportConstants.end() && globalIt->second.reset == sl::Boolean::eTrue) {
                                    merged.reset = sl::Boolean::eTrue;
                                }
                                auto& currentSlot = ctx.frameSlots[fIdx % StreamlineContext::NUM_FRAME_SLOTS];
                                if (currentSlot.frameIndex == fIdx) {
                                    auto slotIt = currentSlot.viewportConstants.find(targetViewport);
                                    if (slotIt != currentSlot.viewportConstants.end() && slotIt->second.reset == sl::Boolean::eTrue) {
                                        merged.reset = sl::Boolean::eTrue;
                                    }
                                }
                            }

                            auto& slot = ctx.frameSlots[fIdx % StreamlineContext::NUM_FRAME_SLOTS];
                            if (slot.frameIndex != fIdx) {
                                slot.frameIndex = fIdx;
                                slot.taggedResources.clear();
                                slot.viewportConstants.clear();
                            }
                            slot.viewportConstants[targetViewport] = merged;
                        }
                        base = base->next;
                    }
                }
            }

            // Strict per-viewport tagged resource lookup with fallback to unversioned per-tag
            auto& slot = ctx.frameSlots[fIdx % StreamlineContext::NUM_FRAME_SLOTS];
            auto getTaggedResource = [&](sl::BufferType type) -> const TaggedResourceEntry* {
                if (slot.frameIndex == fIdx) {
                    auto slotVpIt = slot.taggedResources.find(targetViewport);
                    if (slotVpIt != slot.taggedResources.end()) {
                        auto it = slotVpIt->second.find(type);
                        if (it != slotVpIt->second.end()) {
                            return &it->second;
                        }
                    }
                }
                auto globalVpIt = ctx.taggedResources.find(targetViewport);
                if (globalVpIt != ctx.taggedResources.end()) {
                    auto it = globalVpIt->second.find(type);
                    if (it != globalVpIt->second.end()) {
                        return &it->second;
                    }
                }
                return nullptr;
            };

            // Resolve input color buffer
            auto inEntry = getTaggedResource(sl::kBufferTypeScalingInputColor);
            if (!inEntry) inEntry = getTaggedResource(sl::kBufferTypeHUDLessColor);
            if (inEntry && inEntry->resource.native) {
                inRes = static_cast<ID3D12Resource*>(inEntry->resource.native);
                inState = static_cast<D3D12_RESOURCE_STATES>(inEntry->resource.state);
                drs.colorInExt = inEntry->extent;
            }

            // Resolve output color buffer
            auto outEntry = getTaggedResource(sl::kBufferTypeScalingOutputColor);
            if (!outEntry) outEntry = getTaggedResource(sl::kBufferTypeBackbuffer);
            if (outEntry && outEntry->resource.native) {
                outRes = static_cast<ID3D12Resource*>(outEntry->resource.native);
                outState = static_cast<D3D12_RESOURCE_STATES>(outEntry->resource.state);
                drs.colorOutExt = outEntry->extent;
            }

            if (inRes && outRes) {
                auto depthEntry = getTaggedResource(sl::kBufferTypeDepth);
                if (!depthEntry) depthEntry = getTaggedResource(sl::kBufferTypeLinearDepth);
                if (depthEntry && depthEntry->resource.native) {
                    depthRes = static_cast<ID3D12Resource*>(depthEntry->resource.native);
                    depthState = static_cast<D3D12_RESOURCE_STATES>(depthEntry->resource.state);
                    drs.depthExt = depthEntry->extent;
                }

                auto mvecEntry = getTaggedResource(sl::kBufferTypeMotionVectors);
                if (mvecEntry && mvecEntry->resource.native) {
                    mvecRes = static_cast<ID3D12Resource*>(mvecEntry->resource.native);
                    mvecState = static_cast<D3D12_RESOURCE_STATES>(mvecEntry->resource.state);
                    drs.mvecExt = mvecEntry->extent;
                }

                auto transEntry = getTaggedResource(sl::kBufferTypeTransparencyHint);
                if (transEntry && transEntry->resource.native) {
                    extraArgs.transparencyMask = static_cast<ID3D12Resource*>(transEntry->resource.native);
                    extraArgs.transparencyState = static_cast<D3D12_RESOURCE_STATES>(transEntry->resource.state);
                    drs.transparencyExt = transEntry->extent;
                }

                auto biasEntry = getTaggedResource(sl::kBufferTypeBiasCurrentColorHint);
                if (biasEntry && biasEntry->resource.native) {
                    extraArgs.biasCurrentColorMask = static_cast<ID3D12Resource*>(biasEntry->resource.native);
                    extraArgs.biasState = static_cast<D3D12_RESOURCE_STATES>(biasEntry->resource.state);
                    drs.biasExt = biasEntry->extent;
                }

                auto expEntry = getTaggedResource(sl::kBufferTypeExposure);
                if (expEntry && expEntry->resource.native) {
                    extraArgs.exposureTexture = static_cast<ID3D12Resource*>(expEntry->resource.native);
                    extraArgs.exposureState = static_cast<D3D12_RESOURCE_STATES>(expEntry->resource.state);
                    drs.exposureExt = expEntry->extent;
                }

                auto animEntry = getTaggedResource(sl::kBufferTypeAnimatedTextureHint);
                if (animEntry && animEntry->resource.native) {
                    extraArgs.animatedTextureMask = static_cast<ID3D12Resource*>(animEntry->resource.native);
                    extraArgs.animatedTextureState = static_cast<D3D12_RESOURCE_STATES>(animEntry->resource.state);
                    drs.animTextureExt = animEntry->extent;
                }

                auto rayEntry = getTaggedResource(sl::kBufferTypeRaytracingDistance);
                if (rayEntry && rayEntry->resource.native) {
                    extraArgs.rayTracingHitDistance = static_cast<ID3D12Resource*>(rayEntry->resource.native);
                    extraArgs.rayTraceDistState = static_cast<D3D12_RESOURCE_STATES>(rayEntry->resource.state);
                }

                auto mvecReflEntry = getTaggedResource(sl::kBufferTypeReflectionMotionVectors);
                if (mvecReflEntry && mvecReflEntry->resource.native) {
                    extraArgs.motionVectorsReflection = static_cast<ID3D12Resource*>(mvecReflEntry->resource.native);
                    extraArgs.mvecReflectionsState = static_cast<D3D12_RESOURCE_STATES>(mvecReflEntry->resource.state);
                }

                auto particleEntry = getTaggedResource(sl::kBufferTypeParticleHint);
                if (particleEntry && particleEntry->resource.native) {
                    extraArgs.particleMask = static_cast<ID3D12Resource*>(particleEntry->resource.native);
                    extraArgs.particleMaskState = static_cast<D3D12_RESOURCE_STATES>(particleEntry->resource.state);
                }

                    D3D12_RESOURCE_DESC inDesc = inRes->GetDesc();
                    D3D12_RESOURCE_DESC outDesc = outRes->GetDesc();

                    uint32_t renderW = (drs.colorInExt.width > 0) ? drs.colorInExt.width : (uint32_t)inDesc.Width;
                    uint32_t renderH = (drs.colorInExt.height > 0) ? drs.colorInExt.height : (uint32_t)inDesc.Height;
                    uint32_t targetW = (drs.colorOutExt.width > 0) ? drs.colorOutExt.width : (uint32_t)outDesc.Width;
                    uint32_t targetH = (drs.colorOutExt.height > 0) ? drs.colorOutExt.height : (uint32_t)outDesc.Height;

                    drs.renderWidth = renderW;
                    drs.renderHeight = renderH;
                    drs.targetWidth = targetW;
                    drs.targetHeight = targetH;

                    // Motion vector resolution detection (aligns with NV Streamline dlssEntry.cpp:455-459)
                    uint32_t mvecW = (drs.mvecExt.width > 0) ? drs.mvecExt.width : (mvecRes ? (uint32_t)mvecRes->GetDesc().Width : renderW);
                    uint32_t mvecH = (drs.mvecExt.height > 0) ? drs.mvecExt.height : (mvecRes ? (uint32_t)mvecRes->GetDesc().Height : renderH);

                    if (mvecW > renderW || mvecH > renderH) {
                        // High-resolution motion vectors (e.g. display target resolution)
                        createFlags = 0;
                        mvScaleX = (float)targetW;
                        mvScaleY = (float)targetH;
                    } else {
                        // Low-resolution motion vectors (render resolution)
                        createFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
                        mvScaleX = (float)renderW;
                        mvScaleY = (float)renderH;
                    }
                    bool depthInverted = true;
                    bool mvecJittered = false;

                    // Strict per-viewport constants lookup (NEVER fall back to begin()!)
                    const sl::Constants* pConsts = nullptr;
                    if (slot.frameIndex == fIdx) {
                        auto slotConstIt = slot.viewportConstants.find(targetViewport);
                        if (slotConstIt != slot.viewportConstants.end()) {
                            pConsts = &slotConstIt->second;
                        }
                    }
                    if (!pConsts) {
                        auto globalConstIt = ctx.viewportConstants.find(targetViewport);
                        if (globalConstIt != ctx.viewportConstants.end()) {
                            pConsts = &globalConstIt->second;
                        }
                    }

                    if (pConsts) {
                        auto c = *pConsts;
                        if (IsValidFiniteFloat(c.jitterOffset.x) && std::abs(c.jitterOffset.x) < 5000.0f) {
                            jitterX = c.jitterOffset.x;
                        } else {
                            jitterX = 0.0f;
                        }
                        if (IsValidFiniteFloat(c.jitterOffset.y) && std::abs(c.jitterOffset.y) < 5000.0f) {
                            jitterY = c.jitterOffset.y;
                        } else {
                            jitterY = 0.0f;
                        }

                        if (c.reset == sl::Boolean::eTrue) {
                            if (ctx.lastResetFrame.find(targetViewport) == ctx.lastResetFrame.end() ||
                                ctx.lastResetFrame[targetViewport] != fIdx)
                            {
                                reset = true;
                                ctx.lastResetFrame[targetViewport] = fIdx;
                            }
                            if (slot.frameIndex == fIdx) {
                                slot.viewportConstants[targetViewport].reset = sl::Boolean::eFalse;
                            }
                            ctx.viewportConstants[targetViewport].reset = sl::Boolean::eFalse;
                        }
                        if (c.depthInverted == sl::Boolean::eFalse) {
                            depthInverted = false;
                        } else if (c.depthInverted == sl::Boolean::eTrue) {
                            depthInverted = true;
                        }
                        if (c.motionVectorsJittered == sl::Boolean::eTrue) {
                            mvecJittered = true;
                        }

                        // Validate and sanitize clipToPrevClip matrix against INVALID_FLOAT / NaN / Inf
                        bool matrixValid = true;
                        const float* pMat = reinterpret_cast<const float*>(&c.clipToPrevClip);
                        for (int i = 0; i < 16; ++i) {
                            if (!IsValidFiniteFloat(pMat[i]) || std::abs(pMat[i]) > 1e12f) {
                                matrixValid = false;
                                break;
                            }
                        }

                        if (matrixValid) {
                            extraArgs.hasClipToPrevClip = true;
                            extraArgs.clipToPrevClip = c.clipToPrevClip;
                        } else {
                            extraArgs.hasClipToPrevClip = false;
                            extraArgs.clipToPrevClip = MakeIdentityMatrix();
                        }

                        if (c.cameraMotionIncluded == sl::Boolean::eFalse) {
                            extraArgs.cameraMotionIncluded = false;
                            static std::atomic<bool> s_warnedCameraMotion{false};
                            if (!s_warnedCameraMotion.exchange(true)) {
                                LOG_WARN("cameraMotionIncluded is false for viewport %u; forwarding sanitized clipToPrevClip matrix to NGX parameter bus", targetViewport);
                            }
                        } else {
                            extraArgs.cameraMotionIncluded = true;
                        }

                        if (IsValidFiniteFloat(c.mvecScale.x) && std::abs(c.mvecScale.x) > 1e-6f && std::abs(c.mvecScale.x) < 5000.0f) {
                            mvScaleX = c.mvecScale.x * (float)renderW;
                        } else {
                            mvScaleX = (float)renderW;
                        }
                        if (IsValidFiniteFloat(c.mvecScale.y) && std::abs(c.mvecScale.y) > 1e-6f && std::abs(c.mvecScale.y) < 5000.0f) {
                            mvScaleY = c.mvecScale.y * (float)renderH;
                        } else {
                            mvScaleY = (float)renderH;
                        }
                    }

                    bool isHDR = true;
                    bool useAutoExposure = true;
                    bool alphaUpscaling = false;

                    // Strict per-viewport DLSS options lookup (NEVER fall back to begin()!)
                    auto optIt = ctx.dlssOptions.find(targetViewport);
                    if (optIt == ctx.dlssOptions.end()) {
                        LOG_ERROR("slEvaluateFeature(DLSS): Missing DLSS options for viewport %u", targetViewport);
                        return sl::Result::eErrorMissingConstants;
                    }
                    const auto& opt = optIt->second;
                    mode = opt.mode;
                    if (IsValidFiniteFloat(opt.sharpness) && opt.sharpness >= 0.0f) {
                        sharpness = std::min(opt.sharpness, 1.0f);
                    } else {
                        sharpness = 0.0f;
                    }
                    if (IsValidFiniteFloat(opt.preExposure) && opt.preExposure > 0.0f && opt.preExposure < 1000.0f) {
                        preExposure = opt.preExposure;
                    } else {
                        preExposure = 1.0f;
                    }
                    if (IsValidFiniteFloat(opt.exposureScale) && opt.exposureScale > 0.0f && opt.exposureScale < 1000.0f) {
                        exposureScale = opt.exposureScale;
                    } else {
                        exposureScale = 1.0f;
                    }
                    if (opt.colorBuffersHDR == sl::Boolean::eFalse) {
                        isHDR = false;
                    }
                    if (opt.useAutoExposure == sl::Boolean::eFalse) {
                        useAutoExposure = false;
                    }
                    if (opt.alphaUpscalingEnabled == sl::Boolean::eTrue) {
                        alphaUpscaling = true;
                    }
                    extraArgs.dlaaPreset = opt.dlaaPreset;
                    extraArgs.qualityPreset = opt.qualityPreset;
                    extraArgs.balancedPreset = opt.balancedPreset;
                    extraArgs.performancePreset = opt.performancePreset;
                    extraArgs.ultraPerformancePreset = opt.ultraPerformancePreset;
                    extraArgs.ultraQualityPreset = opt.ultraQualityPreset;
                    extraArgs.indicatorInvertAxisX = (opt.indicatorInvertAxisX == sl::Boolean::eTrue);
                    extraArgs.indicatorInvertAxisY = (opt.indicatorInvertAxisY == sl::Boolean::eTrue);

                    // Official Streamline specification: when DLSSMode is eOff, host must not evaluate DLSS
                    if (mode == sl::DLSSMode::eOff) {
                        LOG_WARN("slEvaluateFeature(DLSS): DLSSMode is set to eOff, slEvaluateFeature(DLSS) should not be called");
                        return sl::Result::eErrorInvalidIntegration;
                    }

                    // Safety guard: if no exposure texture tagged or bound, force useAutoExposure = true
                    if (!extraArgs.exposureTexture) {
                        useAutoExposure = true;
                    }

                    if (isHDR) createFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
                    if (depthInverted) createFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
                    if (mvecJittered) createFlags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
                    if (useAutoExposure) createFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                    if (sharpness > 0.0f) createFlags |= NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                    if (alphaUpscaling) createFlags |= NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling;

                    canEvaluate = true;
                } else {
                    LOG_WARN("slEvaluateFeature(DLSS): Missing inRes (%p) or outRes (%p) for vp %u",
                             inRes, outRes, targetViewport);
                    return sl::Result::eErrorMissingInputParameter;
                }
        } // stateMutex unlocked here to avoid lock contention and ABBA deadlocks

        if (canEvaluate) {
            NVNGXRouter::Instance().Evaluate(
                targetViewport,
                fIdx,
                cmdList,
                inRes,
                outRes,
                depthRes,
                mvecRes,
                jitterX,
                jitterY,
                mvScaleX,
                mvScaleY,
                reset,
                sharpness,
                preExposure,
                exposureScale,
                inState,
                outState,
                mode,
                createFlags,
                drs,
                depthState,
                mvecState,
                extraArgs
            );
            return sl::Result::eOk;
        } else {
            return sl::Result::eErrorMissingInputParameter;
        }
    }

    return sl::Result::eOk;
}

SL_API sl::Result slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport) {
    return impl_slAllocateResources(cmdBuffer, feature, viewport);
}

SL_API sl::Result slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport) {
    return impl_slFreeResources(feature, viewport);
}

SL_API sl::Result slSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer) {
    LOG_DEBUG("slSetTag called for viewport %u, numTags %u", (uint32_t)viewport, numTags);
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();

    if (ctx.preferences.flags & sl::PreferenceFlags::eUseFrameBasedResourceTagging) {
        LOG_ERROR("slSetTag: 'PreferenceFlags::eUseFrameBasedResourceTagging' flag is specified in sl::Preferences but slSetTag was called");
        return sl::Result::eErrorInvalidIntegration;
    }

    if (!tags || numTags == 0) return sl::Result::eErrorInvalidParameter;
    ctx.viewportTagSequence[(uint32_t)viewport] = ++ctx.tagSequence;
    auto& vpTags = ctx.taggedResources[(uint32_t)viewport];

    for (uint32_t i = 0; i < numTags; ++i) {
        const sl::BaseStructure* curr = &tags[i];
        while (curr) {
            if (curr->structType == sl::ResourceTag::s_structType) {
                const auto* tag = static_cast<const sl::ResourceTag*>(curr);
                if (IsNullTag(tag)) {
                    vpTags.erase(tag->type);
                } else {
                    vpTags[tag->type] = MakeTagEntry(ctx, tag, cmdBuffer, (uint32_t)viewport);
                }
            }
            curr = curr->next;
        }
    }
    return sl::Result::eOk;
}

SL_API sl::Result slSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer) {
    LOG_DEBUG("slSetTagForFrame called for frame %u, viewport %u, numTags %u", (uint32_t)frame, (uint32_t)viewport, numTags);
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();

    if ((ctx.preferences.flags & sl::PreferenceFlags::eUseFrameBasedResourceTagging) == 0) {
        LOG_ERROR("slSetTagForFrame: 'PreferenceFlags::eUseFrameBasedResourceTagging' flag is NOT set in sl::Preferences");
        return sl::Result::eErrorInvalidIntegration;
    }

    if (!tags || numTags == 0) return sl::Result::eErrorInvalidParameter;
    ctx.viewportTagSequence[(uint32_t)viewport] = ++ctx.tagSequence;
    uint32_t fIdx = (uint32_t)frame;
    auto& slot = ctx.frameSlots[fIdx % StreamlineContext::NUM_FRAME_SLOTS];
    if (slot.frameIndex != fIdx) {
        slot.frameIndex = fIdx;
        slot.taggedResources.clear();
        slot.viewportConstants.clear();
    }
    auto& vpTags = slot.taggedResources[(uint32_t)viewport];
    // Also update fallback unversioned tags for backward-compatibility
    auto& globalTags = ctx.taggedResources[(uint32_t)viewport];

    for (uint32_t i = 0; i < numTags; ++i) {
        const sl::BaseStructure* curr = &tags[i];
        while (curr) {
            if (curr->structType == sl::ResourceTag::s_structType) {
                const auto* tag = static_cast<const sl::ResourceTag*>(curr);
                if (IsNullTag(tag)) {
                    vpTags.erase(tag->type);
                    globalTags.erase(tag->type);
                } else {
                    vpTags[tag->type] = MakeTagEntry(ctx, tag, cmdBuffer, (uint32_t)viewport);
                }
            }
            curr = curr->next;
        }
    }

    for (const auto& pair : vpTags) {
        globalTags[pair.first] = pair.second;
    }

    return sl::Result::eOk;
}

SL_API sl::Result slSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) {
    LOG_DEBUG("slSetConstants vp=%u, frame=%u: jitter=(%.4f, %.4f), mvecScale=(%.4f, %.4f), depthInv=%d, mvecJittered=%d, reset=%d",
              (uint32_t)viewport, (uint32_t)frame, values.jitterOffset.x, values.jitterOffset.y,
              values.mvecScale.x, values.mvecScale.y,
              (int)values.depthInverted, (int)values.motionVectorsJittered, (int)values.reset);
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    if (!ctx.initialized) {
        LOG_ERROR("slSetConstants: Streamline not initialized");
        return sl::Result::eErrorNotInitialized;
    }
    uint32_t fIdx = (uint32_t)frame;
    auto& slot = ctx.frameSlots[fIdx % StreamlineContext::NUM_FRAME_SLOTS];
    if (slot.frameIndex != fIdx) {
        slot.frameIndex = fIdx;
        slot.taggedResources.clear();
        slot.viewportConstants.clear();
    }
    slot.viewportConstants[(uint32_t)viewport] = values;

    // Fallback unversioned
    ctx.viewportConstants[(uint32_t)viewport] = values;
    return sl::Result::eOk;
}

SL_API sl::Result slSetD3DDevice(void* d3dDevice) {
    LOG_INFO("slSetD3DDevice called with %p", d3dDevice);
    if (!d3dDevice) return sl::Result::eErrorInvalidParameter;

    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();

    auto* unk = static_cast<IUnknown*>(d3dDevice);

    // 1. Probe for ID3D12Device
    ID3D12Device* d3d12Device = nullptr;
    HRESULT hr12 = unk->QueryInterface(IID_PPV_ARGS(&d3d12Device));
    if (SUCCEEDED(hr12) && d3d12Device) {
        d3d12Device->Release(); // QueryInterface added a refcount, release extra ref
        if (ctx.d3d12Device != d3d12Device) {
            if (ctx.m_dummyFence) {
                ctx.m_dummyFence->Release();
                ctx.m_dummyFence = nullptr;
            }
            ctx.d3d12Device = d3d12Device;
        }
        if (!ctx.m_dummyFence) {
            HRESULT hr = ctx.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx.m_dummyFence));
            if (SUCCEEDED(hr)) {
                LOG_INFO("slSetD3DDevice: Created DLSS-G dummy fence %p", ctx.m_dummyFence);
            } else {
                LOG_WARN("slSetD3DDevice: Failed to create dummy fence, hr=0x%08x", (uint32_t)hr);
            }
        }
        NVNGXRouter::Instance().Init(ctx.d3d12Device, ctx.preferences.applicationId);
        return sl::Result::eOk;
    }

    // 2. Probe for ID3D11Device
    static const GUID s_iidID3D11Device = { 0xdb6f6ddb, 0xac77, 0x4e88, { 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40 } };
    void* d3d11Device = nullptr;
    HRESULT hr11 = unk->QueryInterface(s_iidID3D11Device, &d3d11Device);
    if (SUCCEEDED(hr11) && d3d11Device) {
        static_cast<IUnknown*>(d3d11Device)->Release();
        LOG_INFO("slSetD3DDevice: Recognized ID3D11Device %p", d3d11Device);
        return sl::Result::eOk;
    }

    LOG_ERROR("slSetD3DDevice: Unknown interface %p - expecting ID3D12Device or ID3D11Device", d3dDevice);
    return sl::Result::eErrorUnsupportedInterface;
}

static const sl::BufferType s_dlssRequiredTags[] = {
    sl::kBufferTypeDepth,
    sl::kBufferTypeMotionVectors,
    sl::kBufferTypeScalingInputColor,
    sl::kBufferTypeScalingOutputColor
};

SL_API sl::Result slGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& requirements) {
    LOG_DEBUG("slGetFeatureRequirements called for feature %u", feature);
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();
    SL_VALIDATE_FEATURE(feature);

    sl::BaseStructure* savedNext = requirements.next;
    size_t savedVersion = requirements.structVersion;
    requirements = {};
    requirements.next = savedNext;
    requirements.structVersion = savedVersion;

    requirements.flags = sl::FeatureRequirementFlags::eD3D11Supported |
                         sl::FeatureRequirementFlags::eD3D12Supported |
                         sl::FeatureRequirementFlags::eVulkanSupported;
    requirements.maxNumViewports = 4;
    requirements.maxNumCPUThreads = 16;

    if (feature == sl::kFeatureDLSS) {
        requirements.numRequiredTags = 4;
        requirements.requiredTags = s_dlssRequiredTags;
    } else {
        requirements.numRequiredTags = 0;
        requirements.requiredTags = nullptr;
    }

    requirements.osVersionDetected = sl::Version(10, 0, 19045);
    requirements.osVersionRequired = sl::Version(10, 0, 19041);
    requirements.driverVersionDetected = sl::Version(550, 0, 0);
    requirements.driverVersionRequired = sl::Version(510, 0, 0);

    return sl::Result::eOk;
}

SL_API sl::Result slGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& version) {
    LOG_DEBUG("slGetFeatureVersion called for feature %u", feature);
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();
    SL_VALIDATE_FEATURE(feature);

    sl::BaseStructure* savedNext = version.next;
    size_t savedVersion = version.structVersion;
    version = {};
    version.next = savedNext;
    version.structVersion = savedVersion;
    version.versionSL = sl::Version(2, 4, 0);
    version.versionNGX = sl::Version(3, 1, 0);
    return sl::Result::eOk;
}

SL_API sl::Result slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex) {
    auto& ctx = StreamlineContext::Instance();
    {
        std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
        SL_REQUIRE_INITIALIZED();
    }
    std::lock_guard<std::mutex> lock(ctx.mtxFrameHandle);

    if (!frameIndex || (*frameIndex != ctx.frameHandles[ctx.frameHandleIndex].counter.load())) {
        ctx.frameHandleIndex = (ctx.frameHandleIndex + 1) % sl::MAX_FRAMES_IN_FLIGHT;
        ctx.frameHandles[ctx.frameHandleIndex].counter.store(frameIndex ? *frameIndex : ++ctx.frameCounter);
    }

    token = &ctx.frameHandles[ctx.frameHandleIndex];
    NVNGXRouter::Instance().TickFrame(token ? (uint32_t)*token : 0);
    return sl::Result::eOk;
}

SL_API sl::Result slGetNativeInterface(void* proxyInterface, void** baseInterface) {
    LOG_DEBUG("slGetNativeInterface called for proxy %p", proxyInterface);
    if (!proxyInterface || !baseInterface) {
        return sl::Result::eErrorInvalidParameter;
    }
    *baseInterface = proxyInterface;
    static_cast<IUnknown*>(proxyInterface)->AddRef();
    return sl::Result::eOk;
}

SL_API sl::Result slUpgradeInterface(void** baseInterface) {
    LOG_DEBUG("slUpgradeInterface called with %p", baseInterface ? *baseInterface : nullptr);
    if (!baseInterface || !*baseInterface) {
        return sl::Result::eErrorMissingInputParameter;
    }

    // Safely probe if *baseInterface is a valid readable COM interface pointer before invoking QueryInterface
    if (!IsBadReadPtr(*baseInterface, sizeof(void*))) {
        void** vtbl = *(void***)*baseInterface;
        if (vtbl && !IsBadReadPtr(vtbl, sizeof(void*) * 3)) {
            auto* unk = static_cast<IUnknown*>(*baseInterface);

            // 1. Probe for ID3D12Device (auto-capture in manual hooking mode)
            ID3D12Device* d3d12 = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&d3d12))) && d3d12) {
                d3d12->Release();
                slSetD3DDevice(*baseInterface);
                return sl::Result::eOk;
            }

            // 2. Probe for IDXGISwapChain (auto-capture device from swapchain)
            static const GUID s_iidSwapchain = { 0x310d36a0, 0xd2e7, 0x4c0a, { 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x61 } };
            IDXGISwapChain* swapChain = nullptr;
            if (SUCCEEDED(unk->QueryInterface(s_iidSwapchain, (void**)&swapChain)) && swapChain) {
                LOG_INFO("slUpgradeInterface: swapchain %p", *baseInterface);
                framegen::RegisterSwapChain(swapChain, nullptr);
                ID3D12Device* dev = nullptr;
                if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
                    dev->Release();
                    slSetD3DDevice(dev);
                }
                swapChain->Release();
                return sl::Result::eOk;
            }

            // 3. Probe for IDXGIFactory (hook swapchain creation)
            IDXGIFactory* factory = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&factory))) && factory) {
                LOG_INFO("slUpgradeInterface: factory %p", *baseInterface);
                framegen::InstallFactoryHooks(factory);
                factory->Release();
                return sl::Result::eOk;
            }
        }
    }

    return sl::Result::eOk;
}

SL_API sl::Result slGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function) {
    function = nullptr;
    if (!functionName) {
        return sl::Result::eErrorInvalidParameter;
    }

    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    SL_REQUIRE_INITIALIZED();
    SL_VALIDATE_FEATURE(feature);

    // Feature Function Scoping: strictly align with Streamline plugin namespaces
    if (feature == sl::kFeatureDLSS) {
        if (strcmp(functionName, "slDLSSGetOptimalSettings") == 0) {
            function = (void*)&impl_slDLSSGetOptimalSettings;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slDLSSGetState") == 0) {
            function = (void*)&impl_slDLSSGetState;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slDLSSSetOptions") == 0) {
            function = (void*)&impl_slDLSSSetOptions;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slAllocateResources") == 0) {
            function = (void*)&impl_slAllocateResources;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slFreeResources") == 0) {
            function = (void*)&impl_slFreeResources;
            return sl::Result::eOk;
        }
    } else if (feature == sl::kFeatureDLSS_G) {
        if (strcmp(functionName, "slDLSSGGetState") == 0) {
            function = (void*)&impl_slDLSSGGetState;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slDLSSGSetOptions") == 0) {
            function = (void*)&impl_slDLSSGSetOptions;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slAllocateResources") == 0) {
            function = (void*)&impl_slAllocateResources;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slFreeResources") == 0) {
            function = (void*)&impl_slFreeResources;
            return sl::Result::eOk;
        }
    } else if (feature == sl::kFeatureReflex) {
        if (strcmp(functionName, "slReflexSleep") == 0) {
            function = (void*)&impl_slReflexSleep;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slReflexGetState") == 0) {
            function = (void*)&impl_slReflexGetState;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slReflexSetOptions") == 0) {
            function = (void*)&impl_slReflexSetOptions;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slReflexSetCameraData") == 0) {
            function = (void*)&impl_slReflexSetCameraData;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slReflexGetPredictedCameraData") == 0) {
            function = (void*)&impl_slReflexGetPredictedCameraData;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slReflexSetMarker") == 0) {
            function = (void*)&impl_slReflexSetMarker;
            return sl::Result::eOk;
        }
    } else if (feature == sl::kFeaturePCL) {
        if (strcmp(functionName, "slPCLSetMarker") == 0) {
            function = (void*)&impl_slPCLSetMarker;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slPCLGetState") == 0) {
            function = (void*)&impl_slPCLGetState;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slPCLSetOptions") == 0) {
            function = (void*)&impl_slPCLSetOptions;
            return sl::Result::eOk;
        }
    }

    // Unified Bus exports (available for any supported feature)
    if (feature == sl::kFeatureDLSS || feature == sl::kFeatureDLSS_G ||
        feature == sl::kFeatureReflex || feature == sl::kFeaturePCL)
    {
        if (strcmp(functionName, "slSetData") == 0) {
            function = (void*)&impl_slSetData;
            return sl::Result::eOk;
        }
        if (strcmp(functionName, "slGetData") == 0) {
            function = (void*)&impl_slGetData;
            return sl::Result::eOk;
        }
    }

    LOG_WARN("slGetFeatureFunction: function '%s' not found for feature %u", functionName, feature);
    function = nullptr;
    return sl::Result::eErrorMissingOrInvalidAPI;
}

SL_API sl::Result slSetVulkanInfo(const sl::VulkanInfo& info) {
    auto& ctx = StreamlineContext::Instance();
    std::lock_guard<std::recursive_mutex> lock(ctx.stateMutex);
    if (!ctx.initialized) {
        LOG_ERROR("slSetVulkanInfo: Streamline not initialized");
        return sl::Result::eErrorNotInitialized;
    }
    if (&info == nullptr || info.structVersion < sl::kStructVersion1) {
        LOG_ERROR("slSetVulkanInfo: Invalid VulkanInfo parameter");
        return sl::Result::eErrorInvalidParameter;
    }
    LOG_INFO("slSetVulkanInfo called");
    return sl::Result::eOk;
}
