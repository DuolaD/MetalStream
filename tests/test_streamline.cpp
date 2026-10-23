#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <streamline/sl.h>
#include <streamline/sl_dlss.h>
#include <streamline/sl_dlss_g.h>
#include <streamline/sl_reflex.h>
#include <streamline/sl_pcl.h>

int main() {
    printf("[TEST] Loading sl.interposer.dll...\n");
    HMODULE hMod = LoadLibraryA("sl.interposer.dll");
    if (!hMod) {
        printf("[TEST ERROR] Failed to load sl.interposer.dll, error: %lu\n", GetLastError());
        return 1;
    }
    printf("[TEST OK] sl.interposer.dll loaded successfully: %p\n", hMod);

    auto pSlInit = (decltype(&slInit))GetProcAddress(hMod, "slInit");
    auto pSlShutdown = (decltype(&slShutdown))GetProcAddress(hMod, "slShutdown");
    auto pSlIsFeatureSupported = (decltype(&slIsFeatureSupported))GetProcAddress(hMod, "slIsFeatureSupported");
    auto pSlGetFeatureFunction = (decltype(&slGetFeatureFunction))GetProcAddress(hMod, "slGetFeatureFunction");
    auto pSlEvaluateFeature = (decltype(&slEvaluateFeature))GetProcAddress(hMod, "slEvaluateFeature");
    auto pSlSetTag = (decltype(&slSetTag))GetProcAddress(hMod, "slSetTag");
    auto pSlGetFeatureRequirements = (decltype(&slGetFeatureRequirements))GetProcAddress(hMod, "slGetFeatureRequirements");
    auto pSlGetNewFrameToken = (decltype(&slGetNewFrameToken))GetProcAddress(hMod, "slGetNewFrameToken");

    if (!pSlInit || !pSlShutdown || !pSlIsFeatureSupported || !pSlGetFeatureFunction || !pSlGetNewFrameToken) {
        printf("[TEST ERROR] Missing core exports in sl.interposer.dll\n");
        return 1;
    }
    printf("[TEST OK] Found all core exports\n");

    sl::FrameToken* pFrameToken = nullptr;
    sl::Result res = pSlGetNewFrameToken(pFrameToken, nullptr);
    printf("[TEST] slGetNewFrameToken before slInit result: %u (expected %u)\n",
           (uint32_t)res, (uint32_t)sl::Result::eErrorNotInitialized);
    if (res != sl::Result::eErrorNotInitialized) return 1;

    // 1. slInit
    sl::Preferences pref{};
    pref.applicationId = 12345;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    res = pSlInit(pref, sl::kSDKVersion);
    printf("[TEST] slInit result: %u (expected 0)\n", (uint32_t)res);
    if (res != sl::Result::eOk) return 1;

    res = pSlGetNewFrameToken(pFrameToken, nullptr);
    printf("[TEST] slGetNewFrameToken after slInit result: %u, token: %p\n", (uint32_t)res, pFrameToken);
    if (res != sl::Result::eOk || !pFrameToken) return 1;

    // 2. slIsFeatureSupported
    sl::AdapterInfo adapter{};
    res = pSlIsFeatureSupported(sl::kFeatureDLSS, adapter);
    printf("[TEST] slIsFeatureSupported(DLSS) result: %u (expected 0)\n", (uint32_t)res);
    if (res != sl::Result::eOk) return 1;

    res = pSlIsFeatureSupported(sl::kFeatureDLSS_G, adapter);
    printf("[TEST] slIsFeatureSupported(DLSS_G) result: %u (expected 0)\n", (uint32_t)res);
    if (res != sl::Result::eOk) return 1;

    // 3. slGetFeatureRequirements (Check HAGS spoofing)
    sl::FeatureRequirements reqs{};
    res = pSlGetFeatureRequirements(sl::kFeatureDLSS_G, reqs);
    printf("[TEST] slGetFeatureRequirements result: %u, flags: 0x%X\n", (uint32_t)res, (uint32_t)reqs.flags);
    if (res != sl::Result::eOk) return 1;
    if ((reqs.flags & sl::FeatureRequirementFlags::eHardwareSchedulingRequired) != 0) {
        printf("[TEST ERROR] Hardware scheduling flag was unexpectedly set!\n");
        return 1;
    }
    printf("[TEST OK] HAGS requirement check passed: eHardwareSchedulingRequired is false\n");

    // 4. slGetFeatureFunction - slDLSSGetOptimalSettings
    void* pOptFunc = nullptr;
    res = pSlGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", pOptFunc);
    printf("[TEST] slGetFeatureFunction('slDLSSGetOptimalSettings') result: %u, ptr: %p\n", (uint32_t)res, pOptFunc);
    if (res != sl::Result::eOk || !pOptFunc) {
        printf("[TEST ERROR] slGetFeatureFunction('slDLSSGetOptimalSettings') failed!\n");
        return 1;
    }

    auto pfnDLSSGetOptimalSettings = (PFun_slDLSSGetOptimalSettings*)pOptFunc;
    sl::DLSSOptions dlssOpt{};
    dlssOpt.outputWidth = 1920;
    dlssOpt.outputHeight = 1080;
    dlssOpt.mode = sl::DLSSMode::eMaxQuality;

    sl::DLSSOptimalSettings dlssSettings{};
    res = pfnDLSSGetOptimalSettings(dlssOpt, dlssSettings);
    printf("[TEST] slDLSSGetOptimalSettings(1920x1080, eMaxQuality) -> result: %u, render: %ux%u (min: %ux%u, max: %ux%u)\n",
           (uint32_t)res, dlssSettings.optimalRenderWidth, dlssSettings.optimalRenderHeight,
           dlssSettings.renderWidthMin, dlssSettings.renderHeightMin,
           dlssSettings.renderWidthMax, dlssSettings.renderHeightMax);
    if (res != sl::Result::eOk || dlssSettings.optimalRenderWidth == 0 || dlssSettings.optimalRenderHeight == 0) {
        printf("[TEST ERROR] Invalid optimal render dimensions!\n");
        return 1;
    }
    if ((dlssSettings.optimalRenderWidth & 1) != 0 || (dlssSettings.optimalRenderHeight & 1) != 0) {
        printf("[TEST ERROR] Render dimensions are not even!\n");
        return 1;
    }

    // Test other modes
    dlssOpt.mode = sl::DLSSMode::eMaxPerformance;
    res = pfnDLSSGetOptimalSettings(dlssOpt, dlssSettings);
    printf("[TEST] slDLSSGetOptimalSettings(1920x1080, eMaxPerformance) -> render: %ux%u\n",
           dlssSettings.optimalRenderWidth, dlssSettings.optimalRenderHeight);
    if (res != sl::Result::eOk || dlssSettings.optimalRenderWidth != 960 || dlssSettings.optimalRenderHeight != 540) {
        printf("[TEST ERROR] Unexpected performance resolution! Expected 960x540, got %ux%u\n",
               dlssSettings.optimalRenderWidth, dlssSettings.optimalRenderHeight);
        return 1;
    }

    // 5. slGetFeatureFunction - slDLSSSetOptions
    void* pSetOptFunc = nullptr;
    res = pSlGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", pSetOptFunc);
    printf("[TEST] slGetFeatureFunction('slDLSSSetOptions') result: %u, ptr: %p\n", (uint32_t)res, pSetOptFunc);
    if (res != sl::Result::eOk || !pSetOptFunc) return 1;

    sl::ViewportHandle vp(0);
    auto pfnDLSSSetOptions = (PFun_slDLSSSetOptions*)pSetOptFunc;
    res = pfnDLSSSetOptions(vp, dlssOpt);
    printf("[TEST] slDLSSSetOptions result: %u\n", (uint32_t)res);
    if (res != sl::Result::eOk) return 1;

    // 6. slGetFeatureFunction - slDLSSGGetState & slDLSSGSetOptions
    void* pDLSSGStateFunc = nullptr;
    res = pSlGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", pDLSSGStateFunc);
    printf("[TEST] slGetFeatureFunction('slDLSSGGetState') result: %u, ptr: %p\n", (uint32_t)res, pDLSSGStateFunc);
    if (res != sl::Result::eOk || !pDLSSGStateFunc) return 1;

    auto pfnDLSSGGetState = (PFun_slDLSSGGetState*)pDLSSGStateFunc;
    sl::DLSSGState dlssgState{};
    res = pfnDLSSGGetState(vp, dlssgState, nullptr);
    printf("[TEST] slDLSSGGetState result: %u, status: %u, numFramesToGenerateMax: %u\n",
           (uint32_t)res, (uint32_t)dlssgState.status, dlssgState.numFramesToGenerateMax);
    if (res != sl::Result::eOk || dlssgState.status != sl::DLSSGStatus::eOk || dlssgState.numFramesToGenerateMax < 1) {
        printf("[TEST ERROR] Invalid DLSSGState!\n");
        return 1;
    }

    void* pDLSSGSetOptFunc = nullptr;
    res = pSlGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", pDLSSGSetOptFunc);
    printf("[TEST] slGetFeatureFunction('slDLSSGSetOptions') result: %u, ptr: %p\n", (uint32_t)res, pDLSSGSetOptFunc);
    if (res != sl::Result::eOk || !pDLSSGSetOptFunc) return 1;

    sl::DLSSGOptions dlssgOpt{};
    dlssgOpt.mode = sl::DLSSGMode::eOn;
    dlssgOpt.numFramesToGenerate = 1;
    auto pfnDLSSGSetOptions = (PFun_slDLSSGSetOptions*)pDLSSGSetOptFunc;
    res = pfnDLSSGSetOptions(vp, dlssgOpt);
    printf("[TEST] slDLSSGSetOptions result: %u\n", (uint32_t)res);
    if (res != sl::Result::eOk) return 1;

    // 7. slGetFeatureFunction - slReflexSleep & slPCLSetMarker
    void* pReflexSleepFunc = nullptr;
    res = pSlGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", pReflexSleepFunc);
    printf("[TEST] slGetFeatureFunction('slReflexSleep') result: %u, ptr: %p\n", (uint32_t)res, pReflexSleepFunc);
    if (res != sl::Result::eOk || !pReflexSleepFunc) return 1;

    auto pfnReflexSleep = (PFun_slReflexSleep*)pReflexSleepFunc;
    res = pfnReflexSleep(*pFrameToken);
    printf("[TEST] slReflexSleep result: %u\n", (uint32_t)res);
    if (res != sl::Result::eOk) return 1;

    void* pPCLMarkerFunc = nullptr;
    res = pSlGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", pPCLMarkerFunc);
    printf("[TEST] slGetFeatureFunction('slPCLSetMarker') result: %u, ptr: %p\n", (uint32_t)res, pPCLMarkerFunc);
    if (res != sl::Result::eOk || !pPCLMarkerFunc) return 1;

    auto pfnPCLSetMarker = (PFun_slPCLSetMarker*)pPCLMarkerFunc;
    res = pfnPCLSetMarker(sl::PCLMarker::eSimulationStart, *pFrameToken);
    printf("[TEST] slPCLSetMarker result: %u\n", (uint32_t)res);
    if (res != sl::Result::eOk) return 1;

    // 8. slEvaluateFeature
    res = pSlEvaluateFeature(sl::kFeatureDLSS, *pFrameToken, nullptr, 0, nullptr);
    printf("[TEST] slEvaluateFeature(DLSS, cmdBuffer=null) result: %u (expected %u)\n",
           (uint32_t)res, (uint32_t)sl::Result::eErrorMissingInputParameter);
    if (res != sl::Result::eErrorMissingInputParameter) return 1;

    res = pSlEvaluateFeature(sl::kFeatureDLSS_G, *pFrameToken, nullptr, 0, nullptr);
    printf("[TEST] slEvaluateFeature(DLSS_G, cmdBuffer=null) result: %u (expected 0)\n", (uint32_t)res);
    if (res != sl::Result::eOk) return 1;

    // 9. slShutdown
    res = pSlShutdown();
    printf("[TEST] slShutdown result: %u (expected 0)\n", (uint32_t)res);
    if (res != sl::Result::eOk) return 1;

    printf("\n[SUCCESS] ALL STREAMLINE UNIT & INTEGRATION TESTS PASSED!\n");
    return 0;
}
