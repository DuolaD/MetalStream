#pragma once

// Test-only control/inspection API exported by the mock nvngx.dll (tests/mock/mock_nvngx.cpp).
// The mock records what MetalStream asks NGX to do so tests can assert on behavior, not return codes.

#include <cstdint>

struct MockNGXCounters {
    uint32_t initCalls;
    uint32_t shutdownCalls;
    uint32_t paramsAllocated;
    uint32_t paramsDestroyed;
    uint32_t createCalls;
    uint32_t createSucceeded;
    uint32_t releaseCalls;
    uint32_t liveHandles;
    uint32_t evaluateCalls;
    uint32_t evaluateSucceeded;

    // Captured from the most recent CreateFeature
    int32_t lastCreateFlags;
    uint32_t lastCreateWidth;
    uint32_t lastCreateHeight;
    uint32_t lastCreateOutWidth;
    uint32_t lastCreateOutHeight;
    uint32_t lastCreatePerfQuality;

    // Captured from the most recent EvaluateFeature
    int32_t lastEvalReset;
    uint32_t lastEvalWidth;
    uint32_t lastEvalHeight;
    float lastEvalMvScaleX;
    float lastEvalMvScaleY;
    void* lastEvalColor;
    void* lastEvalOutput;
    void* lastEvalCmdList;
    uint32_t resetEvaluations;  // evaluations where Reset != 0
};

typedef void (*PFN_MockNGX_Reset)();
typedef void (*PFN_MockNGX_GetCounters)(MockNGXCounters* out);
typedef void (*PFN_MockNGX_SetCreateResult)(uint32_t ngxResult);
typedef void (*PFN_MockNGX_SetEvaluateResult)(uint32_t ngxResult);
