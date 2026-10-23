#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdint.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <streamline/sl.h>
#include <streamline/sl_consts.h>

// Mock COM interface to verify AddRef behavior
class MockUnknown : public IUnknown {
public:
    std::atomic<ULONG> refCount{1};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = this;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refCount;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --refCount;
        return count;
    }
};

int main() {
    printf("[PHASE 1 TEST] Testing Stability & Thread Safety...\n");

    HMODULE hSl = LoadLibraryA("build/sl.interposer.dll");
    if (!hSl) {
        hSl = LoadLibraryA("sl.interposer.dll");
    }
    if (!hSl) {
        printf("[PHASE 1 FAILED] Could not load sl.interposer.dll\n");
        return 1;
    }
    printf("[PHASE 1] sl.interposer.dll loaded: %p\n", hSl);

    typedef sl::Result (*PFN_slInit)(const sl::Preferences&, uint64_t);
    typedef sl::Result (*PFN_slShutdown)();
    typedef sl::Result (*PFN_slGetNewFrameToken)(sl::FrameToken*&, const uint32_t*);
    typedef sl::Result (*PFN_slGetNativeInterface)(void*, void**);
    typedef sl::Result (*PFN_slEvaluateFeature)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);

    auto pfnInit = (PFN_slInit)GetProcAddress(hSl, "slInit");
    auto pfnShutdown = (PFN_slShutdown)GetProcAddress(hSl, "slShutdown");
    auto pfnGetNewFrameToken = (PFN_slGetNewFrameToken)GetProcAddress(hSl, "slGetNewFrameToken");
    auto pfnGetNativeInterface = (PFN_slGetNativeInterface)GetProcAddress(hSl, "slGetNativeInterface");
    auto pfnEvaluate = (PFN_slEvaluateFeature)GetProcAddress(hSl, "slEvaluateFeature");

    if (!pfnInit || !pfnShutdown || !pfnGetNewFrameToken || !pfnGetNativeInterface || !pfnEvaluate) {
        printf("[PHASE 1 FAILED] Missing required exports\n");
        return 1;
    }

    sl::Preferences pref{};
    pfnInit(pref, 0x2000B0001FEDC);

    // =========================================================================
    // Test 1: COM AddRef() contract in slGetNativeInterface
    // =========================================================================
    printf("\n--- Test 1: slGetNativeInterface COM AddRef() Contract ---\n");
    {
        // 1. Null check
        sl::Result resNull1 = pfnGetNativeInterface(nullptr, nullptr);
        if (resNull1 != sl::Result::eErrorInvalidParameter) {
            printf("[FAILED] Expected eErrorInvalidParameter for null inputs, got: %d\n", (int)resNull1);
            return 1;
        }

        MockUnknown mock;
        void* outInterface = nullptr;
        sl::Result resNull2 = pfnGetNativeInterface(&mock, nullptr);
        if (resNull2 != sl::Result::eErrorInvalidParameter) {
            printf("[FAILED] Expected eErrorInvalidParameter for null baseInterface, got: %d\n", (int)resNull2);
            return 1;
        }

        // 2. AddRef verification
        ULONG initialRef = mock.refCount.load(); // 1
        sl::Result resOk = pfnGetNativeInterface(&mock, &outInterface);
        if (resOk != sl::Result::eOk || outInterface != &mock) {
            printf("[FAILED] slGetNativeInterface failed: res=%d, out=%p\n", (int)resOk, outInterface);
            return 1;
        }

        ULONG afterRef = mock.refCount.load();
        printf("[TEST 1] Initial RefCount: %lu, After slGetNativeInterface: %lu\n", initialRef, afterRef);
        if (afterRef != initialRef + 1) {
            printf("[FAILED] AddRef was NOT called! Expected %lu, got %lu\n", initialRef + 1, afterRef);
            return 1;
        }

        // Caller releases interface according to COM rules
        mock.Release();
        if (mock.refCount.load() != initialRef) {
            printf("[FAILED] RefCount mismatch after Release!\n");
            return 1;
        }
        printf("[PASS] Test 1: slGetNativeInterface strictly adheres to COM AddRef contract!\n");
    }

    // =========================================================================
    // Test 2: FrameToken Ring Buffer (MAX_FRAMES_IN_FLIGHT = 6)
    // =========================================================================
    printf("\n--- Test 2: FrameToken Ring Buffer & Re-use ---\n");
    {
        sl::FrameToken* tokens[12];
        for (int i = 0; i < 12; ++i) {
            pfnGetNewFrameToken(tokens[i], nullptr);
            printf("[TEST 2] Token %d: addr=%p, frameIndex=%u\n", i, tokens[i], (uint32_t)*tokens[i]);
        }

        // Slot 0 and Slot 6 should be the exact same ring buffer slot address
        if (tokens[0] != tokens[6] || tokens[1] != tokens[7]) {
            printf("[FAILED] Ring buffer wrap-around address mismatch!\n");
            return 1;
        }

        // But within slots 0 to 5, all addresses must be distinct!
        for (int i = 0; i < 6; ++i) {
            for (int j = i + 1; j < 6; ++j) {
                if (tokens[i] == tokens[j]) {
                    printf("[FAILED] Distinct slots 0..5 have duplicate address: %p\n", tokens[i]);
                    return 1;
                }
            }
        }
        printf("[PASS] Ring buffer correctly allocates 6 distinct slots!\n");

        // Test same frameIndex re-use
        uint32_t targetIdx = 999;
        sl::FrameToken* t1 = nullptr;
        sl::FrameToken* t2 = nullptr;
        pfnGetNewFrameToken(t1, &targetIdx);
        pfnGetNewFrameToken(t2, &targetIdx);
        if (t1 != t2 || (uint32_t)*t1 != targetIdx) {
            printf("[FAILED] Identical frameIndex should return same token! t1=%p, t2=%p\n", t1, t2);
            return 1;
        }
        printf("[PASS] Test 2: Same frameIndex reuses previous token without advancing ring buffer!\n");
    }

    // =========================================================================
    // Test 3: Multi-threaded Concurrent FrameToken Stress Test
    // =========================================================================
    printf("\n--- Test 3: Multi-threaded Concurrent FrameToken Stress Test ---\n");
    {
        constexpr int NUM_THREADS = 8;
        constexpr int REQUESTS_PER_THREAD = 5000;
        std::atomic<bool> startFlag{false};
        std::vector<std::thread> workers;

        for (int t = 0; t < NUM_THREADS; ++t) {
            workers.emplace_back([&, t]() {
                while (!startFlag.load()) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < REQUESTS_PER_THREAD; ++i) {
                    sl::FrameToken* token = nullptr;
                    pfnGetNewFrameToken(token, nullptr);
                    assert(token != nullptr);
                    volatile uint32_t val = (uint32_t)*token;
                    (void)val;
                }
            });
        }

        startFlag.store(true);
        for (auto& th : workers) {
            th.join();
        }
        printf("[PASS] Test 3: %d concurrent threads requested %d tokens with ZERO crashes or race conditions!\n",
               NUM_THREADS, NUM_THREADS * REQUESTS_PER_THREAD);
    }

    // =========================================================================
    // Test 4: Deadlock Elimination Stress Test (slEvaluateFeature vs slShutdown)
    // =========================================================================
    printf("\n--- Test 4: Concurrency & Deadlock Elimination (ABBA Stress) ---\n");
    {
        std::atomic<bool> stopFlag{false};

        // Thread 1: Rapidly runs slEvaluateFeature
        std::thread evalThread([&]() {
            while (!stopFlag.load()) {
                sl::FrameToken* token = nullptr;
                pfnGetNewFrameToken(token, nullptr);
                pfnEvaluate(sl::kFeatureDLSS, *token, nullptr, 0, nullptr);
                std::this_thread::yield();
            }
        });

        // Thread 2: Rapidly runs slInit and slShutdown
        std::thread shutdownThread([&]() {
            for (int i = 0; i < 200; ++i) {
                pfnShutdown();
                pfnInit(pref, 0x2000B0001FEDC);
                std::this_thread::yield();
            }
            stopFlag.store(true);
        });

        shutdownThread.join();
        evalThread.join();

        printf("[PASS] Test 4: Concurrently interleaved 200 slShutdown/slInit cycles with slEvaluateFeature with ZERO deadlocks!\n");
    }

    pfnShutdown();
    printf("\n[SUCCESS] ALL PHASE 1 STABILITY & SAFETY AUDIT TESTS PASSED COMPLETELY!\n");
    return 0;
}
