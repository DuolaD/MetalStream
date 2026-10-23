#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cassert>
#include <cstdio>
#include <vector>
#include <streamline/sl.h>
#include <streamline/sl_dlss.h>
#include <streamline/sl_dlss_g.h>
#include <streamline/sl_reflex.h>
#include <streamline/sl_pcl.h>

typedef sl::Result (*PFN_slInit)(const sl::Preferences& pref, uint64_t sdkVersion);
typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slIsFeatureSupported)(sl::Feature feature, const sl::AdapterInfo& adapterInfo);
typedef sl::Result (*PFN_slIsFeatureLoaded)(sl::Feature feature, bool& loaded);
typedef sl::Result (*PFN_slSetFeatureLoaded)(sl::Feature feature, bool loaded);
typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slAllocateResources)(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport);
typedef sl::Result (*PFN_slFreeResources)(sl::Feature feature, const sl::ViewportHandle& viewport);
typedef sl::Result (*PFN_slSetTag)(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slSetTagForFrame)(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slSetConstants)(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport);
typedef sl::Result (*PFN_slSetD3DDevice)(void* d3dDevice);
typedef sl::Result (*PFN_slGetFeatureFunction)(sl::Feature feature, const char* functionName, void*& function);
typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*& token, const uint32_t* frameIndex);
typedef sl::Result (*PFN_slSetData)(const sl::BaseStructure* inputs, sl::CommandBuffer* cmdBuffer);
typedef sl::Result (*PFN_slGetData)(const sl::BaseStructure* inputs, sl::BaseStructure* output, sl::CommandBuffer* cmdBuffer);
typedef HRESULT (WINAPI *PFN_DXGIDeclareAdapterRemovalSupport)();

static ID3D12Resource* CreateTestTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format) {
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
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res));
    return res;
}

int main() {
    printf("=======================================================================\n");
    printf("[AUDIT7 VERIFICATION] Round 7 Deep Contract Parity Verification\n");
    printf("=======================================================================\n");

    HMODULE hSl = LoadLibraryA("sl.interposer.dll");
    assert(hSl && "Failed to load sl.interposer.dll");

    auto pfnInit = (PFN_slInit)GetProcAddress(hSl, "slInit");
    auto pfnShutdown = (PFN_slShutdown)GetProcAddress(hSl, "slShutdown");
    auto pfnIsFeatureSupported = (PFN_slIsFeatureSupported)GetProcAddress(hSl, "slIsFeatureSupported");
    auto pfnIsFeatureLoaded = (PFN_slIsFeatureLoaded)GetProcAddress(hSl, "slIsFeatureLoaded");
    auto pfnSetFeatureLoaded = (PFN_slSetFeatureLoaded)GetProcAddress(hSl, "slSetFeatureLoaded");
    auto pfnEvaluateFeature = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");
    auto pfnAllocateResources = (PFN_slAllocateResources)GetProcAddress(hSl, "slAllocateResources");
    auto pfnFreeResources = (PFN_slFreeResources)GetProcAddress(hSl, "slFreeResources");
    auto pfnSetTag = (PFN_slSetTag)GetProcAddress(hSl, "slSetTag");
    auto pfnSetTagForFrame = (PFN_slSetTagForFrame)GetProcAddress(hSl, "slSetTagForFrame");
    auto pfnSetConstants = (PFN_slSetConstants)GetProcAddress(hSl, "slSetConstants");
    auto pfnSetD3DDevice = (PFN_slSetD3DDevice)GetProcAddress(hSl, "slSetD3DDevice");
    auto pfnGetFeatureFunction = (PFN_slGetFeatureFunction)GetProcAddress(hSl, "slGetFeatureFunction");
    auto pfnGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(hSl, "slGetNewFrameToken");
    auto pfnSetData = (PFN_slSetData)GetProcAddress(hSl, "slSetData");
    auto pfnGetData = (PFN_slGetData)GetProcAddress(hSl, "slGetData");
    auto pfnDXGIDeclareAdapterRemovalSupport = (PFN_DXGIDeclareAdapterRemovalSupport)GetProcAddress(hSl, "DXGIDeclareAdapterRemovalSupport");

    assert(pfnInit && pfnShutdown && pfnIsFeatureSupported && pfnEvaluateFeature &&
           pfnAllocateResources && pfnFreeResources && pfnSetTag && pfnSetTagForFrame &&
           pfnSetConstants && pfnSetD3DDevice && pfnGetFeatureFunction && pfnGetNewFrameToken &&
           pfnSetData && pfnGetData);

    // -------------------------------------------------------------------------
    // Phase 1: DXGIDeclareAdapterRemovalSupport Export Parity
    // -------------------------------------------------------------------------
    printf("\n--- Phase 1: DXGIDeclareAdapterRemovalSupport Export Parity ---\n");
    assert(pfnDXGIDeclareAdapterRemovalSupport != nullptr);
    HRESULT hrRemoval = pfnDXGIDeclareAdapterRemovalSupport();
    assert(SUCCEEDED(hrRemoval));
    printf("[PASS] DXGIDeclareAdapterRemovalSupport successfully resolved and invoked: hr=0x%08X\n", (UINT)hrRemoval);

    // -------------------------------------------------------------------------
    // Phase 2: kSDKVersionMagic Enforcement on slInit
    // -------------------------------------------------------------------------
    printf("\n--- Phase 2: kSDKVersionMagic Enforcement on slInit ---\n");
    sl::Preferences prefInvalidVersion{};
    prefInvalidVersion.applicationId = 0x11223344;
    sl::Result rInvalidMagic = pfnInit(prefInvalidVersion, 0x12345678);
    assert(rInvalidMagic == sl::Result::eErrorInvalidParameter);
    printf("[PASS] slInit with invalid magic returns eErrorInvalidParameter: %u\n", (uint32_t)rInvalidMagic);

    rInvalidMagic = pfnInit(prefInvalidVersion, 0x00020400); // Missing 0xFEDC
    assert(rInvalidMagic == sl::Result::eErrorInvalidParameter);
    printf("[PASS] slInit with version missing magic (0x00020400) returns eErrorInvalidParameter: %u\n", (uint32_t)rInvalidMagic);

    // -------------------------------------------------------------------------
    // Phase 3: renderAPI Validation in slIsFeatureSupported
    // -------------------------------------------------------------------------
    printf("\n--- Phase 3: renderAPI Validation in slIsFeatureSupported ---\n");
    sl::Preferences prefBadAPI{};
    prefBadAPI.applicationId = 0x1234;
    prefBadAPI.renderAPI = (sl::RenderAPI)999; // Invalid API enum
    sl::Result rInitBadAPI = pfnInit(prefBadAPI, sl::kSDKVersion);
    assert(rInitBadAPI == sl::Result::eOk);

    sl::AdapterInfo adapterInfo{};
    sl::Result rSupportedBadAPI = pfnIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
    assert(rSupportedBadAPI == sl::Result::eErrorInvalidParameter);
    printf("[PASS] slIsFeatureSupported with invalid renderAPI returned eErrorInvalidParameter: %u\n", (uint32_t)rSupportedBadAPI);

    pfnShutdown();

    // -------------------------------------------------------------------------
    // Phase 4: slSetTag vs slSetTagForFrame & PreferenceFlags::eUseFrameBasedResourceTagging
    // -------------------------------------------------------------------------
    printf("\n--- Phase 4: Frame-Based Resource Tagging Mutual Exclusion ---\n");
    // Subcase 4A: PreferenceFlags::eUseFrameBasedResourceTagging is NOT set
    sl::Preferences prefLegacyTags{};
    prefLegacyTags.applicationId = 0x4321;
    prefLegacyTags.flags = (sl::PreferenceFlags)0;
    prefLegacyTags.renderAPI = sl::RenderAPI::eD3D12;
    pfnInit(prefLegacyTags, sl::kSDKVersion);

    ID3D12Device* device = nullptr;
    HRESULT hrDev = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    assert(SUCCEEDED(hrDev) && device);
    pfnSetD3DDevice(device);

    sl::FrameToken* pToken = nullptr;
    pfnGetNewFrameToken(pToken, nullptr);
    assert(pToken);

    sl::ViewportHandle vp0(0);
    ID3D12Resource* tex = CreateTestTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
    sl::Resource slRes{ sl::ResourceType::eTex2d, tex, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_COMMON };
    sl::ResourceTag tag(&slRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent);

    // When flag is NOT set:
    // slSetTag MUST succeed (eOk)
    sl::Result rLegacyTag = pfnSetTag(vp0, &tag, 1, nullptr);
    assert(rLegacyTag == sl::Result::eOk);
    printf("[PASS] slSetTag succeeded when eUseFrameBasedResourceTagging is NOT set\n");

    // slSetTagForFrame MUST return eErrorInvalidIntegration
    sl::Result rFrameTagWithoutFlag = pfnSetTagForFrame(*pToken, vp0, &tag, 1, nullptr);
    assert(rFrameTagWithoutFlag == sl::Result::eErrorInvalidIntegration);
    printf("[PASS] slSetTagForFrame returned eErrorInvalidIntegration when flag is NOT set: %u\n", (uint32_t)rFrameTagWithoutFlag);

    pfnShutdown();

    // Subcase 4B: PreferenceFlags::eUseFrameBasedResourceTagging IS set
    sl::Preferences prefFrameTags{};
    prefFrameTags.applicationId = 0x4321;
    prefFrameTags.flags = sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    prefFrameTags.renderAPI = sl::RenderAPI::eD3D12;
    pfnInit(prefFrameTags, sl::kSDKVersion);
    pfnSetD3DDevice(device);
    pfnGetNewFrameToken(pToken, nullptr);

    // When flag IS set:
    // slSetTag MUST return eErrorInvalidIntegration
    sl::Result rLegacyTagWithFlag = pfnSetTag(vp0, &tag, 1, nullptr);
    assert(rLegacyTagWithFlag == sl::Result::eErrorInvalidIntegration);
    printf("[PASS] slSetTag returned eErrorInvalidIntegration when flag IS set: %u\n", (uint32_t)rLegacyTagWithFlag);

    // slSetTagForFrame MUST succeed (eOk)
    sl::Result rFrameTagWithFlag = pfnSetTagForFrame(*pToken, vp0, &tag, 1, nullptr);
    assert(rFrameTagWithFlag == sl::Result::eOk);
    printf("[PASS] slSetTagForFrame succeeded when eUseFrameBasedResourceTagging IS set\n");

    // -------------------------------------------------------------------------
    // Phase 5: Reflex & PCL Resource Allocation Rejection
    // -------------------------------------------------------------------------
    printf("\n--- Phase 5: Reflex & PCL Resource Allocation Contract ---\n");
    sl::Result rAllocReflex = pfnAllocateResources(nullptr, sl::kFeatureReflex, vp0);
    assert(rAllocReflex == sl::Result::eErrorMissingOrInvalidAPI);
    printf("[PASS] slAllocateResources(kFeatureReflex) returns eErrorMissingOrInvalidAPI: %u\n", (uint32_t)rAllocReflex);

    sl::Result rFreeReflex = pfnFreeResources(sl::kFeatureReflex, vp0);
    assert(rFreeReflex == sl::Result::eErrorMissingOrInvalidAPI);
    printf("[PASS] slFreeResources(kFeatureReflex) returns eErrorMissingOrInvalidAPI: %u\n", (uint32_t)rFreeReflex);

    sl::Result rAllocPCL = pfnAllocateResources(nullptr, sl::kFeaturePCL, vp0);
    assert(rAllocPCL == sl::Result::eErrorMissingOrInvalidAPI);
    printf("[PASS] slAllocateResources(kFeaturePCL) returns eErrorMissingOrInvalidAPI: %u\n", (uint32_t)rAllocPCL);

    sl::Result rFreePCL = pfnFreeResources(sl::kFeaturePCL, vp0);
    assert(rFreePCL == sl::Result::eErrorMissingOrInvalidAPI);
    printf("[PASS] slFreeResources(kFeaturePCL) returns eErrorMissingOrInvalidAPI: %u\n", (uint32_t)rFreePCL);

    // -------------------------------------------------------------------------
    // Phase 6: slEvaluateFeature(DLSS) Contract (Missing Constants & eOff)
    // -------------------------------------------------------------------------
    printf("\n--- Phase 6: slEvaluateFeature(DLSS) Missing Constants & eOff Contract ---\n");
    ID3D12CommandAllocator* cmdAlloc = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    ID3D12GraphicsCommandList* cmdList = nullptr;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList));

    // Viewport 1 has NO DLSSOptions set yet
    sl::ViewportHandle vp1(1);
    ID3D12Resource* texOut = CreateTestTexture(device, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
    sl::Resource slResOut{ sl::ResourceType::eTex2d, texOut, nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_COMMON };
    sl::ResourceTag tagIn(&slRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent);
    sl::ResourceTag tagOut(&slResOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent);
    sl::ResourceTag vp1Tags[2] = { tagIn, tagOut };
    pfnSetTagForFrame(*pToken, vp1, vp1Tags, 2, nullptr);

    // 6A: Missing Constants evaluation
    const sl::BaseStructure* evalInputsVp1[1] = { &vp1 };
    sl::Result rEvalMissing = pfnEvaluateFeature(sl::kFeatureDLSS, *pToken, evalInputsVp1, 1, (sl::CommandBuffer*)cmdList);
    assert(rEvalMissing == sl::Result::eErrorMissingConstants);
    printf("[PASS] slEvaluateFeature(DLSS) without DLSSOptions returns eErrorMissingConstants: %u\n", (uint32_t)rEvalMissing);

    // 6B: DLSSMode::eOff evaluation
    sl::DLSSOptions optsOff{};
    optsOff.mode = sl::DLSSMode::eOff;
    optsOff.outputWidth = 1920;
    optsOff.outputHeight = 1080;
    optsOff.next = &vp1;
    pfnSetData(&optsOff, nullptr);

    sl::Result rEvalOff = pfnEvaluateFeature(sl::kFeatureDLSS, *pToken, evalInputsVp1, 1, (sl::CommandBuffer*)cmdList);
    assert(rEvalOff == sl::Result::eErrorInvalidIntegration);
    printf("[PASS] slEvaluateFeature(DLSS) with DLSSMode::eOff returns eErrorInvalidIntegration: %u\n", (uint32_t)rEvalOff);

    // -------------------------------------------------------------------------
    // Phase 7: Parameter Bus slGetData Rejection
    // -------------------------------------------------------------------------
    printf("\n--- Phase 7: Parameter Bus slGetData Unrecognized Struct Rejection ---\n");
    struct FakeStruct : public sl::BaseStructure {
        FakeStruct() : BaseStructure(sl::StructType({ 0x1234, 0x5678, 0x9ABC, { 1, 2, 3, 4, 5, 6, 7, 8 } }), 1) {}
    } dummyOutput;

    sl::Result rGetDataBad = pfnGetData(nullptr, &dummyOutput, nullptr);
    assert(rGetDataBad == sl::Result::eErrorMissingInputParameter);
    printf("[PASS] slGetData with completely unrecognized struct returns eErrorMissingInputParameter: %u\n", (uint32_t)rGetDataBad);

    // Clean up
    cmdList->Release();
    cmdAlloc->Release();
    texOut->Release();
    tex->Release();
    device->Release();
    pfnShutdown();

    printf("\n=======================================================================\n");
    printf("[SUCCESS] All Round 7 Contract Parity Checks PASSED 100%%!\n");
    printf("=======================================================================\n");
    return 0;
}
