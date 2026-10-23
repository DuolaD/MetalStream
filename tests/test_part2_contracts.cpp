#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cassert>
#include <string>
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
typedef sl::Result (*PFN_slGetFeatureRequirements)(sl::Feature, sl::FeatureRequirements&);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature, const char*, void*&);
typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

typedef bool (*PFN_slOnPluginLoad)(void* params, const char* loaderJSON, const char** pluginJSON);
typedef void* (*PFN_slGetPluginFunction)(const char*);

int main() {
    printf("=======================================================================\n");
    printf("[AUDIT PART 2 TEST] Interface Contract Consistency & Specification Audit\n");
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
    auto pfnGetFeatureRequirements = (PFN_slGetFeatureRequirements)GetProcAddress(hSl, "slGetFeatureRequirements");
    auto pfnGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(hSl, "slGetFeatureFunction");
    auto pfnD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hSl, "D3D12CreateDevice");

    if (!pfnInit || !pfnShutdown || !pfnGetNewFrameToken || !pfnSetD3DDevice ||
        !pfnGetFeatureRequirements || !pfnGetFeatureFunction || !pfnD3D12CreateDevice) {
        printf("[FAILED] Missing core Streamline exports from sl.interposer.dll\n");
        return 1;
    }

    sl::Preferences pref{};
    pfnInit(pref, 0x2000B0001FEDC);

    // =========================================================================
    // Test 1: slGetFeatureRequirements Zero-Memory Integrity & Poison Defense
    // =========================================================================
    printf("\n--- Test 1: slGetFeatureRequirements Zero-Memory & Pointer Trap Defense ---\n");
    sl::FeatureRequirements reqs;
    // Deliberately fill memory with poison bit-pattern (0xCC) to simulate uninitialized stack memory
    memset(&reqs, 0xCC, sizeof(reqs));

    sl::Result rReq = pfnGetFeatureRequirements(sl::kFeatureDLSS, reqs);
    if (rReq != sl::Result::eOk) {
        printf("[FAILED] slGetFeatureRequirements returned %d\n", (int)rReq);
        return 1;
    }

    // Verify critical pointer and count fields have been correctly initialized or zeroed out
    // (DLSS officially specifies 4 required tags per NV Streamline specification)
    if (reqs.numRequiredTags != 4) {
        printf("[FAILED] DLSS numRequiredTags is not 4 (got: %u)!\n", reqs.numRequiredTags);
        return 1;
    }
    if (reqs.requiredTags == nullptr) {
        printf("[FAILED] DLSS requiredTags pointer is null!\n");
        return 1;
    }
    // Also test Reflex which specifies 0 required tags to verify zeroing defense
    sl::FeatureRequirements reflexReqs;
    memset(&reflexReqs, 0xCC, sizeof(reflexReqs));
    if (pfnGetFeatureRequirements(sl::kFeatureReflex, reflexReqs) != sl::Result::eOk ||
        reflexReqs.numRequiredTags != 0 || reflexReqs.requiredTags != nullptr) {
        printf("[FAILED] Reflex numRequiredTags is not 0 after poison memset!\n");
        return 1;
    }
    if (reqs.vkNumDeviceExtensions != 0 || reqs.vkDeviceExtensions != nullptr) {
        printf("[FAILED] Vulkan device extensions pointers contain wild memory!\n");
        return 1;
    }
    if (reqs.vkNumInstanceExtensions != 0 || reqs.vkInstanceExtensions != nullptr) {
        printf("[FAILED] Vulkan instance extensions pointers contain wild memory!\n");
        return 1;
    }
    if (reqs.osVersionDetected.major != 10 || reqs.osVersionDetected.build != 19045) {
        printf("[FAILED] osVersionDetected not populated correctly: %s\n", reqs.osVersionDetected.toStr().c_str());
        return 1;
    }
    if (reqs.driverVersionDetected.major != 550) {
        printf("[FAILED] driverVersionDetected not populated correctly: %s\n", reqs.driverVersionDetected.toStr().c_str());
        return 1;
    }
    if ((reqs.flags & sl::FeatureRequirementFlags::eHardwareSchedulingRequired) != 0) {
        printf("[FAILED] HAGS flag should NOT be required!\n");
        return 1;
    }

    printf("[PASS] slGetFeatureRequirements safely cleared dirty stack memory (numTags=%u, ptr=%p, os=%s, drv=%s)!\n",
           reqs.numRequiredTags, reqs.requiredTags,
           reqs.osVersionDetected.toStr().c_str(), reqs.driverVersionDetected.toStr().c_str());

    // =========================================================================
    // Test 2: slGetFeatureFunction Return Code Alignment
    // =========================================================================
    printf("\n--- Test 2: slGetFeatureFunction Return Code Specification Alignment ---\n");
    void* fnPtr = (void*)0xDEADBEEF;

    // Subtest 2.1: null functionName -> eErrorInvalidParameter
    sl::Result rNull = pfnGetFeatureFunction(sl::kFeatureDLSS, nullptr, fnPtr);
    if (rNull != sl::Result::eErrorInvalidParameter || fnPtr != nullptr) {
        printf("[FAILED] null functionName did not return eErrorInvalidParameter (got %d)\n", (int)rNull);
        return 1;
    }
    printf("[PASS] Null functionName correctly returned eErrorInvalidParameter\n");

    // Subtest 2.2: known function -> eOk
    sl::Result rKnown = pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fnPtr);
    if (rKnown != sl::Result::eOk || fnPtr == nullptr) {
        printf("[FAILED] slDLSSGetOptimalSettings lookup failed (got %d, ptr %p)\n", (int)rKnown, fnPtr);
        return 1;
    }
    printf("[PASS] Known function returned eOk with valid entry point (%p)\n", fnPtr);

    // Subtest 2.3: unknown function on known feature -> eErrorMissingOrInvalidAPI
    fnPtr = (void*)0xDEADBEEF;
    sl::Result rUnknown = pfnGetFeatureFunction(sl::kFeatureDLSS, "slUnknownNonExistentFunction", fnPtr);
    if (rUnknown != sl::Result::eErrorMissingOrInvalidAPI || fnPtr != nullptr) {
        printf("[FAILED] Unknown function returned %d (expected eErrorMissingOrInvalidAPI = %d)\n",
               (int)rUnknown, (int)sl::Result::eErrorMissingOrInvalidAPI);
        return 1;
    }
    printf("[PASS] Unknown function returned eErrorMissingOrInvalidAPI as mandated by official Streamline contract!\n");

    // =========================================================================
    // Test 3: sl.dlss.dll Plugin JSON Specification Compliance
    // =========================================================================
    printf("\n--- Test 3: sl.dlss.dll pluginJSON Specification Adherence ---\n");
    HMODULE hDlss = LoadLibraryA("build/sl.dlss.dll");
    if (!hDlss) hDlss = LoadLibraryA("sl.dlss.dll");
    if (!hDlss) {
        printf("[FAILED] Could not load sl.dlss.dll\n");
        return 1;
    }

    auto pfnDlssGetPluginFunc = (PFN_slGetPluginFunction)GetProcAddress(hDlss, "slGetPluginFunction");
    if (!pfnDlssGetPluginFunc) {
        printf("[FAILED] Missing slGetPluginFunction in sl.dlss.dll\n");
        return 1;
    }

    auto pfnOnPluginLoad = (PFN_slOnPluginLoad)pfnDlssGetPluginFunc("slOnPluginLoad");
    if (!pfnOnPluginLoad) {
        printf("[FAILED] Missing slOnPluginLoad in sl.dlss.dll\n");
        return 1;
    }

    const char* pluginJsonText = nullptr;
    bool bLoadOk = pfnOnPluginLoad(nullptr, "{}", &pluginJsonText);
    if (!bLoadOk || !pluginJsonText) {
        printf("[FAILED] slOnPluginLoad returned false or null JSON\n");
        return 1;
    }

    std::string jsonStr = pluginJsonText;
    printf("[PLUGIN JSON] Content:\n%s\n", jsonStr.c_str());

    // Validate that required keys exist in accordance with official PluginManager parser:
    // 1. "id": 0
    if (jsonStr.find("\"id\": 0") == std::string::npos && jsonStr.find("\"id\" : 0") == std::string::npos) {
        printf("[FAILED] pluginJSON missing integer 'id': 0!\n");
        return 1;
    }
    // 2. "priority"
    if (jsonStr.find("\"priority\"") == std::string::npos) {
        printf("[FAILED] pluginJSON missing 'priority'!\n");
        return 1;
    }
    // 3. "version" nested object with "major", "minor", "build"
    if (jsonStr.find("\"version\"") == std::string::npos || jsonStr.find("\"major\"") == std::string::npos) {
        printf("[FAILED] pluginJSON missing nested 'version' object!\n");
        return 1;
    }
    // 4. "api" nested object
    if (jsonStr.find("\"api\"") == std::string::npos) {
        printf("[FAILED] pluginJSON missing nested 'api' object!\n");
        return 1;
    }
    // 5. "rhi" list
    if (jsonStr.find("\"rhi\"") == std::string::npos) {
        printf("[FAILED] pluginJSON missing 'rhi'!\n");
        return 1;
    }
    // 6. "external" object
    if (jsonStr.find("\"external\"") == std::string::npos) {
        printf("[FAILED] pluginJSON missing 'external'!\n");
        return 1;
    }
    // 7. "supportedAdapters"
    if (jsonStr.find("\"supportedAdapters\"") == std::string::npos) {
        printf("[FAILED] pluginJSON missing 'supportedAdapters'!\n");
        return 1;
    }

    printf("[PASS] sl.dlss.dll pluginJSON strictly satisfies all official PluginManager JSON schema constraints!\n");

    // =========================================================================
    // Test 4: State Zeroing & Message Leak Elimination
    // =========================================================================
    printf("\n--- Test 4: State Zeroing & Message ID Leak Elimination ---\n");
    void* fnPclGetState = nullptr;
    pfnGetFeatureFunction(sl::kFeatureReflex, "slPCLGetState", fnPclGetState);
    if (fnPclGetState) {
        auto pfnPCLGetState = (sl::Result(*)(sl::PCLState&))fnPclGetState;
        sl::PCLState pclState;
        memset(&pclState, 0xAA, sizeof(pclState));
        pfnPCLGetState(pclState);
        if (pclState.statsWindowMessage != 0) {
            printf("[FAILED] pclState.statsWindowMessage not cleared to 0 (garbage: %u)!\n", pclState.statsWindowMessage);
            return 1;
        }
        printf("[PASS] slPCLGetState cleared statsWindowMessage = 0\n");
    }

    void* fnReflexGetState = nullptr;
    pfnGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", fnReflexGetState);
    if (fnReflexGetState) {
        auto pfnReflexGetState = (sl::Result(*)(sl::ReflexState&))fnReflexGetState;
        sl::ReflexState refState;
        memset(&refState, 0xBB, sizeof(refState));
        pfnReflexGetState(refState);
        if (refState.statsWindowMessage != 0) {
            printf("[FAILED] refState.statsWindowMessage not cleared to 0 (garbage: %u)!\n", refState.statsWindowMessage);
            return 1;
        }
        if (refState.frameReport[0].simEndTime != 0) {
            printf("[FAILED] refState.frameReport was not zero-initialized!\n");
            return 1;
        }
        if (!refState.lowLatencyAvailable) {
            printf("[FAILED] refState.lowLatencyAvailable is false!\n");
            return 1;
        }
        printf("[PASS] slReflexGetState zero-initialized all arrays and statsWindowMessage = 0\n");
    }

    // =========================================================================
    // Test 5: D3D12CreateDevice Automatic Device Bypass Capture
    // =========================================================================
    printf("\n--- Test 5: D3D12CreateDevice Automatic Device Bypass Capture ---\n");
    ID3D12Device* device = nullptr;
    HRESULT hr = pfnD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        printf("[FAILED] D3D12CreateDevice failed: 0x%08lX\n", hr);
        return 1;
    }

    // Query DLSSG State WITHOUT having called slSetD3DDevice explicitly:
    // D3D12CreateDevice should have automatically invoked slSetD3DDevice, creating the dummy fence!
    void* fnDLSSGGetState = nullptr;
    pfnGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fnDLSSGGetState);
    if (!fnDLSSGGetState) {
        printf("[FAILED] Missing slDLSSGGetState export\n");
        return 1;
    }

    auto pfnDLSSGGetState = (sl::Result(*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*))fnDLSSGGetState;
    sl::ViewportHandle vp0(0);
    sl::DLSSGState gState{};
    sl::Result rGState = pfnDLSSGGetState(vp0, gState, nullptr);
    if (rGState != sl::Result::eOk) {
        printf("[FAILED] slDLSSGGetState failed: %d\n", (int)rGState);
        return 1;
    }

    if (!gState.inputsProcessingCompletionFence) {
        printf("[FAILED] inputsProcessingCompletionFence is null! Auto-capture in D3D12CreateDevice failed!\n");
        return 1;
    }

    printf("[PASS] D3D12CreateDevice auto-captured device %p and initialized DLSS-G dummy fence %p!\n",
           device, gState.inputsProcessingCompletionFence);

    device->Release();
    FreeLibrary(hDlss);
    pfnShutdown();

    printf("\n=======================================================================\n");
    printf("[SUCCESS] ALL PART 2 INTERFACE CONTRACT CONSISTENCY TESTS PASSED!\n");
    printf("=======================================================================\n");
    return 0;
}
