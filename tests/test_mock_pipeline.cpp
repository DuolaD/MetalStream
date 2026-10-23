// Behavior tests for the slEvaluateFeature -> NVNGXRouter -> NGX path.
//
// Runs against a mock nvngx.dll and fake D3D12 objects (tests/mock/), so it checks what MetalStream
// actually does (feature creations, NGX evaluations, BlitGuard copies, barriers) instead of return codes.
// Needs only Wine, no GPU. Build output lives in build/mock/ together with sl.interposer.dll and the mock.
//
// CHECK      - required behavior; a failure makes the test exit non-zero.
// KNOWN      - desired behavior that the current code is known not to have (tracked review findings).
//              Reported but does not fail the run; when one starts passing, promote it to CHECK.

#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <streamline/sl.h>
#include <streamline/sl_dlss.h>
#include <streamline/sl_dlss_g.h>
#include <nvngx/nvsdk_ngx_defs.h>

#include "interposer/metalstream_diag.h"
#include "mock/fake_d3d12.h"
#include "mock/mock_nvngx_api.h"

typedef sl::Result (*PFN_slInit)(const sl::Preferences& pref, uint64_t sdkVersion);
typedef sl::Result (*PFN_slSetD3DDevice)(void* d3dDevice);
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*& token, const uint32_t* frameIndex);
typedef sl::Result (*PFN_slSetConstants)(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport);
typedef sl::Result (*PFN_slSetTag)(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slAllocateResources)(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport);
typedef sl::Result (*PFN_slFreeResources)(sl::Feature feature, const sl::ViewportHandle& viewport);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature feature, const char* functionName, void*& function);
typedef sl::Result (*PFN_slDLSSSetOptions)(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options);
typedef int (*PFN_MetalStream_GetDiagnostics)(MetalStreamDiagnostics* out);

static int g_failures = 0;
static int g_knownOpen = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) {                                                   \
            printf("  [PASS] ");                                      \
        } else {                                                      \
            printf("  [FAIL] %s:%d: ", __FILE__, __LINE__);           \
            g_failures++;                                             \
        }                                                             \
        printf(__VA_ARGS__);                                          \
        printf("\n");                                                 \
    } while (0)

#define KNOWN(id, cond, ...)                                                          \
    do {                                                                              \
        if (cond) {                                                                   \
            printf("  [KNOWN %s FIXED] now passes, promote to CHECK: ", id);          \
        } else {                                                                      \
            printf("  [KNOWN %s OPEN] ", id);                                         \
            g_knownOpen++;                                                            \
        }                                                                             \
        printf(__VA_ARGS__);                                                          \
        printf("\n");                                                                 \
    } while (0)

namespace {

struct Api {
    PFN_slInit init;
    PFN_slSetD3DDevice setDevice;
    PFN_slGetNewFrameToken newFrame;
    PFN_slSetConstants setConstants;
    PFN_slSetTag setTag;
    PFN_slEvaluateFeature evaluate;
    PFN_slAllocateResources allocate;
    PFN_slFreeResources freeResources;
    PFN_slDLSSSetOptions dlssSetOptions;
    PFN_MetalStream_GetDiagnostics diagnostics;
    PFN_MockNGX_GetCounters ngxCounters;
    PFN_MockNGX_SetCreateResult ngxSetCreateResult;
    PFN_MockNGX_SetEvaluateResult ngxSetEvaluateResult;
};

Api g_api{};
ID3D12GraphicsCommandList* g_cmdList = nullptr;

MetalStreamDiagnostics Diag() {
    MetalStreamDiagnostics d{};
    d.structSize = sizeof(d);
    g_api.diagnostics(&d);
    return d;
}

MockNGXCounters Ngx() {
    MockNGXCounters c{};
    g_api.ngxCounters(&c);
    return c;
}

FakeCmdListStats CmdStats() {
    FakeCmdListStats s{};
    Fake_GetCmdListStats(g_cmdList, &s);
    return s;
}

struct Snapshot {
    MetalStreamDiagnostics diag;
    MockNGXCounters ngx;
    static Snapshot Take() { return { Diag(), Ngx() }; }
};

// One upscaling viewport with its own textures; mirrors the per-frame call pattern of a typical integration.
struct Viewport {
    sl::ViewportHandle handle;
    ID3D12Resource* colorIn = nullptr;
    ID3D12Resource* colorOut = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* mvec = nullptr;
    sl::Extent colorInExtent{};  // empty = whole texture
    sl::Extent mvecExtent{};

    Viewport(uint32_t id, UINT renderW, UINT renderH, UINT outW, UINT outH) : handle(id) {
        colorIn = Fake_CreateTexture(renderW, renderH, DXGI_FORMAT_R16G16B16A16_FLOAT);
        colorOut = Fake_CreateTexture(outW, outH, DXGI_FORMAT_R16G16B16A16_FLOAT);
        depth = Fake_CreateTexture(renderW, renderH, DXGI_FORMAT_R32_FLOAT);
        mvec = Fake_CreateTexture(renderW, renderH, DXGI_FORMAT_R16G16_FLOAT);
    }

    void SetOptions(sl::DLSSMode mode, uint32_t outW, uint32_t outH) {
        sl::DLSSOptions opts{};
        opts.mode = mode;
        opts.outputWidth = outW;
        opts.outputHeight = outH;
        g_api.dlssSetOptions(handle, opts);
    }

    // Runs one frame: new token, constants, tags, evaluate. Returns the slEvaluateFeature result.
    sl::Result Frame(bool reset = false) {
        sl::FrameToken* token = nullptr;
        g_api.newFrame(token, nullptr);

        sl::Constants consts{};
        consts.jitterOffset = { 0.25f, -0.25f };
        consts.mvecScale = { 1.0f, 1.0f };
        consts.depthInverted = sl::Boolean::eTrue;
        consts.cameraMotionIncluded = sl::Boolean::eTrue;
        consts.motionVectorsJittered = sl::Boolean::eFalse;
        consts.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        g_api.setConstants(consts, *token, handle);

        const uint32_t state = (uint32_t)D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        sl::Resource rIn{ sl::ResourceType::eTex2d, colorIn, nullptr, nullptr, state };
        sl::Resource rOut{ sl::ResourceType::eTex2d, colorOut, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        sl::Resource rDepth{ sl::ResourceType::eTex2d, depth, nullptr, nullptr, state };
        sl::Resource rMvec{ sl::ResourceType::eTex2d, mvec, nullptr, nullptr, state };
        sl::ResourceTag tags[4] = {
            sl::ResourceTag(&rIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent,
                            colorInExtent ? &colorInExtent : nullptr),
            sl::ResourceTag(&rOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent),
            sl::ResourceTag(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                            colorInExtent ? &colorInExtent : nullptr),
            sl::ResourceTag(&rMvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent,
                            mvecExtent ? &mvecExtent : nullptr),
        };
        g_api.setTag(handle, tags, 4, (sl::CommandBuffer*)g_cmdList);

        const sl::BaseStructure* inputs[] = { &handle };
        return g_api.evaluate(sl::kFeatureDLSS, *token, inputs, 1, (sl::CommandBuffer*)g_cmdList);
    }
};

template <typename T>
bool Load(HMODULE mod, const char* name, T& out) {
    out = (T)GetProcAddress(mod, name);
    if (!out) printf("missing export %s\n", name);
    return out != nullptr;
}

bool Setup() {
    // The mock must be the nvngx.dll the interposer loads; load it first from our own directory
    HMODULE ngx = LoadLibraryA("nvngx.dll");
    if (!ngx || !GetProcAddress(ngx, "MockNGX_GetCounters")) {
        printf("nvngx.dll is not the mock (run from build/mock with WINEDLLOVERRIDES=nvngx=n)\n");
        return false;
    }
    HMODULE sl = LoadLibraryA("sl.interposer.dll");
    if (!sl) {
        printf("failed to load sl.interposer.dll\n");
        return false;
    }

    bool ok = Load(sl, "slInit", g_api.init) && Load(sl, "slSetD3DDevice", g_api.setDevice) &&
              Load(sl, "slGetNewFrameToken", g_api.newFrame) && Load(sl, "slSetConstants", g_api.setConstants) &&
              Load(sl, "slSetTag", g_api.setTag) && Load(sl, "slEvaluateFeature", g_api.evaluate) &&
              Load(sl, "slAllocateResources", g_api.allocate) && Load(sl, "slFreeResources", g_api.freeResources) &&
              Load(sl, "MetalStream_GetDiagnostics", g_api.diagnostics) &&
              Load(ngx, "MockNGX_GetCounters", g_api.ngxCounters) &&
              Load(ngx, "MockNGX_SetCreateResult", g_api.ngxSetCreateResult) &&
              Load(ngx, "MockNGX_SetEvaluateResult", g_api.ngxSetEvaluateResult);
    if (!ok) return false;

    sl::Preferences pref{};
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking;
    if (g_api.init(pref, sl::kSDKVersion) != sl::Result::eOk) {
        printf("slInit failed\n");
        return false;
    }
    if (g_api.setDevice(Fake_CreateDevice()) != sl::Result::eOk) {
        printf("slSetD3DDevice failed\n");
        return false;
    }

    PFN_slGetFeatureFunction getFeatureFunction = nullptr;
    if (!Load(sl, "slGetFeatureFunction", getFeatureFunction)) return false;
    void* fn = nullptr;
    getFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fn);
    g_api.dlssSetOptions = (PFN_slDLSSSetOptions)fn;
    if (!g_api.dlssSetOptions) {
        printf("slDLSSSetOptions not available\n");
        return false;
    }

    g_cmdList = Fake_CreateCommandList();
    return Ngx().initCalls > 0;
}

// -----------------------------------------------------------------------------
// Scenarios
// -----------------------------------------------------------------------------

void SteadyState() {
    printf("\n[steady state] 120 identical frames\n");
    Viewport vp(10, 1280, 720, 1920, 1080);
    vp.SetOptions(sl::DLSSMode::eMaxQuality, 1920, 1080);
    Fake_ResetCmdListStats(g_cmdList);
    auto before = Snapshot::Take();

    int badResults = 0;
    for (int i = 0; i < 120; ++i) {
        if (vp.Frame(i == 0) != sl::Result::eOk) badResults++;
    }

    auto after = Snapshot::Take();
    auto cmd = CmdStats();
    CHECK(badResults == 0, "every slEvaluateFeature returned eOk (%d failed)", badResults);
    CHECK(after.ngx.createSucceeded - before.ngx.createSucceeded == 1,
          "MetalFX feature created exactly once (%u)", after.ngx.createSucceeded - before.ngx.createSucceeded);
    CHECK(after.diag.featureRecreates == before.diag.featureRecreates,
          "no recreations (%llu)", (unsigned long long)(after.diag.featureRecreates - before.diag.featureRecreates));
    CHECK(after.ngx.evaluateSucceeded - before.ngx.evaluateSucceeded == 120,
          "NGX evaluated every frame (%u)", after.ngx.evaluateSucceeded - before.ngx.evaluateSucceeded);
    CHECK(after.ngx.resetEvaluations - before.ngx.resetEvaluations == 1,
          "history reset only on the frame that asked for it (%u)", after.ngx.resetEvaluations - before.ngx.resetEvaluations);
    CHECK(cmd.barrierCalls == 0 && cmd.copyResourceCalls == 0 && cmd.copyTextureRegionCalls == 0,
          "no barriers or copies on the success path (barriers=%u copies=%u)",
          cmd.barrierCalls, cmd.copyResourceCalls + cmd.copyTextureRegionCalls);
    CHECK(after.diag.blitFallbacks == before.diag.blitFallbacks, "no BlitGuard fallbacks");
    CHECK(after.ngx.lastEvalWidth == 1280 && after.ngx.lastEvalHeight == 720,
          "render size passed to NGX (%ux%u)", after.ngx.lastEvalWidth, after.ngx.lastEvalHeight);
    CHECK(after.ngx.lastEvalColor == vp.colorIn && after.ngx.lastEvalOutput == vp.colorOut,
          "tagged input/output resources passed to NGX");
    CHECK(after.ngx.lastCreateFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes,
          "render-resolution motion vectors create with MVLowRes (flags=0x%X)", after.ngx.lastCreateFlags);
    CHECK(after.diag.lockWaitSamples > before.diag.lockWaitSamples, "lock wait instrumentation records samples");
}

void EvaluateFailureFallsBackToBlit() {
    printf("\n[fallback] NGX EvaluateFeature fails\n");
    Viewport vp(11, 1280, 720, 1920, 1080);
    vp.SetOptions(sl::DLSSMode::eMaxQuality, 1920, 1080);
    vp.Frame();  // create the feature first
    Fake_ResetCmdListStats(g_cmdList);
    auto before = Snapshot::Take();

    g_api.ngxSetEvaluateResult(NVSDK_NGX_Result_Fail);
    sl::Result r = vp.Frame();
    g_api.ngxSetEvaluateResult(NVSDK_NGX_Result_Success);

    auto after = Snapshot::Take();
    auto cmd = CmdStats();
    CHECK(r == sl::Result::eOk, "slEvaluateFeature still returns eOk (%u)", (uint32_t)r);
    CHECK(cmd.copyTextureRegionCalls == 1 && cmd.lastCopyDst == vp.colorOut && cmd.lastCopySrc == vp.colorIn,
          "BlitGuard copied input into output (copies=%u)", cmd.copyTextureRegionCalls);
    CHECK(cmd.barrierCalls == 2, "BlitGuard issued transition + revert barriers (%u calls)", cmd.barrierCalls);
    CHECK(after.diag.blitFallbacks - before.diag.blitFallbacks == 1, "blit fallback counted");
    CHECK(after.diag.evaluateFailures - before.diag.evaluateFailures == 1, "evaluate failure counted");
}

void CreateFailureFallsBackToBlit() {
    printf("\n[fallback] NGX CreateFeature fails\n");
    Viewport vp(12, 1920, 1080, 1920, 1080);  // same size: BlitGuard uses CopyResource
    vp.SetOptions(sl::DLSSMode::eDLAA, 1920, 1080);
    Fake_ResetCmdListStats(g_cmdList);
    auto before = Snapshot::Take();

    g_api.ngxSetCreateResult(NVSDK_NGX_Result_Fail);
    sl::Result r = vp.Frame();
    g_api.ngxSetCreateResult(NVSDK_NGX_Result_Success);

    auto after = Snapshot::Take();
    auto cmd = CmdStats();
    CHECK(r == sl::Result::eOk, "slEvaluateFeature still returns eOk (%u)", (uint32_t)r);
    CHECK(cmd.copyResourceCalls == 1 && cmd.lastCopyDst == vp.colorOut, "BlitGuard copied with CopyResource");
    CHECK(after.diag.featureCreateFailures - before.diag.featureCreateFailures == 1, "create failure counted");
    CHECK(after.ngx.evaluateCalls == before.ngx.evaluateCalls, "no NGX evaluate without a feature");
}

void ModeChangeRecreatesOnceAndDrainsReleases() {
    printf("\n[mode change] Quality -> Performance\n");
    Viewport vp(13, 1280, 720, 1920, 1080);
    vp.SetOptions(sl::DLSSMode::eMaxQuality, 1920, 1080);
    for (int i = 0; i < 5; ++i) vp.Frame();
    auto before = Snapshot::Take();

    vp.SetOptions(sl::DLSSMode::eMaxPerformance, 1920, 1080);
    vp.colorIn = Fake_CreateTexture(960, 540, DXGI_FORMAT_R16G16B16A16_FLOAT);
    vp.depth = Fake_CreateTexture(960, 540, DXGI_FORMAT_R32_FLOAT);
    vp.mvec = Fake_CreateTexture(960, 540, DXGI_FORMAT_R16G16_FLOAT);
    for (int i = 0; i < 10; ++i) vp.Frame();

    auto after = Snapshot::Take();
    CHECK(after.ngx.createSucceeded - before.ngx.createSucceeded == 1,
          "exactly one new feature (%u)", after.ngx.createSucceeded - before.ngx.createSucceeded);
    CHECK(after.diag.featureRecreates - before.diag.featureRecreates == 1,
          "exactly one recreation (%llu)", (unsigned long long)(after.diag.featureRecreates - before.diag.featureRecreates));
    CHECK(after.ngx.resetEvaluations - before.ngx.resetEvaluations == 1,
          "history reset forced once on recreation (%u)", after.ngx.resetEvaluations - before.ngx.resetEvaluations);
    CHECK(after.ngx.releaseCalls - before.ngx.releaseCalls == 1, "old feature released after the deferral window");
    CHECK(after.diag.deferredReleasesPending == 0, "deferred release queue drained (%llu pending)",
          (unsigned long long)after.diag.deferredReleasesPending);
    CHECK(after.ngx.lastEvalWidth == 960, "new render size reaches NGX (%u)", after.ngx.lastEvalWidth);
}

// --- Known findings from the review (expected to stay OPEN until their batch lands) ---

void KnownAllocateThenEvaluateCreatesTwice() {
    printf("\n[M2] slAllocateResources then evaluate\n");
    Viewport vp(14, 1280, 720, 1920, 1080);
    vp.SetOptions(sl::DLSSMode::eMaxQuality, 1920, 1080);
    auto before = Snapshot::Take();
    g_api.allocate((sl::CommandBuffer*)g_cmdList, sl::kFeatureDLSS, vp.handle);
    for (int i = 0; i < 3; ++i) vp.Frame();
    auto after = Snapshot::Take();
    KNOWN("M2", after.ngx.createSucceeded - before.ngx.createSucceeded == 1,
          "one MetalFX feature for allocate + evaluate (created %u)", after.ngx.createSucceeded - before.ngx.createSucceeded);
}

void KnownFullSizeMotionVectorsLoseMVLowRes() {
    printf("\n[M1] DRS-style allocation: full-size textures, render-size extents, mvec tagged without extent\n");
    Viewport vp(15, 1920, 1080, 1920, 1080);
    vp.colorInExtent = { 0, 0, 1280, 720 };
    vp.SetOptions(sl::DLSSMode::eMaxQuality, 1920, 1080);
    vp.Frame();
    auto c = Ngx();
    KNOWN("M1", c.lastCreateFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes,
          "feature keeps MVLowRes (flags=0x%X)", c.lastCreateFlags);
}

void KnownFreeingFrameGenBreaksDlss() {
    printf("\n[S1] slFreeResources(DLSS_G) then DLSS evaluate without re-sending options\n");
    Viewport vp(16, 1280, 720, 1920, 1080);
    vp.SetOptions(sl::DLSSMode::eMaxQuality, 1920, 1080);
    for (int i = 0; i < 3; ++i) vp.Frame();
    g_api.freeResources(sl::kFeatureDLSS_G, vp.handle);
    Fake_ResetCmdListStats(g_cmdList);
    auto before = Snapshot::Take();
    sl::Result r = vp.Frame();
    auto after = Snapshot::Take();
    auto cmd = CmdStats();
    const bool outputWritten = (after.ngx.evaluateSucceeded > before.ngx.evaluateSucceeded) ||
                               cmd.copyResourceCalls + cmd.copyTextureRegionCalls > 0;
    KNOWN("S1", outputWritten, "output still produced after freeing DLSS-G (result=%u)", (uint32_t)r);
}

void KnownModeOffWritesNoOutput() {
    printf("\n[L1] evaluate while DLSSMode::eOff\n");
    Viewport vp(17, 1280, 720, 1920, 1080);
    vp.SetOptions(sl::DLSSMode::eOff, 1920, 1080);
    Fake_ResetCmdListStats(g_cmdList);
    sl::Result r = vp.Frame();
    auto cmd = CmdStats();
    KNOWN("L1", cmd.copyResourceCalls + cmd.copyTextureRegionCalls == 1,
          "eOff falls back to BlitGuard (result=%u, copies=%u)", (uint32_t)r,
          cmd.copyResourceCalls + cmd.copyTextureRegionCalls);
}

// Opt-in: METALSTREAM_MOCK_SOAK_SECONDS=N runs steady frames at ~60 fps for N seconds so the
// periodic "[stats ...]" log line (every 5 s, needs METALSTREAM_LOG=1) can be inspected.
void Soak(int seconds) {
    printf("\n[soak] %d s of steady frames\n", seconds);
    Viewport vp(20, 1280, 720, 1920, 1080);
    vp.SetOptions(sl::DLSSMode::eMaxQuality, 1920, 1080);
    auto before = Snapshot::Take();
    const DWORD end = GetTickCount() + (DWORD)seconds * 1000;
    int frames = 0;
    while ((LONG)(end - GetTickCount()) > 0) {
        vp.Frame(frames == 0);
        frames++;
        Sleep(16);
    }
    auto after = Snapshot::Take();
    CHECK(after.ngx.createSucceeded - before.ngx.createSucceeded == 1 &&
              after.diag.featureRecreates == before.diag.featureRecreates,
          "%d frames, one feature, no recreations", frames);
}

} // namespace

int main() {
    printf("MetalStream mock pipeline test\n");
    if (!Setup()) {
        printf("[FAIL] setup\n");
        return 2;
    }

    SteadyState();
    EvaluateFailureFallsBackToBlit();
    CreateFailureFallsBackToBlit();
    ModeChangeRecreatesOnceAndDrainsReleases();

    KnownAllocateThenEvaluateCreatesTwice();
    KnownFullSizeMotionVectorsLoseMVLowRes();
    KnownFreeingFrameGenBreaksDlss();
    KnownModeOffWritesNoOutput();

    if (const char* soak = getenv("METALSTREAM_MOCK_SOAK_SECONDS")) {
        Soak(atoi(soak));
    }

    printf("\n%d failure(s), %d known issue(s) open\n", g_failures, g_knownOpen);
    return g_failures == 0 ? 0 : 1;
}
