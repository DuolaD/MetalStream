#pragma once

// Minimal fake D3D12 objects for MetalStream behavior tests (no GPU or D3DMetal required).
// Implemented in C (fake_d3d12.c) against the CINTERFACE vtables so every slot is filled.
//
// - Fake device: QueryInterface(ID3D12Device/IUnknown) returns itself; everything else is a no-op.
// - Fake resource: GetDesc returns the size/format it was created with.
// - Fake command list: records ResourceBarrier / CopyResource / CopyTextureRegion calls.

#include <windows.h>
#include <d3d12.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FakeCmdListStats {
    unsigned int barrierCalls;
    unsigned int barrierCount;
    unsigned int copyResourceCalls;
    unsigned int copyTextureRegionCalls;
    ID3D12Resource* lastCopyDst;
    ID3D12Resource* lastCopySrc;
} FakeCmdListStats;

ID3D12Device* Fake_CreateDevice(void);
ID3D12Resource* Fake_CreateTexture(UINT width, UINT height, DXGI_FORMAT format);
ID3D12GraphicsCommandList* Fake_CreateCommandList(void);
void Fake_GetCmdListStats(ID3D12GraphicsCommandList* cmdList, FakeCmdListStats* out);
void Fake_ResetCmdListStats(ID3D12GraphicsCommandList* cmdList);

#ifdef __cplusplus
}
#endif
