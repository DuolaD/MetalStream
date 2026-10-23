#include "dxgi_proxy.h"
#include "framegen.h"
#include "../common/logger.h"
#include <mutex>

namespace {

HMODULE g_dxgiModule = nullptr;
std::once_flag g_dxgiOnce;
bool g_dxgiSuccess = false;

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID riid, void **ppFactory);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID riid, void **ppFactory);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void **ppFactory);
typedef HRESULT (WINAPI *PFN_DXGIGetDebugInterface1)(UINT Flags, REFIID riid, void **pDebug);
typedef HRESULT (WINAPI *PFN_DXGIDeclareAdapterRemovalSupport)();

PFN_CreateDXGIFactory g_pfnCreateDXGIFactory = nullptr;
PFN_CreateDXGIFactory1 g_pfnCreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 g_pfnCreateDXGIFactory2 = nullptr;
PFN_DXGIGetDebugInterface1 g_pfnDXGIGetDebugInterface1 = nullptr;
PFN_DXGIDeclareAdapterRemovalSupport g_pfnDXGIDeclareAdapterRemovalSupport = nullptr;

} // namespace

namespace metalstream {

bool InitDXGIProxy() {
    std::call_once(g_dxgiOnce, []() {
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring dxgiPath = std::wstring(sysDir) + L"\\dxgi.dll";

        g_dxgiModule = LoadLibraryW(dxgiPath.c_str());
        if (!g_dxgiModule) {
            LOG_ERROR("Failed to load system dxgi.dll from %ls (error %lu)", dxgiPath.c_str(), GetLastError());
            g_dxgiSuccess = false;
            return;
        }

        g_pfnCreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(g_dxgiModule, "CreateDXGIFactory");
        g_pfnCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(g_dxgiModule, "CreateDXGIFactory1");
        g_pfnCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_dxgiModule, "CreateDXGIFactory2");
        g_pfnDXGIGetDebugInterface1 = (PFN_DXGIGetDebugInterface1)GetProcAddress(g_dxgiModule, "DXGIGetDebugInterface1");
        g_pfnDXGIDeclareAdapterRemovalSupport = (PFN_DXGIDeclareAdapterRemovalSupport)GetProcAddress(g_dxgiModule, "DXGIDeclareAdapterRemovalSupport");

        LOG_INFO("DXGI proxy initialized successfully from %ls", dxgiPath.c_str());
        g_dxgiSuccess = true;
    });
    return g_dxgiSuccess;
}

void ShutdownDXGIProxy() {
    if (g_dxgiModule) {
        FreeLibrary(g_dxgiModule);
        g_dxgiModule = nullptr;
    }
}

} // namespace metalstream

extern "C" {

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory) {
    metalstream::InitDXGIProxy();
    LOG_DEBUG("CreateDXGIFactory called");
    if (!g_pfnCreateDXGIFactory) return E_FAIL;
    HRESULT hr = g_pfnCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) metalstream::framegen::InstallFactoryHooks(static_cast<IUnknown*>(*ppFactory));
    return hr;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    metalstream::InitDXGIProxy();
    LOG_DEBUG("CreateDXGIFactory1 called");
    if (!g_pfnCreateDXGIFactory1) return E_FAIL;
    HRESULT hr = g_pfnCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) metalstream::framegen::InstallFactoryHooks(static_cast<IUnknown*>(*ppFactory));
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
    metalstream::InitDXGIProxy();
    LOG_DEBUG("CreateDXGIFactory2 called (Flags=0x%X)", Flags);
    if (!g_pfnCreateDXGIFactory2) return E_FAIL;
    HRESULT hr = g_pfnCreateDXGIFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) metalstream::framegen::InstallFactoryHooks(static_cast<IUnknown*>(*ppFactory));
    return hr;
}

HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **pDebug) {
    metalstream::InitDXGIProxy();
    LOG_DEBUG("DXGIGetDebugInterface1 called");
    if (!g_pfnDXGIGetDebugInterface1) return E_FAIL;
    return g_pfnDXGIGetDebugInterface1(Flags, riid, pDebug);
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    metalstream::InitDXGIProxy();
    LOG_DEBUG("DXGIDeclareAdapterRemovalSupport called");
    if (g_pfnDXGIDeclareAdapterRemovalSupport) {
        return g_pfnDXGIDeclareAdapterRemovalSupport();
    }
    return S_OK;
}

}
