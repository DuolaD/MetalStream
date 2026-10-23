#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <streamline/sl.h>
#include <streamline/sl_dlss.h>
#include <streamline/sl_dlss_g.h>
#include <streamline/sl_reflex.h>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <vector>

typedef sl::Result (*PFN_slInit)(const sl::Preferences& pref, uint64_t sdkVersion);
typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slIsFeatureSupported)(sl::Feature feature, const sl::AdapterInfo& adapterInfo);
typedef sl::Result (*PFN_slIsFeatureLoaded)(sl::Feature feature, bool& loaded);
typedef sl::Result (*PFN_slSetFeatureLoaded)(sl::Feature feature, bool loaded);
typedef sl::Result (*PFN_slGetFeatureRequirements)(sl::Feature feature, sl::FeatureRequirements& requirements);
typedef sl::Result (*PFN_slGetFeatureVersion)(sl::Feature feature, sl::FeatureVersion& version);
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*& token, const uint32_t* frameIndex);
typedef sl::Result (*PFN_slGetNativeInterface)(void* proxyInterface, void** baseInterface);
typedef sl::Result (*PFN_slUpgradeInterface)(void** baseInterface);
typedef sl::Result (*PFN_slSetD3DDevice)(void* d3dDevice);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature feature, const char* functionName, void*& function);
typedef sl::Result (*PFN_slSetData)(const sl::BaseStructure* inputs, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slGetData)(const sl::BaseStructure* inputs, sl::BaseStructure* output, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slAllocateResources)(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport);
typedef sl::Result (*PFN_slFreeResources)(sl::Feature feature, const sl::ViewportHandle& viewport);
typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer);

int main() {
    printf("=======================================================================\n");
    printf("[AUDIT 6 VERIFICATION] Round 6 API Contract & Lifecycle Verification\n");
    printf("=======================================================================\n");

    HMODULE hSl = LoadLibraryA("build/sl.interposer.dll");
    if (!hSl) hSl = LoadLibraryA("sl.interposer.dll");
    if (!hSl) {
        printf("[FAILED] Could not load sl.interposer.dll\n");
        return 1;
    }

    auto pfnInit = (PFN_slInit)GetProcAddress(hSl, "slInit");
    auto pfnShutdown = (PFN_slShutdown)GetProcAddress(hSl, "slShutdown");
    auto pfnIsFeatureSupported = (PFN_slIsFeatureSupported)GetProcAddress(hSl, "slIsFeatureSupported");
    auto pfnIsFeatureLoaded = (PFN_slIsFeatureLoaded)GetProcAddress(hSl, "slIsFeatureLoaded");
    auto pfnSetFeatureLoaded = (PFN_slSetFeatureLoaded)GetProcAddress(hSl, "slSetFeatureLoaded");
    auto pfnGetFeatureRequirements = (PFN_slGetFeatureRequirements)GetProcAddress(hSl, "slGetFeatureRequirements");
    auto pfnGetFeatureVersion = (PFN_slGetFeatureVersion)GetProcAddress(hSl, "slGetFeatureVersion");
    auto pfnGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(hSl, "slGetNewFrameToken");
    auto pfnGetNativeInterface = (PFN_slGetNativeInterface)GetProcAddress(hSl, "slGetNativeInterface");
    auto pfnUpgradeInterface = (PFN_slUpgradeInterface)GetProcAddress(hSl, "slUpgradeInterface");
    auto pfnSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(hSl, "slSetD3DDevice");
    auto pfnGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(hSl, "slGetFeatureFunction");
    auto pfnSetData = (PFN_slSetData)GetProcAddress(hSl, "slSetData");
    auto pfnGetData = (PFN_slGetData)GetProcAddress(hSl, "slGetData");
    auto pfnAllocateResources = (PFN_slAllocateResources)GetProcAddress(hSl, "slAllocateResources");
    auto pfnFreeResources = (PFN_slFreeResources)GetProcAddress(hSl, "slFreeResources");
    auto pfnEvaluateFeature = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");

    assert(pfnInit && pfnShutdown && pfnIsFeatureSupported && pfnIsFeatureLoaded && pfnSetFeatureLoaded);
    assert(pfnGetFeatureRequirements && pfnGetFeatureVersion && pfnGetNewFrameToken);
    assert(pfnGetNativeInterface && pfnUpgradeInterface && pfnSetD3DDevice && pfnGetFeatureFunction);
    assert(pfnSetData && pfnGetData && pfnAllocateResources && pfnFreeResources && pfnEvaluateFeature);

    // =========================================================================
    // Test 1: Uninitialized Calls Guard (slValidateState parity before slInit)
    // =========================================================================
    printf("\n--- Test 1: slValidateState Contract Parity (Uninitialized State) ---\n");

    // 1. slShutdown before slInit must return eErrorNotInitialized
    sl::Result rShutdown = pfnShutdown();
    assert(rShutdown == sl::Result::eErrorNotInitialized);
    printf("[PASS] slShutdown returns eErrorNotInitialized when not initialized\n");

    // 2. slIsFeatureSupported before slInit
    sl::AdapterInfo adapter{};
    sl::Result rSupp = pfnIsFeatureSupported(sl::kFeatureDLSS, adapter);
    assert(rSupp == sl::Result::eErrorNotInitialized);
    printf("[PASS] slIsFeatureSupported returns eErrorNotInitialized when not initialized\n");

    // 3. slIsFeatureLoaded before slInit
    bool bLoaded = true;
    sl::Result rLoaded = pfnIsFeatureLoaded(sl::kFeatureDLSS, bLoaded);
    assert(rLoaded == sl::Result::eErrorNotInitialized);
    printf("[PASS] slIsFeatureLoaded returns eErrorNotInitialized when not initialized\n");

    // 4. slSetFeatureLoaded before slInit
    sl::Result rSetLoaded = pfnSetFeatureLoaded(sl::kFeatureDLSS, true);
    assert(rSetLoaded == sl::Result::eErrorNotInitialized);
    printf("[PASS] slSetFeatureLoaded returns eErrorNotInitialized when not initialized\n");

    // 5. slGetFeatureRequirements before slInit
    sl::FeatureRequirements reqs{};
    sl::Result rReqs = pfnGetFeatureRequirements(sl::kFeatureDLSS, reqs);
    assert(rReqs == sl::Result::eErrorNotInitialized);
    printf("[PASS] slGetFeatureRequirements returns eErrorNotInitialized when not initialized\n");

    // 6. slGetFeatureVersion before slInit
    sl::FeatureVersion ver{};
    sl::Result rVer = pfnGetFeatureVersion(sl::kFeatureDLSS, ver);
    assert(rVer == sl::Result::eErrorNotInitialized);
    printf("[PASS] slGetFeatureVersion returns eErrorNotInitialized when not initialized\n");

    // 7. slGetNewFrameToken before slInit
    sl::FrameToken* pToken = nullptr;
    sl::Result rToken = pfnGetNewFrameToken(pToken, nullptr);
    assert(rToken == sl::Result::eErrorNotInitialized);
    printf("[PASS] slGetNewFrameToken returns eErrorNotInitialized when not initialized\n");

    // 8. slGetNativeInterface with null parameters
    sl::Result rNative = pfnGetNativeInterface(nullptr, nullptr);
    assert(rNative == sl::Result::eErrorInvalidParameter);
    printf("[PASS] slGetNativeInterface returns eErrorInvalidParameter for null\n");

    // 9. slUpgradeInterface with null
    sl::Result rUpgrade = pfnUpgradeInterface(nullptr);
    assert(rUpgrade == sl::Result::eErrorMissingInputParameter);
    printf("[PASS] slUpgradeInterface returns eErrorMissingInputParameter for null\n");

    // 10. slSetD3DDevice before slInit
    void* dummy = (void*)0x1234;
    sl::Result rSetDev = pfnSetD3DDevice(dummy);
    assert(rSetDev == sl::Result::eErrorNotInitialized);
    printf("[PASS] slSetD3DDevice returns eErrorNotInitialized when not initialized\n");

    // 11. slGetFeatureFunction before slInit
    void* fnPtr = nullptr;
    sl::Result rFunc = pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fnPtr);
    assert(rFunc == sl::Result::eErrorNotInitialized);
    printf("[PASS] slGetFeatureFunction returns eErrorNotInitialized when not initialized\n");

    // 12. slSetData and slGetData before slInit
    sl::ViewportHandle vp(0);
    sl::Result rSetData = pfnSetData(&vp, nullptr);
    assert(rSetData == sl::Result::eErrorNotInitialized);
    sl::DLSSState dlssState{};
    sl::Result rGetData = pfnGetData(&vp, &dlssState, nullptr);
    assert(rGetData == sl::Result::eErrorNotInitialized);
    printf("[PASS] slSetData and slGetData return eErrorNotInitialized when not initialized\n");

    // =========================================================================
    // Test 2: slInit Initialization
    // =========================================================================
    printf("\n--- Test 2: slInit Successful Lifecycle Transition ---\n");
    sl::Preferences pref{};
    pref.applicationId = 98765;
    sl::Result rInit = pfnInit(pref, sl::kSDKVersion);
    assert(rInit == sl::Result::eOk);
    printf("[PASS] slInit successfully transitioned system to initialized state\n");

    // =========================================================================
    // Test 3: Unknown / Invalid Feature Parity (eErrorFeatureMissing)
    // =========================================================================
    printf("\n--- Test 3: Unknown Feature Contract Parity (eErrorFeatureMissing) ---\n");
    const sl::Feature invalidFeature = (sl::Feature)9999;

    // 1. slIsFeatureSupported with unknown feature
    assert(pfnIsFeatureSupported(invalidFeature, adapter) == sl::Result::eErrorFeatureMissing);
    printf("[PASS] slIsFeatureSupported returns eErrorFeatureMissing for unknown feature\n");

    // 2. slIsFeatureLoaded with unknown feature
    assert(pfnIsFeatureLoaded(invalidFeature, bLoaded) == sl::Result::eErrorFeatureMissing);
    printf("[PASS] slIsFeatureLoaded returns eErrorFeatureMissing for unknown feature\n");

    // 3. slSetFeatureLoaded with unknown feature
    assert(pfnSetFeatureLoaded(invalidFeature, true) == sl::Result::eErrorFeatureMissing);
    printf("[PASS] slSetFeatureLoaded returns eErrorFeatureMissing for unknown feature\n");

    // 4. slGetFeatureRequirements with unknown feature
    assert(pfnGetFeatureRequirements(invalidFeature, reqs) == sl::Result::eErrorFeatureMissing);
    printf("[PASS] slGetFeatureRequirements returns eErrorFeatureMissing for unknown feature\n");

    // 5. slGetFeatureVersion with unknown feature
    assert(pfnGetFeatureVersion(invalidFeature, ver) == sl::Result::eErrorFeatureMissing);
    printf("[PASS] slGetFeatureVersion returns eErrorFeatureMissing for unknown feature\n");

    // 6. slGetFeatureFunction with unknown feature
    fnPtr = (void*)0xDEADBEEF;
    assert(pfnGetFeatureFunction(invalidFeature, "slDLSSGetOptimalSettings", fnPtr) == sl::Result::eErrorFeatureMissing);
    assert(fnPtr == nullptr);
    printf("[PASS] slGetFeatureFunction returns eErrorFeatureMissing for unknown feature\n");

    // 7. slAllocateResources / slFreeResources with unknown feature
    assert(pfnAllocateResources(nullptr, invalidFeature, vp) == sl::Result::eErrorFeatureMissing);
    assert(pfnFreeResources(invalidFeature, vp) == sl::Result::eErrorFeatureMissing);
    printf("[PASS] slAllocateResources and slFreeResources return eErrorFeatureMissing for unknown feature\n");

    // =========================================================================
    // Test 4: Supported Feature Verification & Function Namespace Scoping
    // =========================================================================
    printf("\n--- Test 4: Supported Feature Verification & Namespace Scoping ---\n");

    // Valid features report eOk
    assert(pfnIsFeatureSupported(sl::kFeatureDLSS, adapter) == sl::Result::eOk);
    assert(pfnIsFeatureSupported(sl::kFeatureDLSS_G, adapter) == sl::Result::eOk);
    assert(pfnIsFeatureSupported(sl::kFeatureReflex, adapter) == sl::Result::eOk);
    assert(pfnIsFeatureSupported(sl::kFeaturePCL, adapter) == sl::Result::eOk);
    printf("[PASS] Valid features (DLSS, DLSS-G, Reflex, PCL) return eOk\n");

    // slIsFeatureLoaded and slSetFeatureLoaded on valid feature
    bLoaded = false;
    assert(pfnIsFeatureLoaded(sl::kFeatureDLSS, bLoaded) == sl::Result::eOk);
    assert(bLoaded == true); // Default loaded
    assert(pfnSetFeatureLoaded(sl::kFeatureDLSS, false) == sl::Result::eOk);
    assert(pfnIsFeatureLoaded(sl::kFeatureDLSS, bLoaded) == sl::Result::eOk);
    assert(bLoaded == false);
    assert(pfnSetFeatureLoaded(sl::kFeatureDLSS, true) == sl::Result::eOk);
    assert(pfnIsFeatureLoaded(sl::kFeatureDLSS, bLoaded) == sl::Result::eOk);
    assert(bLoaded == true);
    printf("[PASS] slIsFeatureLoaded and slSetFeatureLoaded toggle feature load state correctly\n");

    // Function resolution:
    // 1. Valid function on valid feature
    fnPtr = nullptr;
    assert(pfnGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fnPtr) == sl::Result::eOk);
    assert(fnPtr != nullptr);
    printf("[PASS] slGetFeatureFunction resolves known function on valid feature\n");

    // 2. Invalid function on valid feature -> must return eErrorMissingOrInvalidAPI
    fnPtr = (void*)0xDEADBEEF;
    assert(pfnGetFeatureFunction(sl::kFeatureDLSS, "NonExistentFunction", fnPtr) == sl::Result::eErrorMissingOrInvalidAPI);
    assert(fnPtr == nullptr);
    printf("[PASS] slGetFeatureFunction returns eErrorMissingOrInvalidAPI when function not in feature namespace\n");

    // 3. Null function pointer -> eErrorInvalidParameter
    fnPtr = (void*)0xDEADBEEF;
    assert(pfnGetFeatureFunction(sl::kFeatureDLSS, nullptr, fnPtr) == sl::Result::eErrorInvalidParameter);
    assert(fnPtr == nullptr);
    printf("[PASS] slGetFeatureFunction returns eErrorInvalidParameter on null functionName\n");

    // =========================================================================
    // Test 5: Frame Token Generation when Initialized
    // =========================================================================
    printf("\n--- Test 5: Frame Token Generation ---\n");
    pToken = nullptr;
    assert(pfnGetNewFrameToken(pToken, nullptr) == sl::Result::eOk);
    assert(pToken != nullptr);
    printf("[PASS] slGetNewFrameToken successfully returns valid token when initialized\n");

    // =========================================================================
    // Test 6: Shutdown Lifecycle & Re-check Uninitialized State
    // =========================================================================
    printf("\n--- Test 6: slShutdown and Post-Shutdown Idempotency Verification ---\n");
    assert(pfnShutdown() == sl::Result::eOk);
    printf("[PASS] slShutdown successfully closed Streamline context\n");

    // Immediately calling APIs again must now return eErrorNotInitialized!
    assert(pfnShutdown() == sl::Result::eErrorNotInitialized);
    assert(pfnGetNewFrameToken(pToken, nullptr) == sl::Result::eErrorNotInitialized);
    assert(pfnGetFeatureRequirements(sl::kFeatureDLSS, reqs) == sl::Result::eErrorNotInitialized);
    assert(pfnGetFeatureVersion(sl::kFeatureDLSS, ver) == sl::Result::eErrorNotInitialized);
    printf("[PASS] Post-shutdown API calls strictly return eErrorNotInitialized!\n");

    FreeLibrary(hSl);

    printf("\n=======================================================================\n");
    printf("[SUCCESS] All Round 6 Contract & Lifecycle Parity Tests PASSED 100%%!\n");
    printf("=======================================================================\n");
    return 0;
}
