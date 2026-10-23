// Real-GPU probe (needs Wine + GPTk, run via scripts/run_gptk_tests.sh):
// does GPTk D3DMetal's NGX implementation expose a working NVSDK_NGX_Feature_FrameGeneration
// (DLSS-G -> MetalFX frame interpolation)? D3DMetal only enables it when a file named
// nvngx_dlssg.dll exists under the executable's directory; the build drops an empty one there.
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <nvngx/nvsdk_ngx.h>
#include <nvngx/nvsdk_ngx_defs.h>
#include <nvngx/nvsdk_ngx_params.h>

typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_Init)(unsigned long long, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_GetCaps)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_Alloc)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_Create)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_Eval)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);

static const UINT W = 1280, H = 720;
static int g_failures = 0;

#define CHECK(cond, ...) do { \
        if (cond) { printf("[PASS] "); } else { printf("[FAIL] "); ++g_failures; } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

static ID3D12Resource* MakeTex(ID3D12Device* dev, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = flags;
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, cv, IID_PPV_ARGS(&r));
    if (FAILED(hr)) printf("CreateCommittedResource fmt=%d failed 0x%08lX\n", fmt, hr);
    return r;
}

static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &br);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) { printf("D3D12CreateDevice failed 0x%08lX\n", hr); return 1; }

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ID3D12CommandAllocator* ca; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    ID3D12GraphicsCommandList* cl; dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));
    ID3D12Fence* fence; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fv = 0;
    auto submitAndWait = [&]() {
        cl->Close();
        ID3D12CommandList* lists[] = { cl };
        q->ExecuteCommandLists(1, lists);
        q->Signal(fence, ++fv);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(fv, ev);
        DWORD w = WaitForSingleObject(ev, 10000);
        CloseHandle(ev);
        ca->Reset(); cl->Reset(ca, nullptr);
        return w == WAIT_OBJECT_0;
    };

    HMODULE m = LoadLibraryW(L"nvngx.dll");
    if (!m) { printf("LoadLibrary nvngx.dll failed %lu\n", GetLastError()); return 1; }
    auto pInit = (PFN_Init)GetProcAddress(m, "NVSDK_NGX_D3D12_Init");
    auto pCaps = (PFN_GetCaps)GetProcAddress(m, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    auto pAlloc = (PFN_Alloc)GetProcAddress(m, "NVSDK_NGX_D3D12_AllocateParameters");
    auto pCreate = (PFN_Create)GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature");
    auto pEval = (PFN_Eval)GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature");

    NVSDK_NGX_Result r = pInit(0x2401, L".", dev, nullptr, NVSDK_NGX_Version_API);
    CHECK(NVSDK_NGX_SUCCEED(r), "NGX Init -> 0x%08X", r);

    NVSDK_NGX_Parameter* caps = nullptr;
    r = pCaps(&caps);
    CHECK(NVSDK_NGX_SUCCEED(r) && caps, "GetCapabilityParameters -> 0x%08X", r);
    if (caps) {
        int iv = -1; unsigned uv = 0xFFFFFFFF;
        NVSDK_NGX_Result ri = caps->Get("FrameGeneration.Available", &iv);
        NVSDK_NGX_Result ru = caps->Get("FrameGeneration.Available", &uv);
        CHECK(NVSDK_NGX_SUCCEED(ri) && iv == 1, "FrameGeneration.Available int=%d (0x%08X) uint=%u (0x%08X)", iv, ri, uv, ru);
        iv = -1; ri = caps->Get("SuperSampling.Available", &iv);
        printf("    SuperSampling.Available   int=%d (0x%08X)\n", iv, ri);
    }

    // Resources
    D3D12_CLEAR_VALUE ccv = {}; ccv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ccv.Color[0] = 0.2f; ccv.Color[1] = 0.4f; ccv.Color[2] = 0.6f; ccv.Color[3] = 1.0f;
    ID3D12Resource* backbuf = MakeTex(dev, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &ccv);
    ID3D12Resource* hudless = MakeTex(dev, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &ccv);
    ID3D12Resource* depth   = MakeTex(dev, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
    ID3D12Resource* mvecs   = MakeTex(dev, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
    ID3D12Resource* outInterp = MakeTex(dev, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    ID3D12Resource* outReal   = MakeTex(dev, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);

    // Clear backbuffer/hudless to a known color
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
    ID3D12DescriptorHeap* rtvHeap; dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    D3D12_CPU_DESCRIPTOR_HANDLE h0 = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = h0; h1.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dev->CreateRenderTargetView(backbuf, nullptr, h0);
    dev->CreateRenderTargetView(hudless, nullptr, h1);
    cl->ClearRenderTargetView(h0, ccv.Color, 0, nullptr);
    cl->ClearRenderTargetView(h1, ccv.Color, 0, nullptr);
    Barrier(cl, backbuf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, hudless, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Create FrameGeneration feature
    NVSDK_NGX_Parameter* p = nullptr;
    r = pAlloc(&p);
    CHECK(NVSDK_NGX_SUCCEED(r) && p, "AllocateParameters -> 0x%08X", r);
    p->Set(NVSDK_NGX_Parameter_Width, W);
    p->Set(NVSDK_NGX_Parameter_Height, H);
    p->Set(NVSDK_NGX_Parameter_OutWidth, W);
    p->Set(NVSDK_NGX_Parameter_OutHeight, H);
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set("DLSSG.BackbufferFormat", (unsigned)DXGI_FORMAT_R8G8B8A8_UNORM);

    NVSDK_NGX_Handle* handle = nullptr;
    r = pCreate(cl, NVSDK_NGX_Feature_FrameGeneration, p, &handle);
    CHECK(NVSDK_NGX_SUCCEED(r) && handle, "CreateFeature(FrameGeneration=%d) -> 0x%08X handle=%p", (int)NVSDK_NGX_Feature_FrameGeneration, r, (void*)handle);
    CHECK(submitAndWait(), "CreateFeature command list completed");
    if (NVSDK_NGX_FAILED(r) || !handle) { printf("RESULT: FAIL (FrameGeneration feature not creatable)\n"); return 1; }

    // Evaluate
    p->Set("DLSSG.Backbuffer", backbuf);
    p->Set("DLSSG.HUDLess", hudless);
    p->Set("DLSSG.Depth", depth);
    p->Set("DLSSG.MVecs", mvecs);
    p->Set("DLSSG.OutputInterpolated", outInterp);
    p->Set("DLSSG.OutputReal", outReal);
    p->Set("DLSSG.MvecScaleX", 1.0f);
    p->Set("DLSSG.MvecScaleY", 1.0f);
    p->Set("DLSSG.CameraNear", 0.1f);
    p->Set("DLSSG.CameraFar", 1000.0f);
    p->Set("DLSSG.CameraFOV", 1.0472f);
    p->Set("DLSSG.CameraAspectRatio", (float)W / H);
    p->Set("DLSSG.DepthInverted", 1u);
    p->Set("DLSSG.Reset", 0u);
    p->Set("DLSSG.JitterOffsetX", 0.0f);
    p->Set("DLSSG.JitterOffsetY", 0.0f);
    const char* subrects[] = { "Backbuffer", "Depth", "MVecs", "HUDLess" };
    for (const char* s : subrects) {
        char n[128];
        snprintf(n, sizeof n, "DLSSG.%sSubrectBaseX", s); p->Set(n, 0u);
        snprintf(n, sizeof n, "DLSSG.%sSubrectBaseY", s); p->Set(n, 0u);
        snprintf(n, sizeof n, "DLSSG.%sSubrectWidth", s); p->Set(n, W);
        snprintf(n, sizeof n, "DLSSG.%sSubrectHeight", s); p->Set(n, H);
    }

    for (int frame = 0; frame < 3; ++frame) {
        p->Set("DLSSG.Reset", frame == 0 ? 1u : 0u);
        r = pEval(cl, handle, p, nullptr);
        CHECK(NVSDK_NGX_SUCCEED(r), "EvaluateFeature frame %d -> 0x%08X", frame, r);
        CHECK(submitAndWait(), "EvaluateFeature frame %d command list completed", frame);
    }

    // Readback center pixel of OutputInterpolated
    D3D12_HEAP_PROPERTIES rh = {}; rh.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)W * H * 4;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb; dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    Barrier(cl, outInterp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = outInterp; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dst.PlacedFootprint.Footprint.Width = W;
    dst.PlacedFootprint.Footprint.Height = H; dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = W * 4;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    submitAndWait();
    uint8_t* px = nullptr; rb->Map(0, nullptr, (void**)&px);
    size_t off = ((size_t)(H / 2) * W + W / 2) * 4;
    auto close = [](int a, int b) { return a - b <= 2 && b - a <= 2; };
    CHECK(close(px[off], 51) && close(px[off + 1], 102) && close(px[off + 2], 153) && px[off + 3] == 255,
          "OutputInterpolated center RGBA = %u %u %u %u (expect ~51 102 153 255)", px[off], px[off + 1], px[off + 2], px[off + 3]);
    rb->Unmap(0, nullptr);

    // Temporal semantics: feed a different gray level each frame and see which frames
    // OutputInterpolated / OutputReal correspond to (is the output delayed by a frame?)
    Barrier(cl, outInterp, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto readCenterRed = [&](ID3D12Resource* res) -> int {
        Barrier(cl, res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION s = src; s.pResource = res;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
        Barrier(cl, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submitAndWait();
        uint8_t* data = nullptr;
        rb->Map(0, nullptr, (void**)&data);
        int v = data[off];
        rb->Unmap(0, nullptr);
        return v;
    };
    const int grays[] = { 40, 80, 120, 160, 200 };
    int interpVals[5] = {}, realVals[5] = {};
    for (int f = 0; f < 5; ++f) {
        float g = grays[f] / 255.0f;
        float color[4] = { g, g, g, 1.0f };
        Barrier(cl, backbuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(cl, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ClearRenderTargetView(h0, color, 0, nullptr);
        cl->ClearRenderTargetView(h1, color, 0, nullptr);
        Barrier(cl, backbuf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, hudless, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        p->Set("DLSSG.Reset", f == 0 ? 1u : 0u);
        r = pEval(cl, handle, p, nullptr);
        submitAndWait();
        interpVals[f] = readCenterRed(outInterp);
        realVals[f] = readCenterRed(outReal);
        printf("[INFO] frame %d: input gray %3d -> OutputInterpolated %3d, OutputReal %3d\n", f, grays[f], interpVals[f], realVals[f]);
    }
    // Expected without delay: interpolated(f) = (g[f-1] + g[f]) / 2, real(f) = g[f]
    // With zero motion vectors MetalFX warps instead of blending, so a static scene yields the
    // current frame for both outputs; that still rules out a one-frame delay.
    bool noDelay = true, oneFrameDelay = true, tracksCurrent = true;
    for (int f = 2; f < 5; ++f) {
        if (!close(interpVals[f], grays[f]) || !close(realVals[f], grays[f])) tracksCurrent = false;
        if (!close(interpVals[f], (grays[f - 1] + grays[f]) / 2) || !close(realVals[f], grays[f])) noDelay = false;
        if (!close(interpVals[f], (grays[f - 2] + grays[f - 1]) / 2) || !close(realVals[f], grays[f - 1])) oneFrameDelay = false;
    }
    printf("[INFO] temporal semantics: %s\n", tracksCurrent ? "static scene: both outputs track the current frame (no delay)"
                                        : noDelay ? "interpolated = midpoint(prev, current), real = current (no delay)"
                                        : oneFrameDelay ? "ONE FRAME DELAY: interpolated = midpoint(f-2, f-1), real = f-1"
                                                        : "unrecognized pattern (see values above)");

    // Motion semantics: a white bar moves +80px per frame; uniform motion vectors are given in
    // different conventions. A correct interpolation puts the bar halfway between the previous
    // and the current position. Tells which DLSSG.MVecs / MvecScale convention D3DMetal expects.
    ID3D12Resource* mvRT = MakeTex(dev, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC mvHd = {}; mvHd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; mvHd.NumDescriptors = 1;
    ID3D12DescriptorHeap* mvHeap; dev->CreateDescriptorHeap(&mvHd, IID_PPV_ARGS(&mvHeap));
    D3D12_CPU_DESCRIPTOR_HANDLE hMv = mvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(mvRT, nullptr, hMv);
    p->Set("DLSSG.MVecs", mvRT);

    auto barCenter = [&](ID3D12Resource* res) -> float {
        Barrier(cl, res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION s = src; s.pResource = res;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
        Barrier(cl, res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submitAndWait();
        uint8_t* data = nullptr;
        rb->Map(0, nullptr, (void**)&data);
        const uint8_t* row = data + (size_t)(H / 2) * W * 4;
        double sum = 0, n = 0;
        for (UINT x = 0; x < W; ++x) {
            if (row[x * 4] > 128) { sum += x; n += 1; }
        }
        rb->Unmap(0, nullptr);
        return n > 0 ? (float)(sum / n) : -1.0f;
    };

    struct MotionCase { const char* name; float mv; float scale; bool mustInterpolate = false; };
    const float motionPx = 80.0f;
    const MotionCase cases[] = {
        { "mv=(prev-curr) in UV,     MvecScale=1 ", -motionPx / W, 1.0f },
        { "mv=(prev-curr) in UV,     MvecScale=-1", -motionPx / W, -1.0f },
        { "mv=(prev-curr) in pixels, MvecScale=1 ", -motionPx, 1.0f },
        { "mv=(prev-curr) in UV,     MvecScale=W ", -motionPx / W, (float)W, true },
        { "mv=0 (no motion)                      ", 0.0f, 1.0f },
    };
    const float bg[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const int kMotionFrames = 4;
    const float prevX = 200.0f + motionPx * (kMotionFrames - 2), currX = 200.0f + motionPx * (kMotionFrames - 1);
    printf("[INFO] motion test: bar previous x=%.0f, current x=%.0f -> correct interpolation x=%.0f (x=%.0f would be extrapolated)\n",
           prevX, currX, (prevX + currX) / 2, currX + motionPx / 2);
    for (const auto& mc : cases) {
        p->Set("DLSSG.MvecScaleX", mc.scale);
        p->Set("DLSSG.MvecScaleY", mc.scale);
        float interpX = -1, realX = -1;
        for (int f = 0; f < kMotionFrames; ++f) {
            LONG x = (LONG)(200.0f + motionPx * f);
            D3D12_RECT bar = { x - 20, 0, x + 20, (LONG)H };
            Barrier(cl, backbuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Barrier(cl, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Barrier(cl, mvRT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            cl->ClearRenderTargetView(h0, bg, 0, nullptr);
            cl->ClearRenderTargetView(h1, bg, 0, nullptr);
            cl->ClearRenderTargetView(h0, white, 1, &bar);
            cl->ClearRenderTargetView(h1, white, 1, &bar);
            const float mvColor[4] = { mc.mv, 0.0f, 0.0f, 0.0f };
            cl->ClearRenderTargetView(hMv, mvColor, 0, nullptr);
            Barrier(cl, backbuf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, hudless, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, mvRT, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            p->Set("DLSSG.Reset", f == 0 ? 1u : 0u);
            pEval(cl, handle, p, nullptr);
            submitAndWait();
        }
        interpX = barCenter(outInterp);
        realX = barCenter(outReal);
        printf("[INFO]   %s -> OutputInterpolated bar x=%.1f, OutputReal bar x=%.1f\n", mc.name, interpX, realX);
        if (mc.mustInterpolate) {
            CHECK(fabsf(interpX - (prevX + currX) / 2) < 3.0f && fabsf(realX - currX) < 3.0f,
                  "motion vectors in pixels of the mvec texture interpolate correctly (%s)", mc.name);
        }
    }

    // Low-resolution motion vectors (half res, like DLSS render resolution): are the pixels in
    // motion-vector texture space or in output space?
    {
        const UINT LW = W / 2, LH = H / 2;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = LW; d.Height = LH; d.DepthOrArraySize = 1;
        d.MipLevels = 1; d.Format = DXGI_FORMAT_R16G16_FLOAT; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* mvLow = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&mvLow));
        d.Format = DXGI_FORMAT_R32_FLOAT; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* depthLow = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&depthLow));
        dev->CreateRenderTargetView(mvLow, nullptr, hMv);
        p->Set("DLSSG.MVecs", mvLow);
        p->Set("DLSSG.Depth", depthLow);
        p->Set("DLSSG.MVecsSubrectWidth", LW); p->Set("DLSSG.MVecsSubrectHeight", LH);
        p->Set("DLSSG.DepthSubrectWidth", LW); p->Set("DLSSG.DepthSubrectHeight", LH);

        const MotionCase lowCases[] = {
            { "low-res mv in UV, MvecScale=mvec width  ", -motionPx / W, (float)LW, true },
            { "low-res mv in UV, MvecScale=output width", -motionPx / W, (float)W },
        };
        for (const auto& mc : lowCases) {
            p->Set("DLSSG.MvecScaleX", mc.scale);
            p->Set("DLSSG.MvecScaleY", mc.scale);
            for (int f = 0; f < kMotionFrames; ++f) {
                LONG x = (LONG)(200.0f + motionPx * f);
                D3D12_RECT bar = { x - 20, 0, x + 20, (LONG)H };
                Barrier(cl, backbuf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                Barrier(cl, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                Barrier(cl, mvLow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                cl->ClearRenderTargetView(h0, bg, 0, nullptr);
                cl->ClearRenderTargetView(h1, bg, 0, nullptr);
                cl->ClearRenderTargetView(h0, white, 1, &bar);
                cl->ClearRenderTargetView(h1, white, 1, &bar);
                const float mvColor[4] = { mc.mv, 0.0f, 0.0f, 0.0f };
                cl->ClearRenderTargetView(hMv, mvColor, 0, nullptr);
                Barrier(cl, backbuf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, hudless, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, mvLow, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                p->Set("DLSSG.Reset", f == 0 ? 1u : 0u);
                pEval(cl, handle, p, nullptr);
                submitAndWait();
            }
            float lowX = barCenter(outInterp);
            printf("[INFO]   %s -> OutputInterpolated bar x=%.1f\n", mc.name, lowX);
            if (mc.mustInterpolate) {
                CHECK(fabsf(lowX - (prevX + currX) / 2) < 3.0f, "low-res motion vectors interpolate correctly (%s)", mc.name);
            }
        }
    }

    printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
