// Mock nvngx.dll for MetalStream behavior tests.
// Implements the D3D12 entry points NVNGXRouter loads and records every call.
// Never deployed to a game; built into build/mock/ next to the mock test only.

#define NVSDK_NGX 1
#include <windows.h>
#include <d3d12.h>
#include <nvngx/nvsdk_ngx.h>
#include <nvngx/nvsdk_ngx_defs.h>
#include <nvngx/nvsdk_ngx_params.h>

#include <map>
#include <mutex>
#include <set>
#include <string>

#include "mock_nvngx_api.h"

namespace {

class MockParameter : public NVSDK_NGX_Parameter {
public:
    void Set(const char* n, void* v) override { m_ptrs[n] = v; }
    void Set(const char* n, ID3D12Resource* v) override { m_ptrs[n] = v; }
    void Set(const char* n, ID3D11Resource* v) override { m_ptrs[n] = v; }
    void Set(const char* n, int v) override { SetNumber(n, (double)v); }
    void Set(const char* n, unsigned int v) override { SetNumber(n, (double)v); }
    void Set(const char* n, double v) override { SetNumber(n, v); }
    void Set(const char* n, float v) override { SetNumber(n, (double)v); }
    void Set(const char* n, unsigned long long v) override { SetNumber(n, (double)v); }

    NVSDK_NGX_Result Get(const char* n, void** out) const override { return GetPtr(n, out); }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** out) const override { return GetPtr(n, out); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** out) const override { return GetPtr(n, out); }
    NVSDK_NGX_Result Get(const char* n, int* out) const override { return GetNumber(n, out); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* out) const override { return GetNumber(n, out); }
    NVSDK_NGX_Result Get(const char* n, double* out) const override { return GetNumber(n, out); }
    NVSDK_NGX_Result Get(const char* n, float* out) const override { return GetNumber(n, out); }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* out) const override { return GetNumber(n, out); }

    void Reset() override {
        m_numbers.clear();
        m_ptrs.clear();
    }

    double Number(const char* n, double fallback = 0.0) const {
        auto it = m_numbers.find(n);
        return it != m_numbers.end() ? it->second : fallback;
    }
    void* Ptr(const char* n) const {
        auto it = m_ptrs.find(n);
        return it != m_ptrs.end() ? it->second : nullptr;
    }

private:
    void SetNumber(const char* n, double v) { m_numbers[n] = v; }

    template <typename T>
    NVSDK_NGX_Result GetPtr(const char* n, T** out) const {
        auto it = m_ptrs.find(n);
        if (!out || it == m_ptrs.end()) return NVSDK_NGX_Result_Fail;
        *out = static_cast<T*>(it->second);
        return NVSDK_NGX_Result_Success;
    }

    template <typename T>
    NVSDK_NGX_Result GetNumber(const char* n, T* out) const {
        auto it = m_numbers.find(n);
        if (!out || it == m_numbers.end()) return NVSDK_NGX_Result_Fail;
        *out = (T)it->second;
        return NVSDK_NGX_Result_Success;
    }

    std::map<std::string, double> m_numbers;
    std::map<std::string, void*> m_ptrs;
};

std::mutex g_mutex;
MockNGXCounters g_counters{};
std::set<NVSDK_NGX_Handle*> g_handles;
unsigned int g_nextHandleId = 1;
NVSDK_NGX_Result g_createResult = NVSDK_NGX_Result_Success;
NVSDK_NGX_Result g_evaluateResult = NVSDK_NGX_Result_Success;

} // namespace

extern "C" {

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init(unsigned long long, const wchar_t*, ID3D12Device*,
                                                 const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counters.initCalls++;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counters.shutdownCalls++;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_AllocateParameters(NVSDK_NGX_Parameter** outParams) {
    if (!outParams) return NVSDK_NGX_Result_Fail;
    std::lock_guard<std::mutex> lock(g_mutex);
    *outParams = new MockParameter();
    g_counters.paramsAllocated++;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter* params) {
    std::lock_guard<std::mutex> lock(g_mutex);
    delete static_cast<MockParameter*>(params);
    g_counters.paramsDestroyed++;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                          NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** outHandle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counters.createCalls++;
    if (!params || !outHandle) return NVSDK_NGX_Result_Fail;
    if (NVSDK_NGX_FAILED(g_createResult)) return g_createResult;

    auto* p = static_cast<MockParameter*>(params);
    g_counters.lastCreateFlags = (int32_t)p->Number(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
    g_counters.lastCreateWidth = (uint32_t)p->Number(NVSDK_NGX_Parameter_Width);
    g_counters.lastCreateHeight = (uint32_t)p->Number(NVSDK_NGX_Parameter_Height);
    g_counters.lastCreateOutWidth = (uint32_t)p->Number(NVSDK_NGX_Parameter_OutWidth);
    g_counters.lastCreateOutHeight = (uint32_t)p->Number(NVSDK_NGX_Parameter_OutHeight);
    g_counters.lastCreatePerfQuality = (uint32_t)p->Number(NVSDK_NGX_Parameter_PerfQualityValue);

    auto* handle = new NVSDK_NGX_Handle{ g_nextHandleId++ };
    g_handles.insert(handle);
    *outHandle = handle;
    g_counters.createSucceeded++;
    g_counters.liveHandles = (uint32_t)g_handles.size();
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counters.releaseCalls++;
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    delete handle;
    g_handles.erase(it);
    g_counters.liveHandles = (uint32_t)g_handles.size();
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* cmdList, const NVSDK_NGX_Handle* handle,
                                                            NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counters.evaluateCalls++;
    if (!params || !handle || g_handles.find(const_cast<NVSDK_NGX_Handle*>(handle)) == g_handles.end()) {
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    if (NVSDK_NGX_FAILED(g_evaluateResult)) return g_evaluateResult;

    auto* p = static_cast<MockParameter*>(params);
    g_counters.lastEvalReset = (int32_t)p->Number(NVSDK_NGX_Parameter_Reset);
    if (g_counters.lastEvalReset) g_counters.resetEvaluations++;
    g_counters.lastEvalWidth = (uint32_t)p->Number(NVSDK_NGX_Parameter_Width);
    g_counters.lastEvalHeight = (uint32_t)p->Number(NVSDK_NGX_Parameter_Height);
    g_counters.lastEvalMvScaleX = (float)p->Number(NVSDK_NGX_Parameter_MV_Scale_X);
    g_counters.lastEvalMvScaleY = (float)p->Number(NVSDK_NGX_Parameter_MV_Scale_Y);
    g_counters.lastEvalColor = p->Ptr(NVSDK_NGX_Parameter_Color);
    g_counters.lastEvalOutput = p->Ptr(NVSDK_NGX_Parameter_Output);
    g_counters.lastEvalCmdList = cmdList;
    g_counters.evaluateSucceeded++;
    return NVSDK_NGX_Result_Success;
}

__declspec(dllexport) void MockNGX_Reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t live = (uint32_t)g_handles.size();
    g_counters = {};
    g_counters.liveHandles = live;
    g_createResult = NVSDK_NGX_Result_Success;
    g_evaluateResult = NVSDK_NGX_Result_Success;
}

__declspec(dllexport) void MockNGX_GetCounters(MockNGXCounters* out) {
    if (!out) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    *out = g_counters;
}

__declspec(dllexport) void MockNGX_SetCreateResult(uint32_t result) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_createResult = (NVSDK_NGX_Result)result;
}

__declspec(dllexport) void MockNGX_SetEvaluateResult(uint32_t result) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_evaluateResult = (NVSDK_NGX_Result)result;
}

} // extern "C"
