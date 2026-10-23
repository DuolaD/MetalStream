#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "fake_d3d12.h"

// Every vtable slot defaults to this. On x64 the caller cleans up arguments, so ignoring them is safe;
// it returns 0 (S_OK / nullptr). Methods MetalStream actually relies on are overridden below.
static ULONG_PTR STDMETHODCALLTYPE Fake_Noop(void* This) {
    (void)This;
    return 0;
}

static void FillWithNoop(void* vtbl, size_t size) {
    void** slots = (void**)vtbl;
    for (size_t i = 0; i < size / sizeof(void*); ++i) {
        slots[i] = (void*)&Fake_Noop;
    }
}

static ULONG STDMETHODCALLTYPE Fake_AddRef(void* This) {
    (void)This;
    return 2;
}

static ULONG STDMETHODCALLTYPE Fake_Release(void* This) {
    (void)This;
    return 1;
}

// -----------------------------------------------------------------------------
// Device
// -----------------------------------------------------------------------------

static const GUID kIID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID kIID_ID3D12Device = { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };

typedef struct FakeDevice {
    const ID3D12DeviceVtbl* lpVtbl;
} FakeDevice;

static HRESULT STDMETHODCALLTYPE FakeDevice_QueryInterface(ID3D12Device* This, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, &kIID_ID3D12Device) || IsEqualGUID(riid, &kIID_IUnknown)) {
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE FakeDevice_CreateFence(ID3D12Device* This, UINT64 initial, D3D12_FENCE_FLAGS flags,
                                                        REFIID riid, void** fence) {
    (void)This; (void)initial; (void)flags; (void)riid;
    if (fence) *fence = NULL;
    return E_NOTIMPL;
}

ID3D12Device* Fake_CreateDevice(void) {
    static ID3D12DeviceVtbl s_vtbl;
    static FakeDevice s_device;
    static int s_init = 0;
    if (!s_init) {
        FillWithNoop(&s_vtbl, sizeof(s_vtbl));
        s_vtbl.QueryInterface = FakeDevice_QueryInterface;
        s_vtbl.AddRef = (ULONG (STDMETHODCALLTYPE*)(ID3D12Device*))Fake_AddRef;
        s_vtbl.Release = (ULONG (STDMETHODCALLTYPE*)(ID3D12Device*))Fake_Release;
        s_vtbl.CreateFence = FakeDevice_CreateFence;
        s_device.lpVtbl = &s_vtbl;
        s_init = 1;
    }
    return (ID3D12Device*)&s_device;
}

// -----------------------------------------------------------------------------
// Resource
// -----------------------------------------------------------------------------

typedef struct FakeResource {
    const ID3D12ResourceVtbl* lpVtbl;
    D3D12_RESOURCE_DESC desc;
} FakeResource;

static D3D12_RESOURCE_DESC* STDMETHODCALLTYPE FakeResource_GetDesc(ID3D12Resource* This, D3D12_RESOURCE_DESC* ret) {
    *ret = ((FakeResource*)This)->desc;
    return ret;
}

ID3D12Resource* Fake_CreateTexture(UINT width, UINT height, DXGI_FORMAT format) {
    static ID3D12ResourceVtbl s_vtbl;
    static int s_init = 0;
    if (!s_init) {
        FillWithNoop(&s_vtbl, sizeof(s_vtbl));
        s_vtbl.AddRef = (ULONG (STDMETHODCALLTYPE*)(ID3D12Resource*))Fake_AddRef;
        s_vtbl.Release = (ULONG (STDMETHODCALLTYPE*)(ID3D12Resource*))Fake_Release;
        s_vtbl.GetDesc = FakeResource_GetDesc;
        s_init = 1;
    }
    FakeResource* res = (FakeResource*)calloc(1, sizeof(FakeResource));
    res->lpVtbl = &s_vtbl;
    res->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res->desc.Width = width;
    res->desc.Height = height;
    res->desc.DepthOrArraySize = 1;
    res->desc.MipLevels = 1;
    res->desc.Format = format;
    res->desc.SampleDesc.Count = 1;
    return (ID3D12Resource*)res;
}

// -----------------------------------------------------------------------------
// Command list
// -----------------------------------------------------------------------------

typedef struct FakeCmdList {
    const ID3D12GraphicsCommandListVtbl* lpVtbl;
    FakeCmdListStats stats;
} FakeCmdList;

static void STDMETHODCALLTYPE FakeCmdList_ResourceBarrier(ID3D12GraphicsCommandList* This, UINT count,
                                                          const D3D12_RESOURCE_BARRIER* barriers) {
    (void)barriers;
    FakeCmdList* self = (FakeCmdList*)This;
    self->stats.barrierCalls++;
    self->stats.barrierCount += count;
}

static void STDMETHODCALLTYPE FakeCmdList_CopyResource(ID3D12GraphicsCommandList* This, ID3D12Resource* dst,
                                                       ID3D12Resource* src) {
    FakeCmdList* self = (FakeCmdList*)This;
    self->stats.copyResourceCalls++;
    self->stats.lastCopyDst = dst;
    self->stats.lastCopySrc = src;
}

static void STDMETHODCALLTYPE FakeCmdList_CopyTextureRegion(ID3D12GraphicsCommandList* This,
                                                            const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                                                            const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    (void)x; (void)y; (void)z; (void)box;
    FakeCmdList* self = (FakeCmdList*)This;
    self->stats.copyTextureRegionCalls++;
    self->stats.lastCopyDst = dst ? dst->pResource : NULL;
    self->stats.lastCopySrc = src ? src->pResource : NULL;
}

ID3D12GraphicsCommandList* Fake_CreateCommandList(void) {
    static ID3D12GraphicsCommandListVtbl s_vtbl;
    static int s_init = 0;
    if (!s_init) {
        FillWithNoop(&s_vtbl, sizeof(s_vtbl));
        s_vtbl.AddRef = (ULONG (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*))Fake_AddRef;
        s_vtbl.Release = (ULONG (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*))Fake_Release;
        s_vtbl.ResourceBarrier = FakeCmdList_ResourceBarrier;
        s_vtbl.CopyResource = FakeCmdList_CopyResource;
        s_vtbl.CopyTextureRegion = FakeCmdList_CopyTextureRegion;
        s_init = 1;
    }
    FakeCmdList* list = (FakeCmdList*)calloc(1, sizeof(FakeCmdList));
    list->lpVtbl = &s_vtbl;
    return (ID3D12GraphicsCommandList*)list;
}

void Fake_GetCmdListStats(ID3D12GraphicsCommandList* cmdList, FakeCmdListStats* out) {
    if (cmdList && out) *out = ((FakeCmdList*)cmdList)->stats;
}

void Fake_ResetCmdListStats(ID3D12GraphicsCommandList* cmdList) {
    if (cmdList) memset(&((FakeCmdList*)cmdList)->stats, 0, sizeof(FakeCmdListStats));
}
