#pragma once

// DLSS-G frame generation (prototype).
//
// GPTk's D3DMetal implements NVSDK_NGX_Feature_FrameGeneration on top of MetalFX
// MTLFXFrameInterpolator (it is enabled when nvngx_dlssg.dll sits in the game directory).
// This module plays the role of NVIDIA's sl.dlss_g plugin: it hooks the DXGI factory /
// swapchain vtables, and on each Present it evaluates the NGX feature to produce an
// interpolated frame, presents it, then presents the real frame.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <streamline/sl.h>
#include <streamline/sl_consts.h>

namespace metalstream::framegen {

struct TagInput {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = (D3D12_RESOURCE_STATES)UINT_MAX;
    sl::Extent extent{};
};

struct Inputs {
    bool requested = false;          // game enabled DLSS-G (mode eOn/eAuto) and the feature is loaded
    uint32_t viewport = 0;
    TagInput hudless;
    TagInput depth;
    TagInput mvecs;
    bool hasConstants = false;
    sl::Constants constants{};
};

// Implemented in sl_interposer.cpp: snapshot of the Streamline state needed at Present time
bool CollectInputs(Inputs& out);

// Patch the vtable shared by every factory created from the same dxgi.dll
void InstallFactoryHooks(IUnknown* factory);
// Patch the swapchain vtable and remember which D3D12 queue presents it (queueOrDevice may be null).
// createdHere: seen at creation (before the game fetched its buffers), so it can use proxy backbuffers.
void RegisterSwapChain(IUnknown* swapChain, IUnknown* queueOrDevice, bool createdHere = false);
// Create a throwaway factory from the system dxgi.dll so its (shared) vtable gets hooked early
void InstallGlobalHooks();

// For tags with eOnlyValidNow: copy the resource on the game's command list into a persistent
// texture so it is still valid at Present. Returns the copy (and its state) or nullptr.
ID3D12Resource* PreserveVolatileTag(ID3D12GraphicsCommandList* cmdList, sl::BufferType type,
                                    ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                                    D3D12_RESOURCE_STATES* outState);

// False once a copy returned by PreserveVolatileTag has been evicted (a stored tag may still name it)
bool IsPreservedCopyAlive(ID3D12Resource* resource);

// True while frames are actually being generated
bool IsActive();
// DLSSGState::numFramesActuallyPresented: 2 while a generated frame is shown per game frame, else 1
uint32_t FramesPresentedPerGameFrame();

void Shutdown();

} // namespace metalstream::framegen
