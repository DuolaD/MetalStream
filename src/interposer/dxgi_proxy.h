#pragma once

#include <windows.h>
#include <dxgi.h>
#include <dxgi1_6.h>

namespace metalstream {

bool InitDXGIProxy();
void ShutdownDXGIProxy();

} // namespace metalstream

extern "C" {
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory);
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory);
__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory);
__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **pDebug);
__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport();
}
