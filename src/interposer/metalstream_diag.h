#pragma once

#include <cstdint>

// Cumulative counters exported for tests and offline diagnostics.
// Plain C layout; callers set structSize so the struct can grow later.
struct MetalStreamDiagnostics {
    uint32_t structSize;
    uint32_t reserved;
    uint64_t evaluateCalls;          // NVNGXRouter::Evaluate entries
    uint64_t evaluateDispatched;     // NGX EvaluateFeature succeeded
    uint64_t evaluateFailures;       // NGX EvaluateFeature failed (BlitGuard used)
    uint64_t featureCreates;         // NGX CreateFeature succeeded
    uint64_t featureCreateFailures;  // NGX CreateFeature failed (BlitGuard used)
    uint64_t featureRecreates;       // existing handle dropped because parameters changed
    uint64_t blitFallbacks;          // BlitGuard copies issued
    uint64_t deferredReleasesPending;
    uint64_t lockWaitSamples;        // m_mutex acquisitions measured
    uint64_t lockWaitTotalUs;
};

// Returns 1 on success, 0 if out is null or structSize is too small.
extern "C" __declspec(dllexport) int MetalStream_GetDiagnostics(MetalStreamDiagnostics* out);
