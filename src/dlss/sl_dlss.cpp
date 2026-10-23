#include <windows.h>
#include "../common/logger.h"
#include <streamline/sl.h>
#include <streamline/sl_dlss.h>
#include <string.h>

template <typename T>
static const T* findStruct(const sl::BaseStructure* base) {
    while (base) {
        if (base->structType == T::s_structType) {
            return static_cast<const T*>(base);
        }
        base = base->next;
    }
    return nullptr;
}

template <typename T>
static T* findStruct(sl::BaseStructure* base) {
    while (base) {
        if (base->structType == T::s_structType) {
            return static_cast<T*>(base);
        }
        base = base->next;
    }
    return nullptr;
}

extern "C" {

static bool slOnPluginLoad(void* params, const char* loaderJSON, const char** pluginJSON) {
    LOG_INFO("sl.dlss.dll: slOnPluginLoad called");
    if (pluginJSON) {
        static const char* s_pluginJson = 
            "{"
            "  \"id\": 0,"
            "  \"priority\": 100,"
            "  \"name\": \"sl.dlss\","
            "  \"namespace\": \"dlss\","
            "  \"sha\": \"0\","
            "  \"supportedAdapters\": 4294967295,"
            "  \"rhi\": [\"d3d11\", \"d3d12\", \"vk\"],"
            "  \"version\": {"
            "    \"major\": 3,"
            "    \"minor\": 1,"
            "    \"build\": 0"
            "  },"
            "  \"api\": {"
            "    \"major\": 2,"
            "    \"minor\": 0,"
            "    \"build\": 0"
            "  },"
            "  \"external\": {"
            "    \"hws\": { \"required\": false }"
            "  }"
            "}";
        *pluginJSON = s_pluginJson;
    }
    return true;
}

static bool slOnPluginStartup(const char* jsonConfig, void* device) {
    LOG_INFO("sl.dlss.dll: slOnPluginStartup called");
    return true;
}

static void slOnPluginShutdown() {
    LOG_INFO("sl.dlss.dll: slOnPluginShutdown called");
}

static sl::Result Internal_slDLSSGetOptimalSettings(const sl::DLSSOptions& options, sl::DLSSOptimalSettings& settings) {
    settings = {};
    float scale = 1.0f;
    switch (options.mode) {
        case sl::DLSSMode::eMaxQuality:         scale = 0.666667f; break; // Quality (1.5x)
        case sl::DLSSMode::eBalanced:           scale = 0.58f;     break; // Balanced (1.7x)
        case sl::DLSSMode::eMaxPerformance:     scale = 0.50f;     break; // Performance (2.0x)
        case sl::DLSSMode::eUltraPerformance:   scale = 0.333333f; break; // Ultra Performance (3.0x)
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

    LOG_DEBUG("sl.dlss.dll: slDLSSGetOptimalSettings mode=%u, out=%ux%u -> render=%ux%u",
             (uint32_t)options.mode, options.outputWidth, options.outputHeight, rw, rh);
    return sl::Result::eOk;
}

static sl::Result Internal_slDLSSGetState(const sl::ViewportHandle& viewport, sl::DLSSState& state) {
    state = {};
    state.estimatedVRAMUsageInBytes = 87 * 1024 * 1024;
    return sl::Result::eOk;
}

static sl::Result Internal_slDLSSSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options) {
    LOG_DEBUG("sl.dlss.dll: slDLSSSetOptions viewport=%u, mode=%u", (uint32_t)viewport, (uint32_t)options.mode);
    return sl::Result::eOk;
}

static sl::Result Internal_slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport) {
    if (feature != sl::kFeatureDLSS) {
        LOG_WARN("sl.dlss.dll: slAllocateResources unsupported feature %u", feature);
        return sl::Result::eErrorFeatureMissing;
    }
    LOG_INFO("sl.dlss.dll: slAllocateResources feature=%u, viewport=%u", feature, (uint32_t)viewport);
    return sl::Result::eOk;
}

static sl::Result Internal_slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport) {
    if (feature != sl::kFeatureDLSS) {
        LOG_WARN("sl.dlss.dll: slFreeResources unsupported feature %u", feature);
        return sl::Result::eErrorFeatureMissing;
    }
    LOG_INFO("sl.dlss.dll: slFreeResources feature=%u, viewport=%u", feature, (uint32_t)viewport);
    return sl::Result::eOk;
}

__declspec(dllexport) sl::Result slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport) {
    return Internal_slAllocateResources(cmdBuffer, feature, viewport);
}

__declspec(dllexport) sl::Result slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport) {
    return Internal_slFreeResources(feature, viewport);
}

__declspec(dllexport) sl::Result slSetData(const sl::BaseStructure* inputs, sl::CommandBuffer* cmdBuffer) {
    if (!inputs) return sl::Result::eErrorMissingInputParameter;

    const auto* opts = findStruct<sl::DLSSOptions>(inputs);
    const auto* vp = findStruct<sl::ViewportHandle>(inputs);

    uint32_t viewportId = vp ? (uint32_t)(*vp) : 0;
    if (opts) {
        LOG_DEBUG("sl.dlss.dll: slSetData viewport=%u, mode=%u, out=%ux%u",
                  viewportId, (uint32_t)opts->mode, opts->outputWidth, opts->outputHeight);
        Internal_slDLSSSetOptions(sl::ViewportHandle(viewportId), *opts);
    }
    return sl::Result::eOk;
}

__declspec(dllexport) sl::Result slGetData(const sl::BaseStructure* inputs, sl::BaseStructure* output, sl::CommandBuffer* cmdBuffer) {
    if (!output) return sl::Result::eErrorMissingInputParameter;

    auto* settings = findStruct<sl::DLSSOptimalSettings>(output);
    auto* state = findStruct<sl::DLSSState>(output);
    const auto* opts = inputs ? findStruct<sl::DLSSOptions>(inputs) : nullptr;
    const auto* vp = inputs ? findStruct<sl::ViewportHandle>(inputs) : nullptr;

    if ((!opts || !settings) && !state) {
        LOG_ERROR("sl.dlss.dll: slGetData missing required inputs/output parameters");
        return sl::Result::eErrorMissingInputParameter;
    }

    uint32_t viewportId = vp ? (uint32_t)(*vp) : 0;

    if (settings && opts) {
        Internal_slDLSSGetOptimalSettings(*opts, *settings);
    }
    if (state) {
        Internal_slDLSSGetState(sl::ViewportHandle(viewportId), *state);
    }
    return sl::Result::eOk;
}

static sl::Result Internal_slIsSupported(const sl::AdapterInfo& adapterInfo) {
    return sl::Result::eOk;
}

__declspec(dllexport) void* slGetPluginFunction(const char* functionName) {
    if (!functionName) return nullptr;
    LOG_DEBUG("sl.dlss.dll: slGetPluginFunction('%s')", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0) return (void*)&slOnPluginLoad;
    if (strcmp(functionName, "slOnPluginStartup") == 0) return (void*)&slOnPluginStartup;
    if (strcmp(functionName, "slOnPluginShutdown") == 0) return (void*)&slOnPluginShutdown;
    if (strcmp(functionName, "slDLSSGetOptimalSettings") == 0) return (void*)&Internal_slDLSSGetOptimalSettings;
    if (strcmp(functionName, "slDLSSGetState") == 0) return (void*)&Internal_slDLSSGetState;
    if (strcmp(functionName, "slDLSSSetOptions") == 0) return (void*)&Internal_slDLSSSetOptions;
    if (strcmp(functionName, "slAllocateResources") == 0) return (void*)&Internal_slAllocateResources;
    if (strcmp(functionName, "slFreeResources") == 0) return (void*)&Internal_slFreeResources;
    if (strcmp(functionName, "slSetData") == 0) return (void*)&slSetData;
    if (strcmp(functionName, "slGetData") == 0) return (void*)&slGetData;
    if (strcmp(functionName, "slIsSupported") == 0) return (void*)&Internal_slIsSupported;

    return nullptr;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            LOG_INFO("sl.dlss.dll loaded");
            break;
        case DLL_PROCESS_DETACH:
            // NOTE: Do not perform any I/O, file logging, or mutex locks inside DllMain(DLL_PROCESS_DETACH)
            // to prevent loader lock deadlocks during Wine/Proton module unload.
            break;
    }
    return TRUE;
}

}
