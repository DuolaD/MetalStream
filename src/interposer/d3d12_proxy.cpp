#include "d3d12_proxy.h"
#include "../common/logger.h"
#include <streamline/sl.h>
#include <mutex>

namespace {

HMODULE g_d3d12Module = nullptr;
HMODULE g_d3d11Module = nullptr;

std::once_flag g_d3d12Once;
bool g_d3d12Success = false;

std::once_flag g_d3d11Once;
bool g_d3d11Success = false;

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12GetDebugInterface)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12SerializeRootSignature)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI *PFN_D3D12CreateRootSignatureDeserializer)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12SerializeVersionedRootSignature)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI *PFN_D3D12CreateVersionedRootSignatureDeserializer)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT (WINAPI *PFN_D3D12EnableExperimentalFeatures)(UINT, const IID*, void*, UINT*);
typedef HRESULT (WINAPI *PFN_D3D12GetInterface)(REFCLSID, REFIID, void**);

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *PFN_D3D11CreateDeviceAndSwapChain)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

PFN_D3D12CreateDevice g_pfnD3D12CreateDevice = nullptr;
PFN_D3D12GetDebugInterface g_pfnD3D12GetDebugInterface = nullptr;
PFN_D3D12SerializeRootSignature g_pfnD3D12SerializeRootSignature = nullptr;
PFN_D3D12CreateRootSignatureDeserializer g_pfnD3D12CreateRootSignatureDeserializer = nullptr;
PFN_D3D12SerializeVersionedRootSignature g_pfnD3D12SerializeVersionedRootSignature = nullptr;
PFN_D3D12CreateVersionedRootSignatureDeserializer g_pfnD3D12CreateVersionedRootSignatureDeserializer = nullptr;
PFN_D3D12EnableExperimentalFeatures g_pfnD3D12EnableExperimentalFeatures = nullptr;
PFN_D3D12GetInterface g_pfnD3D12GetInterface = nullptr;

PFN_D3D11CreateDevice g_pfnD3D11CreateDevice = nullptr;
PFN_D3D11CreateDeviceAndSwapChain g_pfnD3D11CreateDeviceAndSwapChain = nullptr;

} // namespace

namespace metalstream {

bool InitD3D12Proxy() {
    std::call_once(g_d3d12Once, []() {
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring d3d12Path = std::wstring(sysDir) + L"\\d3d12.dll";

        g_d3d12Module = LoadLibraryW(d3d12Path.c_str());
        if (!g_d3d12Module) {
            LOG_ERROR("Failed to load system d3d12.dll from %ls", d3d12Path.c_str());
            g_d3d12Success = false;
            return;
        }

        g_pfnD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(g_d3d12Module, "D3D12CreateDevice");
        g_pfnD3D12GetDebugInterface = (PFN_D3D12GetDebugInterface)GetProcAddress(g_d3d12Module, "D3D12GetDebugInterface");
        g_pfnD3D12SerializeRootSignature = (PFN_D3D12SerializeRootSignature)GetProcAddress(g_d3d12Module, "D3D12SerializeRootSignature");
        g_pfnD3D12CreateRootSignatureDeserializer = (PFN_D3D12CreateRootSignatureDeserializer)GetProcAddress(g_d3d12Module, "D3D12CreateRootSignatureDeserializer");
        g_pfnD3D12SerializeVersionedRootSignature = (PFN_D3D12SerializeVersionedRootSignature)GetProcAddress(g_d3d12Module, "D3D12SerializeVersionedRootSignature");
        g_pfnD3D12CreateVersionedRootSignatureDeserializer = (PFN_D3D12CreateVersionedRootSignatureDeserializer)GetProcAddress(g_d3d12Module, "D3D12CreateVersionedRootSignatureDeserializer");
        g_pfnD3D12EnableExperimentalFeatures = (PFN_D3D12EnableExperimentalFeatures)GetProcAddress(g_d3d12Module, "D3D12EnableExperimentalFeatures");
        g_pfnD3D12GetInterface = (PFN_D3D12GetInterface)GetProcAddress(g_d3d12Module, "D3D12GetInterface");

        LOG_INFO("D3D12 proxy initialized successfully from %ls", d3d12Path.c_str());
        g_d3d12Success = true;
    });
    return g_d3d12Success;
}

static bool InitD3D11Proxy() {
    std::call_once(g_d3d11Once, []() {
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring d3d11Path = std::wstring(sysDir) + L"\\d3d11.dll";

        g_d3d11Module = LoadLibraryW(d3d11Path.c_str());
        if (!g_d3d11Module) {
            LOG_WARN("Could not load d3d11.dll from %ls", d3d11Path.c_str());
            g_d3d11Success = false;
            return;
        }

        g_pfnD3D11CreateDevice = (PFN_D3D11CreateDevice)GetProcAddress(g_d3d11Module, "D3D11CreateDevice");
        g_pfnD3D11CreateDeviceAndSwapChain = (PFN_D3D11CreateDeviceAndSwapChain)GetProcAddress(g_d3d11Module, "D3D11CreateDeviceAndSwapChain");
        g_d3d11Success = true;
    });
    return g_d3d11Success;
}

void ShutdownD3D12Proxy() {
    if (g_d3d12Module) {
        FreeLibrary(g_d3d12Module);
        g_d3d12Module = nullptr;
    }
    if (g_d3d11Module) {
        FreeLibrary(g_d3d11Module);
        g_d3d11Module = nullptr;
    }
}

} // namespace metalstream

extern "C" {

HRESULT WINAPI D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    metalstream::InitD3D12Proxy();
    LOG_DEBUG("D3D12CreateDevice called (MinimumFeatureLevel=0x%X)", (UINT)MinimumFeatureLevel);
    if (!g_pfnD3D12CreateDevice) return E_FAIL;
    HRESULT hr = g_pfnD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        LOG_INFO("D3D12CreateDevice succeeded, auto-capturing ID3D12Device: %p", *ppDevice);
        metalstream::SetInternalD3D12Device(*ppDevice);
    }
    return hr;
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** ppvDebug) {
    metalstream::InitD3D12Proxy();
    if (!g_pfnD3D12GetDebugInterface) return E_FAIL;
    return g_pfnD3D12GetDebugInterface(riid, ppvDebug);
}

HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* pRootSignature, D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob) {
    metalstream::InitD3D12Proxy();
    if (!g_pfnD3D12SerializeRootSignature) return E_FAIL;
    return g_pfnD3D12SerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(LPCVOID pSrcData, SIZE_T SrcDataSizeInBytes, REFIID pRootSignatureDeserializerInterface, void** ppvRootSignatureDeserializer) {
    metalstream::InitD3D12Proxy();
    if (!g_pfnD3D12CreateRootSignatureDeserializer) return E_FAIL;
    return g_pfnD3D12CreateRootSignatureDeserializer(pSrcData, SrcDataSizeInBytes, pRootSignatureDeserializerInterface, ppvRootSignatureDeserializer);
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature, ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob) {
    metalstream::InitD3D12Proxy();
    if (!g_pfnD3D12SerializeVersionedRootSignature) return E_FAIL;
    return g_pfnD3D12SerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
}

HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(LPCVOID pSrcData, SIZE_T SrcDataSizeInBytes, REFIID pRootSignatureDeserializerInterface, void** ppvRootSignatureDeserializer) {
    metalstream::InitD3D12Proxy();
    if (!g_pfnD3D12CreateVersionedRootSignatureDeserializer) return E_FAIL;
    return g_pfnD3D12CreateVersionedRootSignatureDeserializer(pSrcData, SrcDataSizeInBytes, pRootSignatureDeserializerInterface, ppvRootSignatureDeserializer);
}

HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT NumFeatures, const IID* pIIDs, void* pConfigurationStructs, UINT* pConfigurationStructSizes) {
    metalstream::InitD3D12Proxy();
    if (!g_pfnD3D12EnableExperimentalFeatures) return E_FAIL;
    return g_pfnD3D12EnableExperimentalFeatures(NumFeatures, pIIDs, pConfigurationStructs, pConfigurationStructSizes);
}

HRESULT WINAPI D3D12GetInterface(REFCLSID rclsid, REFIID riid, void** ppv) {
    metalstream::InitD3D12Proxy();
    if (!g_pfnD3D12GetInterface) return E_FAIL;
    return g_pfnD3D12GetInterface(rclsid, riid, ppv);
}

HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    metalstream::InitD3D11Proxy();
    if (!g_pfnD3D11CreateDevice) return E_FAIL;
    return g_pfnD3D11CreateDevice(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    metalstream::InitD3D11Proxy();
    if (!g_pfnD3D11CreateDeviceAndSwapChain) return E_FAIL;
    return g_pfnD3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
}

}
