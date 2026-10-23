#include "framegen.h"
#include "nvngx_router.h"
#include "dxgi_proxy.h"
#include "../common/logger.h"
#include "../common/config.h"

#include <nvngx/nvsdk_ngx.h>
#include <nvngx/nvsdk_ngx_defs.h>
#include <nvngx/nvsdk_ngx_params.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metalstream::framegen {
namespace {

// ---------------------------------------------------------------------------
// vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7)
// ---------------------------------------------------------------------------
constexpr int kSlotFactoryCreateSwapChain = 10;        // IDXGIFactory
constexpr int kSlotFactoryCreateSwapChainForHwnd = 15; // IDXGIFactory2
constexpr int kSlotPresent = 8;                        // IDXGISwapChain
constexpr int kSlotResizeBuffers = 13;                 // IDXGISwapChain
constexpr int kSlotPresent1 = 22;                      // IDXGISwapChain1
constexpr int kSlotResizeBuffers1 = 39;                // IDXGISwapChain3
constexpr int kSlotGetBuffer = 9;                      // IDXGISwapChain
constexpr int kSlotGetCurrentBackBufferIndex = 36;     // IDXGISwapChain3

using PFN_CreateSwapChain = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_ResizeBuffers = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_ResizeBuffers1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using PFN_GetBuffer = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using PFN_GetCurrentBackBufferIndex = UINT (STDMETHODCALLTYPE*)(IDXGISwapChain3*);

std::mutex g_hookMutex;
PFN_CreateSwapChain g_origCreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd g_origCreateSwapChainForHwnd = nullptr;
PFN_Present g_origPresent = nullptr;
PFN_Present1 g_origPresent1 = nullptr;
PFN_ResizeBuffers g_origResizeBuffers = nullptr;
PFN_ResizeBuffers1 g_origResizeBuffers1 = nullptr;
PFN_GetBuffer g_origGetBuffer = nullptr;
PFN_GetCurrentBackBufferIndex g_origGetCurrentBackBufferIndex = nullptr;

// Our own Present calls go straight to the originals; this guards against a dxgi
// implementation that routes Present1 -> Present through its vtable.
thread_local int t_hookDepth = 0;

// Once a vtable slot points into this DLL it must never be unloaded: the game's swapchain and factory
// objects outlive a FreeLibrary(sl.interposer.dll) and would call into unmapped code
void PinSelf() {
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&PinSelf), &self)) {
            LOG_WARN("framegen: could not pin the module (error %lu)", GetLastError());
        }
    });
}

template <typename T>
bool PatchSlot(void* object, int slot, T hook, T& original, const char* name) {
    void** vtbl = *reinterpret_cast<void***>(object);
    if (vtbl[slot] == reinterpret_cast<void*>(hook)) return true;
    if (original && reinterpret_cast<void*>(original) != vtbl[slot]) {
        LOG_WARN("framegen: %s vtable %p has a different implementation (%p vs %p); not hooked",
                 name, vtbl, vtbl[slot], reinterpret_cast<void*>(original));
        return false;
    }
    PinSelf();
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("framegen: VirtualProtect failed for %s (error %lu)", name, GetLastError());
        return false;
    }
    original = reinterpret_cast<T>(vtbl[slot]);
    vtbl[slot] = reinterpret_cast<void*>(hook);
    VirtualProtect(&vtbl[slot], sizeof(void*), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[slot], sizeof(void*));
    LOG_INFO("framegen: hooked %s (vtable %p, original %p)", name, vtbl, reinterpret_cast<void*>(original));
    return true;
}

inline bool IsValidFloat(float v) {
    return std::isfinite(v) && std::fabs(v) < 3.0e38f;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!res || before == after || before == (D3D12_RESOURCE_STATES)UINT_MAX) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double TicksToMs(int64_t ticks) {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    return (double)ticks * 1000.0 / freq;
}

int64_t MsToTicks(double ms) {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    return (int64_t)(ms * freq / 1000.0);
}

// Sleep most of the way (Sleep() has ~1ms granularity), then spin for precision
void WaitUntil(int64_t target) {
    for (;;) {
        double remainingMs = TicksToMs(target - Now());
        if (remainingMs <= 0.0) return;
        if (remainingMs > 2.0) {
            Sleep((DWORD)(remainingMs - 1.5));
        } else {
            SwitchToThread();
        }
    }
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// ---------------------------------------------------------------------------
// Presenter: paced presentation on a dedicated thread (used when the game presents without vsync)
//
// With vsync off every Present returns immediately, so presenting the generated and the real frame
// back-to-back puts both on screen at nearly the same time and the compositor drops one of them.
// The game thread only records the GPU work and queues a job; this thread presents the generated
// frame, waits half a game frame interval, then presents the real frame. The game thread never
// sleeps for pacing, so the game frame rate is unaffected. The thread is the only user of the real
// swapchain while it runs (the game sees proxy backbuffers).
// ---------------------------------------------------------------------------
class Presenter {
public:
    struct Job {
        IDXGISwapChain* swapChain = nullptr;   // owned reference (Submit takes it): the game may release its
                                               // swapchain right after Present, before this job is shown
        ID3D12Resource* generated = nullptr;   // UNORDERED_ACCESS state; null = present the real frame only
        ID3D12Resource* real = nullptr;        // COPY_SOURCE state
        int64_t arrivalTicks = 0;
        int64_t halfIntervalTicks = 0;
        int64_t minGapTicks = 0;               // presents closer than this would land in the same refresh
        UINT flags = 0;
        uint64_t id = 0;
    };

    struct PacingStats {
        uint64_t presents = 0;
        double genToRealSumMs = 0, genToRealSqMs = 0, genToRealMaxMs = 0;
        double realToGenSumMs = 0, realToGenSqMs = 0, realToGenMaxMs = 0;
        uint64_t genToRealCount = 0, realToGenCount = 0;
        uint64_t skippedReal = 0; // real frame not shown because the next game frame was ready first
        uint64_t dropped = 0;     // stale jobs skipped because a newer game frame was already queued
        uint64_t bursts = 0;      // presents less than half a refresh apart (should stay ~0)
        uint64_t realOnlyFast = 0; // game too fast for generated frames: real frame shown alone
        double presentCallSumMs = 0, presentCallMaxMs = 0;
        uint64_t presentCalls = 0;
    };

    ~Presenter() { Stop(); }

    bool Start(ID3D12CommandQueue* queue) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_thread) return true;
        if (m_abandoned) return false;
        ID3D12Device* device = nullptr;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
        bool ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        for (auto& a : m_allocators) ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a.allocator)));
        ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].allocator, nullptr, IID_PPV_ARGS(&m_list)));
        device->Release();
        if (!ok) { ReleaseObjects(); return false; }
        m_list->Close();
        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_queue = queue;
        m_queue->AddRef();
        m_stop = false;
        m_thread = CreateThread(nullptr, 0, &Presenter::ThreadMain, this, 0, nullptr);
        if (!m_thread) { ReleaseObjects(); return false; }
        LOG_INFO("framegen: present thread started");
        return true;
    }

    void Stop() {
        HANDLE thread = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_thread) return;
            m_stop = true;
            thread = m_thread;
        }
        m_cv.notify_all();
        const bool exited = WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0;
        CloseHandle(thread);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_thread = nullptr;
        if (!exited) {
            // Still stuck inside a Present: its command list, queue and swapchain references must stay
            // valid until it returns (it then sees m_stop and exits). Leak them and present inline from now on.
            m_abandoned = true;
            LOG_ERROR("framegen: present thread did not stop within 2s; abandoning it (inline presents from now on)");
            return;
        }
        WaitFence(m_fenceValue);
        ReleaseObjects();
    }

    bool Running() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_thread != nullptr;
    }

    uint64_t Submit(Job job) {
        job.swapChain->AddRef();
        std::lock_guard<std::mutex> lock(m_mutex);
        job.id = ++m_submitted;
        m_jobs.push_back(job);
        m_cv.notify_all();
        return job.id;
    }

    // Blocks until job `id` has been presented (or the timeout expires)
    bool WaitCompleted(uint64_t id, DWORD timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return m_completed >= id || !m_thread; });
    }

    // Blocks until every submitted job has been presented
    void WaitIdle() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return m_completed >= m_submitted || !m_thread; });
    }

    HRESULT LastResult() const { return m_lastResult.load(); }
    // Frames shown for the last presented game frame (generated + real; 1 when only the real one fit)
    uint32_t LastShown() const { return m_lastShown.load(std::memory_order_relaxed); }

    PacingStats TakeStats() {
        std::lock_guard<std::mutex> lock(m_mutex);
        PacingStats s = m_stats;
        m_stats = PacingStats{};
        return s;
    }

private:
    static DWORD WINAPI ThreadMain(LPVOID param) {
        t_hookDepth = 1; // everything this thread calls must reach the real swapchain
        static_cast<Presenter*>(param)->Run();
        return 0;
    }

    void Run() {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_cv.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
            if (m_stop) return;
            // When the game produces frames faster than they can be shown (no frame cap, presents blocked
            // by the display), only the newest frame matters: drop stale ones instead of presenting them
            // back-to-back, which would add latency and judder
            std::vector<IDXGISwapChain*> droppedRefs;
            while (m_jobs.size() > 1) {
                m_completed = m_jobs.front().id;
                droppedRefs.push_back(m_jobs.front().swapChain);
                m_jobs.pop_front();
                m_stats.dropped++;
            }
            Job job = m_jobs.front();
            m_jobs.pop_front();
            lock.unlock();
            m_cv.notify_all();
            for (IDXGISwapChain* sc : droppedRefs) sc->Release();

            const bool stopped = !PresentJob(job);
            // Drop our reference before reporting completion: waiters (swapchain creation, resize) rely on it
            job.swapChain->Release();
            if (stopped) return;

            lock.lock();
            m_completed = job.id;
            m_cv.notify_all();
        }
    }

    // Presents one job; returns false when the thread is stopping
    bool PresentJob(Job& job) {
        // Never present faster than the display refreshes: presents closer than one refresh land in
        // the same refresh and one of them is never seen
        m_minGapTicks = job.minGapTicks;
        bool presentReal = true;
        // A game running faster than ~1.5 refreshes per frame leaves no room for a generated frame:
        // show the real frames only (lower latency, no interpolation artifacts)
        if (job.generated && 2 * job.halfIntervalTicks < 3 * job.minGapTicks / 2) {
            job.generated = nullptr;
            std::lock_guard<std::mutex> statsLock(m_mutex);
            m_stats.realOnlyFast++;
        }
        if (job.generated) {
            if (WaitUntilOrNewJob(m_lastPresentTicks + job.minGapTicks, false) == WaitResult::Stopped) return false;
            PresentCopy(job, job.generated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            // Show the generated frame for half a game frame. If the next game frame is ready first,
            // skip this real frame: the next generated frame is later in time, so the sequence stays
            // ordered and no two presents bunch up
            int64_t target = m_lastPresentTicks + std::max(job.halfIntervalTicks, job.minGapTicks);
            WaitResult r = WaitUntilOrNewJob(target, true);
            if (r == WaitResult::Stopped) return false;
            if (r == WaitResult::NewJob) {
                presentReal = false;
                std::lock_guard<std::mutex> statsLock(m_mutex);
                m_stats.skippedReal++;
            }
        } else if (WaitUntilOrNewJob(m_lastPresentTicks + job.minGapTicks, false) == WaitResult::Stopped) {
            return false;
        }
        if (presentReal) PresentCopy(job, job.real, D3D12_RESOURCE_STATE_COPY_SOURCE, false);
        m_lastShown = (job.generated ? 1u : 0u) + (presentReal ? 1u : 0u);
        return true;
    }

    enum class WaitResult { Reached, NewJob, Stopped };

    // Sleeps until `target`; returns early when stopping, or (if interruptible) when a job is queued
    WaitResult WaitUntilOrNewJob(int64_t target, bool interruptible) {
        for (;;) {
            {
                std::lock_guard<std::mutex> peek(m_mutex);
                if (m_stop) return WaitResult::Stopped;
                if (interruptible && !m_jobs.empty()) return WaitResult::NewJob;
            }
            int64_t now = Now();
            if (now >= target) return WaitResult::Reached;
            double remainingMs = TicksToMs(target - now);
            if (remainingMs > 2.0) Sleep((DWORD)(remainingMs - 1.5));
            else SwitchToThread();
        }
    }

    void PresentCopy(const Job& job, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState, bool generated) {
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(job.swapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
        ID3D12Resource* backbuffer = nullptr;
        UINT index = g_origGetCurrentBackBufferIndex(sc3);
        if (SUCCEEDED(g_origGetBuffer(sc3, index, IID_PPV_ARGS(&backbuffer))) && backbuffer) {
            auto& slot = m_allocators[m_allocatorIndex];
            WaitFence(slot.fenceValue);
            if (SUCCEEDED(slot.allocator->Reset()) && SUCCEEDED(m_list->Reset(slot.allocator, nullptr))) {
                Transition(m_list, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(m_list, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
                m_list->CopyResource(backbuffer, src);
                Transition(m_list, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
                Transition(m_list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
                m_list->Close();
                ID3D12CommandList* lists[] = { m_list };
                m_queue->ExecuteCommandLists(1, lists);
                m_queue->Signal(m_fence, ++m_fenceValue);
                slot.fenceValue = m_fenceValue;
                m_allocatorIndex = (m_allocatorIndex + 1) % kAllocators;
            }
            backbuffer->Release();
            int64_t tCall = Now();
            m_lastResult = g_origPresent(sc3, 0, job.flags);
            double callMs = TicksToMs(Now() - tCall);
            {
                std::lock_guard<std::mutex> statsLock(m_mutex);
                m_stats.presentCallSumMs += callMs;
                m_stats.presentCalls++;
                if (callMs > m_stats.presentCallMaxMs) m_stats.presentCallMaxMs = callMs;
            }
            RecordPresent(generated);
        }
        sc3->Release();
    }

    void RecordPresent(bool generated) {
        int64_t now = Now();
        if (m_lastPresentTicks) {
            double gap = TicksToMs(now - m_lastPresentTicks);
            std::lock_guard<std::mutex> lock(m_mutex);
            if (gap < 0.5 * TicksToMs(m_minGapTicks)) m_stats.bursts++;
            if (!generated && m_lastWasGenerated) {
                m_stats.genToRealSumMs += gap; m_stats.genToRealSqMs += gap * gap; m_stats.genToRealCount++;
                if (gap > m_stats.genToRealMaxMs) m_stats.genToRealMaxMs = gap;
            } else if (generated && !m_lastWasGenerated && gap < 100.0) {
                m_stats.realToGenSumMs += gap; m_stats.realToGenSqMs += gap * gap; m_stats.realToGenCount++;
                if (gap > m_stats.realToGenMaxMs) m_stats.realToGenMaxMs = gap;
            }
            m_stats.presents++;
        }
        m_lastPresentTicks = now;
        m_lastWasGenerated = generated;
    }

    void WaitFence(UINT64 value) {
        if (!m_fence || value == 0 || m_fence->GetCompletedValue() >= value) return;
        m_fence->SetEventOnCompletion(value, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, 1000);
    }

    void ReleaseObjects() {
        SafeRelease(m_list);
        for (auto& a : m_allocators) { SafeRelease(a.allocator); a.fenceValue = 0; }
        SafeRelease(m_fence);
        SafeRelease(m_queue);
        if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
        m_fenceValue = 0;
        for (Job& job : m_jobs) job.swapChain->Release();
        m_jobs.clear();
        m_completed = m_submitted;
    }

    static constexpr int kAllocators = 4;
    struct AllocatorSlot {
        ID3D12CommandAllocator* allocator = nullptr;
        UINT64 fenceValue = 0;
    };

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Job> m_jobs;
    uint64_t m_submitted = 0;
    uint64_t m_completed = 0;
    bool m_stop = false;
    bool m_abandoned = false; // a Stop() timed out: never start again
    HANDLE m_thread = nullptr;
    std::atomic<HRESULT> m_lastResult{S_OK};
    std::atomic<uint32_t> m_lastShown{2};
    PacingStats m_stats;

    // Present-thread only
    ID3D12CommandQueue* m_queue = nullptr;
    ID3D12Fence* m_fence = nullptr;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    AllocatorSlot m_allocators[kAllocators];
    int m_allocatorIndex = 0;
    ID3D12GraphicsCommandList* m_list = nullptr;
    int64_t m_lastPresentTicks = 0;
    int64_t m_minGapTicks = 0;
    bool m_lastWasGenerated = false;
};

// ---------------------------------------------------------------------------
// FrameGenerator
// ---------------------------------------------------------------------------
class FrameGenerator {
public:
    // Intentionally never destroyed: a static destructor would stop the present thread and release
    // D3D12 objects from DLL_PROCESS_DETACH, under the loader lock, after D3D12 may be gone
    static FrameGenerator& Get() {
        static FrameGenerator* s = new FrameGenerator();
        return *s;
    }

    // createdHere: sc was just created. It may have the address of a swapchain the game destroyed, so
    // never keep that one's proxy buffers (size and count may differ).
    void RegisterQueue(IDXGISwapChain* sc, ID3D12CommandQueue* queue, bool createdHere) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_swapChain == sc && m_queue == queue && !createdHere) return;
        if (m_queue == queue) {
            // Same queue (e.g. the game recreated its swapchain): keep the queue objects, never wait on the GPU here
            m_presenter.WaitIdle();
            ReleaseSizeDependentLocked();
            DropOrphanLocked(sc); // sc reuses the address of a destroyed swapchain
            if (m_swapChain && m_swapChain != sc && m_fakeCount) {
                OrphanProxyBuffersLocked();
            } else {
                RetireProxyBuffersLocked();
            }
            m_proxyFor = nullptr;
            m_swapChain = sc;
            m_lastTrackedPresent = m_presentCount;
            m_active = false;
            LOG_INFO("framegen: tracking swapchain %p presented on queue %p", sc, queue);
            return;
        }
        ReleaseAllLocked();
        m_swapChain = sc;
        m_lastTrackedPresent = m_presentCount;
        m_queue = queue;
        m_queue->AddRef();
        LOG_INFO("framegen: tracking swapchain %p presented on queue %p", sc, queue);
    }

    bool IsActive() const { return m_active.load(std::memory_order_relaxed); }

    // DLSSGState::numFramesActuallyPresented: frames presented per game frame, as actually shown
    uint32_t FramesPresentedPerGameFrame() const {
        if (!IsActive()) return 1;
        uint32_t n = m_threadedPresents.load(std::memory_order_relaxed) ? m_presenter.LastShown()
                                                                        : m_inlineShown.load(std::memory_order_relaxed);
        return n ? n : 1;
    }

    // The game renders into proxy backbuffers (see GetBuffer/GetCurrentBackBufferIndex hooks) whose
    // index advances by exactly one per game Present, as the game expects. We own the real swapchain:
    // its buffers are filled by copies and presented once or twice per game frame. Engines that track
    // the backbuffer index themselves (Unity) would otherwise render into the wrong buffer.
    //
    // With vsync the two presents are spaced by the display itself and run inline. Without vsync they
    // go to the Presenter thread, which spaces them by half a game frame.
    HRESULT Present(IDXGISwapChain* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params, bool isPresent1) {
        auto callOriginal = [&]() -> HRESULT {
            ++t_hookDepth;
            HRESULT hr = isPresent1 ? g_origPresent1(static_cast<IDXGISwapChain1*>(sc), sync, flags, params)
                                    : g_origPresent(sc, sync, flags);
            --t_hookDepth;
            return hr;
        };

        if (t_hookDepth > 0 || (flags & DXGI_PRESENT_TEST)) {
            return callOriginal();
        }

        const bool threaded = m_config.pacing != 0 && sync == 0 && m_config.debugView == 0;
        // Frame cap while generating without vsync: more game frames than the display can show twice
        // are wasted and only add judder. Done before taking any lock.
        if (threaded && m_active.load(std::memory_order_relaxed)) {
            ApplyFrameCap();
        }

        // Snapshot Streamline state first (takes the interposer state mutex; never hold m_mutex while doing so)
        Inputs in;
        bool haveInputs = m_config.frameGeneration != 0 && CollectInputs(in) && in.requested &&
                          in.depth.resource && in.mvecs.resource;

        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_presentCount;
        ProcessRetiredLocked(false);
        AgeOrphansLocked();

        if (sc == m_swapChain) m_lastTrackedPresent = m_presentCount;
        // The tracked swapchain went quiet (e.g. a temporary second swapchain the game has destroyed) while
        // an older one keeps presenting: generate frames for that one again
        if (sc != m_swapChain && m_presentCount - m_lastTrackedPresent > kOrphanIdlePresents && FindOrphanLocked(sc)) {
            PromoteOrphanLocked(sc);
        }

        const bool proxied = sc == m_swapChain && m_proxyFor == sc && m_fakeCount > 0;
        if (OrphanProxy* o = proxied ? nullptr : FindOrphanLocked(sc)) {
            // An older swapchain the game still renders into through its proxy buffers: show them as is
            ID3D12Resource* orphanSource = o->fake[o->index];
            o->index = (o->index + 1) % o->count;
            o->lastPresent = m_presentCount;
            IDXGISwapChain3* orphanSc3 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&orphanSc3))) && orphanSc3) {
                // A mismatch means this is a different swapchain at a destroyed one's address
                if (!CopyToRealBackbufferLocked(orphanSc3, orphanSource, D3D12_RESOURCE_STATE_PRESENT)) DropOrphanLocked(sc);
                orphanSc3->Release();
            }
            lock.unlock();
            return callOriginal();
        }
        if (!proxied) {
            if (haveInputs && !m_warnedUntracked) {
                m_warnedUntracked = true;
                LOG_WARN("framegen: DLSS-G requested but swapchain %p is not proxied (tracked %p, proxy %p, buffers %u); "
                         "frame generation needs a swapchain created after slInit", sc, m_swapChain, m_proxyFor, m_fakeCount);
            }
            m_active = false;
            lock.unlock();
            return callOriginal();
        }

        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
            lock.unlock();
            return callOriginal();
        }
        // The frame the game just finished, in its proxy backbuffer (state PRESENT)
        ID3D12Resource* source = m_fake[m_fakeIndex];
        m_fakeIndex = (m_fakeIndex + 1) % m_fakeCount;

        const bool generate = haveInputs && EnsureResourcesLocked(source);
        const bool useThread = threaded && generate && m_presenter.Start(m_queue);
        if (!useThread) {
            // Inline presents must never overlap with the present thread
            m_presenter.WaitIdle();
        }
        if (!generate) {
            if (m_active.exchange(false)) {
                LOG_INFO("framegen: deactivated (requested=%d depth=%p mvecs=%p)",
                         (int)in.requested, in.depth.resource, in.mvecs.resource);
            }
            m_havePrevFrame = false;
            // On a failed copy still present (an old frame): Present must return a DXGI result, never E_FAIL
            CopyToRealBackbufferLocked(sc3, source, D3D12_RESOURCE_STATE_PRESENT);
            sc3->Release();
            lock.unlock();
            return callOriginal();
        }

        int64_t tStart = Now();
        if ((sync != m_lastSync || flags != m_lastFlags) && m_presentParamLogs < 20) {
            ++m_presentParamLogs;
            m_lastSync = sync;
            m_lastFlags = flags;
            LOG_INFO("framegen: game presents with SyncInterval=%u Flags=0x%X (Present%s) -> %s", sync, flags,
                     isPresent1 ? "1" : "", useThread ? "paced present thread" : "inline presents");
        }
        if (m_fence) {
            // How far the GPU lags behind our submissions (command lists)
            UINT64 inflight = m_fenceValue - m_fence->GetCompletedValue();
            m_stats.inflightSum += inflight;
            if (inflight > m_stats.inflightMax) m_stats.inflightMax = inflight;
            m_stats.inflightSamples++;
        }
        // Game frame interval (entry to entry), smoothed; drives the pacing of the real frame
        if (m_lastEntryTicks) {
            double dtMs = TicksToMs(tStart - m_lastEntryTicks);
            if (dtMs > 0.0 && dtMs < 100.0) {
                m_frameIntervalMs = m_frameIntervalMs > 0.0 ? m_frameIntervalMs * 0.9 + dtMs * 0.1 : dtMs;
            }
        }
        m_lastEntryTicks = tStart;

        if (!m_active.exchange(true)) {
            LOG_INFO("framegen: activated on viewport %u (%ux%u fmt=%d, hudless=%p depth=%p mvecs=%p)",
                     in.viewport, m_width, m_height, (int)m_format, in.hudless.resource, in.depth.resource, in.mvecs.resource);
        }

        // Output slot: the present thread may still be copying out of it
        OutputSlot& slot = m_slots[m_slotIndex];
        if (useThread && slot.jobId && !m_presenter.WaitCompleted(slot.jobId, 100)) {
            LOG_WARN("framegen: present thread did not release output slot %d in time", m_slotIndex);
        }

        // ---- Interpolate between the previous and the current real frame ----
        const bool reset = !m_havePrevFrame ||
                           (in.hasConstants && in.constants.reset == sl::Boolean::eTrue);
        ID3D12Resource* realBuffer = useThread ? nullptr : AcquireRealBackbufferLocked(sc3);
        ID3D12GraphicsCommandList* list = (useThread || realBuffer) ? BeginListLocked() : nullptr;
        bool evalOk = false;
        if (list) {
            const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Transition(list, source, D3D12_RESOURCE_STATE_PRESENT, srv);
            Transition(list, in.hudless.resource, in.hudless.state, srv);
            Transition(list, in.depth.resource, in.depth.state, srv);
            Transition(list, in.mvecs.resource, in.mvecs.state, srv);

            evalOk = EvaluateLocked(list, source, slot.interp, in, reset);

            Transition(list, in.hudless.resource, srv, in.hudless.state);
            Transition(list, in.depth.resource, srv, in.depth.state);
            Transition(list, in.mvecs.resource, srv, in.mvecs.state);
            Transition(list, source, srv, D3D12_RESOURCE_STATE_PRESENT);

            const bool showGenerated = evalOk && !reset;
            if (useThread) {
                // The present thread may run after the game reuses its proxy buffer: keep a copy
                Transition(list, slot.real, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                Transition(list, source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->CopyResource(slot.real, source);
                Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
                Transition(list, slot.real, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            } else if (showGenerated) {
                // First present shows the generated frame; without one (reset/failure) show the real frame
                RecordCopyLocked(list, realBuffer, slot.interp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            } else {
                RecordCopyLocked(list, realBuffer, source, D3D12_RESOURCE_STATE_PRESENT);
            }
            const bool capturing = PollCaptureHotkeyLocked() && evalOk;
            if (capturing) RecordCaptureLocked(list, source, slot.interp, in);
            EndListLocked();
            if (capturing) FinishCaptureLocked(in, reset);
        }
        SafeRelease(realBuffer);
        m_havePrevFrame = evalOk;

        if (useThread && list) {
            Presenter::Job job;
            job.swapChain = sc;
            job.generated = (evalOk && !reset) ? slot.interp : nullptr;
            job.real = slot.real;
            job.arrivalTicks = tStart;
            job.halfIntervalTicks = MsToTicks(std::min(m_frameIntervalMs * 0.5, 25.0));
            job.minGapTicks = MsToTicks(900.0 / m_refreshHz); // 90% of a refresh period, tolerates timer jitter
            job.flags = flags;
            slot.jobId = m_presenter.Submit(job);
            m_threadedPresents = true;
            m_slotIndex = (m_slotIndex + 1) % kOutputSlots;
            if (job.generated) m_stats.generated++; else m_stats.realOnly++;
            m_stats.threaded++;
            m_stats.recordTicks += Now() - tStart;
            MaybeLogStatsLocked(Now());
            sc3->Release();
            return m_presenter.LastResult();
        }

        m_threadedPresents = false;
        if (!list || !evalOk || reset) {
            m_inlineShown = 1;
            sc3->Release();
            m_stats.realOnly++;
            lock.unlock();
            return callOriginal();
        }

        // m_mutex is never held across a real Present: it can block for a refresh with vsync (slSetTag on
        // other threads waits on m_mutex), and a window procedure run from inside Present may resize the
        // swapchain, which takes m_mutex again on this thread
        int64_t tRecorded = Now();
        const uint64_t epoch = m_epoch;
        lock.unlock();
        HRESULT hrInterp = callOriginal();
        int64_t tInterpPresented = Now();
        lock.lock();

        if (m_config.debugView == 1 || epoch != m_epoch) {
            // Debug: only generated frames reach the screen. Epoch changed: the swapchain was resized or
            // replaced during that Present, so `source` is no longer this frame's buffer: skip the real frame.
            m_inlineShown = 1;
            sc3->Release();
            m_stats.generated++;
            m_stats.recordTicks += tRecorded - tStart;
            m_stats.presentInterpTicks += tInterpPresented - tRecorded;
            MaybeLogStatsLocked(tInterpPresented);
            return hrInterp;
        }

        // Present the real frame (the proxy buffer is untouched until the game reuses it, which happens
        // after this copy on the same queue); with vsync this waits for the next display refresh
        HRESULT hrReal = hrInterp;
        if (SUCCEEDED(hrInterp) && CopyToRealBackbufferLocked(sc3, source, D3D12_RESOURCE_STATE_PRESENT)) {
            lock.unlock();
            hrReal = callOriginal();
            lock.lock();
            m_inlineShown = SUCCEEDED(hrReal) ? 2 : 1;
        } else {
            m_inlineShown = 1;
        }
        int64_t tEnd = Now();
        sc3->Release();

        m_stats.generated++;
        m_stats.recordTicks += tRecorded - tStart;
        m_stats.presentInterpTicks += tInterpPresented - tRecorded;
        m_stats.presentRealTicks += tEnd - tInterpPresented;
        MaybeLogStatsLocked(tEnd);
        return hrReal;
    }

    // Holds the game thread so that game frames are no closer than m_minFrameIntervalMs
    void ApplyFrameCap() {
        if (m_minFrameIntervalMs <= 0.0) return;
        int64_t last = m_lastCapTicks.load(std::memory_order_relaxed);
        if (last) WaitUntil(last + MsToTicks(m_minFrameIntervalMs));
        m_lastCapTicks.store(Now(), std::memory_order_relaxed);
    }

    // ---- Proxy backbuffers ----
    // Returns true and a proxy buffer when the game asks for a buffer of the proxied swapchain
    bool GetProxyBuffer(IDXGISwapChain* sc, UINT index, REFIID riid, void** out, HRESULT* hr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (OrphanProxy* o = FindOrphanLocked(sc)) {
            if (index >= o->count) { *hr = DXGI_ERROR_INVALID_CALL; return true; }
            *hr = o->fake[index]->QueryInterface(riid, out);
            return true;
        }
        if (m_proxyFor != sc || sc != m_swapChain) return false;
        if (!EnsureProxyBuffersLocked(sc)) return false;
        if (index >= m_fakeCount) { *hr = DXGI_ERROR_INVALID_CALL; return true; }
        *hr = m_fake[index]->QueryInterface(riid, out);
        return true;
    }

    bool GetProxyIndex(IDXGISwapChain3* sc, UINT* index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (OrphanProxy* o = FindOrphanLocked(sc)) {
            *index = o->index;
            return true;
        }
        // Without proxy buffers (not fetched yet, or creating them failed) the game has the real ones
        if (m_proxyFor != static_cast<IDXGISwapChain*>(sc) || static_cast<IDXGISwapChain*>(sc) != m_swapChain ||
            m_fakeCount == 0) return false;
        *index = m_fakeIndex;
        return true;
    }

    void EnableProxy(IDXGISwapChain* sc) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_swapChain != sc || m_config.frameGeneration == 0) return;
        m_proxyFor = sc;
        LOG_INFO("framegen: proxy backbuffers enabled for swapchain %p", sc);
    }

    // The present thread holds a reference to the swapchain of each queued job: let it finish before the
    // game creates a new swapchain, which fails while the old one for the same window is still alive
    void BeforeCreateSwapChain() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_presenter.Stop(); // drops the references of any job still queued
    }

    void BeforeResize(IDXGISwapChain* sc) {
        // The game has already drained its own work before resizing and we hold no backbuffer
        // references, so just retire our size-dependent objects
        std::lock_guard<std::mutex> lock(m_mutex);
        if (DropOrphanLocked(sc)) return; // the game fetches the real buffers again after resizing
        if (sc != m_swapChain) return;    // another swapchain: leave ours alone
        // Stop the present thread rather than drain it: it presents on the swapchain being resized, and
        // a drain that times out would let it Present during the resize. It restarts on the next frame.
        m_presenter.Stop();
        ReleaseSizeDependentLocked();
        RetireProxyBuffersLocked();
    }

    ID3D12Resource* Preserve(ID3D12GraphicsCommandList* cmdList, sl::BufferType type, ID3D12Resource* res,
                             D3D12_RESOURCE_STATES state, D3D12_RESOURCE_STATES* outState) {
        if (!cmdList || !res || state == (D3D12_RESOURCE_STATES)UINT_MAX) return nullptr;
        // Called from slSetTag while the game is recording: must never wait on the GPU here.
        // Games may tag several different resources with the same type (e.g. per viewport or per
        // pass), so keep one persistent copy per source resource.
        std::lock_guard<std::mutex> lock(m_mutex);
        D3D12_RESOURCE_DESC desc = res->GetDesc();
        if (m_preserved.size() >= kMaxPreserved && m_preserved.find({type, res}) == m_preserved.end()) {
            // Evict the least recently tagged copy. Stored tags may still name it: CollectInputs checks
            // IsPreservedCopyAlive, and the release itself is deferred past any GPU use.
            auto lru = m_preserved.begin();
            for (auto it = m_preserved.begin(); it != m_preserved.end(); ++it) {
                if (it->second.lastUsed < lru->second.lastUsed) lru = it;
            }
            RetireLocked(lru->second.copy);
            m_preserved.erase(lru);
        }
        auto& entry = m_preserved[{type, res}];
        entry.lastUsed = m_presentCount;
        ID3D12Resource*& copy = entry.copy;
        if (copy) {
            D3D12_RESOURCE_DESC cd = copy->GetDesc();
            if (cd.Width != desc.Width || cd.Height != desc.Height || cd.Format != desc.Format) {
                RetireLocked(copy);
                copy = nullptr;
            }
        }
        if (!copy) {
            ID3D12Device* dev = nullptr;
            res->GetDevice(IID_PPV_ARGS(&dev));
            if (!dev) return nullptr;
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&copy));
            dev->Release();
            if (FAILED(hr)) {
                LOG_WARN("framegen: failed to allocate copy for volatile tag %u (hr=0x%08X)", type, (uint32_t)hr);
                copy = nullptr;
                return nullptr;
            }
            LOG_INFO("framegen: preserving volatile tag %u from %p (%llux%u fmt=%d)", type, res,
                     (unsigned long long)desc.Width, desc.Height, (int)desc.Format);
        }
        Transition(cmdList, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmdList, copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(copy, res);
        Transition(cmdList, copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmdList, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        if (outState) *outState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return copy;
    }

    bool IsPreservedCopyAlive(ID3D12Resource* res) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& kv : m_preserved) {
            if (kv.second.copy == res) return true;
        }
        return false;
    }

    // slShutdown: stop generating and release everything NGX owns (nvngx is unloaded right after), but
    // keep the proxy backbuffers and queue objects: the game still renders into the proxies and may keep
    // presenting its swapchain after slShutdown
    void Shutdown() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_presenter.Stop();
        FlushLocked();
        ReleaseSizeDependentLocked();
        for (auto& kv : m_preserved) RetireLocked(kv.second.copy);
        m_preserved.clear();
        ProcessRetiredLocked(true);
        ReleaseNGXLocked();
        m_active = false;
    }

private:
    static constexpr int kRingSize = 8;
    static constexpr size_t kMaxOrphans = 2;
    static constexpr uint64_t kOrphanIdlePresents = 600; // ~5-10 s of the tracked swapchain's presents

    struct OrphanProxy {
        IDXGISwapChain* swapChain = nullptr; // not owned
        ID3D12Resource* fake[DXGI_MAX_SWAP_CHAIN_BUFFERS] = {};
        UINT count = 0;
        UINT index = 0;
        uint64_t lastPresent = 0;
    };
    static constexpr uint64_t kRetireDelayPresents = 16;
    static constexpr size_t kMaxPreserved = 32;

    struct PreservedCopy {
        ID3D12Resource* copy = nullptr;
        uint64_t lastUsed = 0; // m_presentCount when last tagged
    };

    struct Retired {
        IUnknown* object = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        uint64_t releaseAt = 0;
    };

    struct PreservedKeyHash {
        size_t operator()(const std::pair<sl::BufferType, ID3D12Resource*>& k) const {
            return std::hash<void*>()(k.second) ^ (size_t(k.first) * 0x9E3779B97F4A7C15ull);
        }
    };

    struct Stats {
        uint64_t generated = 0;
        uint64_t realOnly = 0;
        int64_t recordTicks = 0;
        int64_t presentInterpTicks = 0;
        int64_t presentRealTicks = 0;
        uint64_t threaded = 0;
        uint64_t inflightSum = 0;
        uint64_t inflightMax = 0;
        uint64_t inflightSamples = 0;
        int64_t lastLogTicks = 0;
    };

    FrameGenerator() : m_config(Config::Get()) {
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        DWORD refreshHz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) ? mode.dmDisplayFrequency : 0;
        if (refreshHz <= 1) refreshHz = 60;
        m_refreshHz = refreshHz;
        if (m_config.maxGameFps < 0) {
            m_minFrameIntervalMs = 2000.0 / refreshHz; // each game frame becomes two displayed frames
        } else if (m_config.maxGameFps > 0) {
            m_minFrameIntervalMs = 1000.0 / m_config.maxGameFps;
        }
        LOG_INFO("framegen: display %lu Hz -> vsync-off game frame cap %.2f ms (FGMaxGameFps=%d)",
                 refreshHz, m_minFrameIntervalMs, m_config.maxGameFps);
    }

    bool LoadNGXLocked() {
        if (m_pfnEvaluate) return true;
        HMODULE ngx = NVNGXRouter::Instance().GetNVNGXModule();
        if (!ngx) return false;
        m_pfnAllocate = (PFN_Allocate)GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters");
        m_pfnDestroy = (PFN_Destroy)GetProcAddress(ngx, "NVSDK_NGX_D3D12_DestroyParameters");
        m_pfnCreate = (PFN_Create)GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature");
        m_pfnRelease = (PFN_Release)GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature");
        m_pfnEvaluate = (PFN_Evaluate)GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature");
        if (!m_pfnAllocate || !m_pfnCreate || !m_pfnEvaluate) {
            m_pfnEvaluate = nullptr;
            return false;
        }
        return true;
    }

    bool EnsureResourcesLocked(ID3D12Resource* backbuffer) {
        if (m_ngxFailed || !LoadNGXLocked()) return false;
        D3D12_RESOURCE_DESC bd = backbuffer->GetDesc();
        if (m_feature && bd.Width == m_width && bd.Height == m_height && bd.Format == m_format) return true;

        ReleaseSizeDependentLocked();

        if (!EnsureQueueObjectsLocked()) return false;

        m_width = (UINT)bd.Width;
        m_height = bd.Height;
        m_format = bd.Format;

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = m_width;
        td.Height = m_height;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = m_format;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        bool allocated = SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                     nullptr, IID_PPV_ARGS(&m_realOut)));
        for (auto& slot : m_slots) {
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            allocated = allocated && SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                                 nullptr, IID_PPV_ARGS(&slot.interp)));
            td.Flags = D3D12_RESOURCE_FLAG_NONE;
            allocated = allocated && SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                                 nullptr, IID_PPV_ARGS(&slot.real)));
        }
        if (!allocated) {
            LOG_ERROR("framegen: failed to allocate %ux%u output textures (fmt=%d)", m_width, m_height, (int)m_format);
            ReleaseSizeDependentLocked();
            return false;
        }

        if (!m_params && NVSDK_NGX_FAILED(m_pfnAllocate(&m_params))) {
            m_params = nullptr;
            m_ngxFailed = true;
            return false;
        }
        m_params->Set(NVSDK_NGX_Parameter_Width, m_width);
        m_params->Set(NVSDK_NGX_Parameter_Height, m_height);
        m_params->Set(NVSDK_NGX_Parameter_OutWidth, m_width);
        m_params->Set(NVSDK_NGX_Parameter_OutHeight, m_height);
        m_params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        m_params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

        ID3D12GraphicsCommandList* list = BeginListLocked();
        if (!list) return false;
        NVSDK_NGX_Result res = m_pfnCreate(list, NVSDK_NGX_Feature_FrameGeneration, m_params, &m_feature);
        EndListLocked();
        if (NVSDK_NGX_FAILED(res) || !m_feature) {
            // Typically 0xBAD00000 when nvngx_dlssg.dll is missing from the game directory
            LOG_ERROR("framegen: NGX CreateFeature(FrameGeneration) failed: 0x%08X - frame generation disabled", res);
            m_feature = nullptr;
            m_ngxFailed = true;
            ReleaseSizeDependentLocked();
            return false;
        }
        LOG_INFO("framegen: NGX FrameGeneration feature created (%ux%u)", m_width, m_height);
        m_havePrevFrame = false;
        return true;
    }

    bool EvaluateLocked(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer, ID3D12Resource* output, const Inputs& in, bool reset) {
        NVSDK_NGX_Parameter* p = m_params;
        p->Set("DLSSG.Backbuffer", backbuffer);
        p->Set("DLSSG.HUDLess", in.hudless.resource);
        p->Set("DLSSG.Depth", in.depth.resource);
        p->Set("DLSSG.MVecs", in.mvecs.resource);
        p->Set("DLSSG.OutputInterpolated", output);
        p->Set("DLSSG.OutputReal", m_realOut);
        p->Set("DLSSG.Reset", reset ? 1u : 0u);

        const sl::Constants& c = in.constants;
        const bool hc = in.hasConstants;
        // Streamline's mvecScale maps motion vectors to UV units, but D3DMetal's FrameGeneration wants
        // pixels of the motion-vector texture (verified by test_gptk_fg_probe's moving-bar test)
        uint32_t mvW = in.mvecs.extent.width, mvH = in.mvecs.extent.height;
        if (!in.mvecs.extent) {
            D3D12_RESOURCE_DESC md = in.mvecs.resource->GetDesc();
            mvW = (uint32_t)md.Width;
            mvH = md.Height;
        }
        const float uvScaleX = hc && IsValidFloat(c.mvecScale.x) ? c.mvecScale.x : 1.0f;
        const float uvScaleY = hc && IsValidFloat(c.mvecScale.y) ? c.mvecScale.y : 1.0f;
        m_lastMvecScaleX = uvScaleX * (float)mvW;
        m_lastMvecScaleY = uvScaleY * (float)mvH;
        p->Set("DLSSG.MvecScaleX", m_lastMvecScaleX);
        p->Set("DLSSG.MvecScaleY", m_lastMvecScaleY);
        p->Set("DLSSG.JitterOffsetX", hc && IsValidFloat(c.jitterOffset.x) ? c.jitterOffset.x : 0.0f);
        p->Set("DLSSG.JitterOffsetY", hc && IsValidFloat(c.jitterOffset.y) ? c.jitterOffset.y : 0.0f);
        p->Set("DLSSG.DepthInverted", (hc && c.depthInverted == sl::Boolean::eTrue) ? 1u : 0u);
        if (hc) {
            if (IsValidFloat(c.cameraNear)) p->Set("DLSSG.CameraNear", c.cameraNear);
            if (IsValidFloat(c.cameraFar)) p->Set("DLSSG.CameraFar", c.cameraFar);
            if (IsValidFloat(c.cameraFOV)) p->Set("DLSSG.CameraFOV", c.cameraFOV);
            if (IsValidFloat(c.cameraAspectRatio)) p->Set("DLSSG.CameraAspectRatio", c.cameraAspectRatio);
            // Streamline leaves unset camera data at INVALID_FLOAT (3.4e38); never forward that to MetalFX
            auto setVec = [&](const char* name, const sl::float3& v) {
                if (!IsValidFloat(v.x) || !IsValidFloat(v.y) || !IsValidFloat(v.z)) return;
                char key[64];
                snprintf(key, sizeof key, "DLSSG.%sX", name); p->Set(key, v.x);
                snprintf(key, sizeof key, "DLSSG.%sY", name); p->Set(key, v.y);
                snprintf(key, sizeof key, "DLSSG.%sZ", name); p->Set(key, v.z);
            };
            if (m_config.cameraData) {
            setVec("CameraPos", c.cameraPos);
            setVec("CameraUp", c.cameraUp);
            setVec("CameraRight", c.cameraRight);
            setVec("CameraFwd", c.cameraFwd);
            const float* m = reinterpret_cast<const float*>(&c.cameraViewToClip);
            bool matrixValid = false;
            for (int i = 0; i < 16; ++i) {
                if (!IsValidFloat(m[i])) { matrixValid = false; break; }
                if (m[i] != 0.0f) matrixValid = true;
            }
            if (matrixValid) {
                m_viewToClip = c.cameraViewToClip;
                p->Set("DLSSG.CameraViewToClip", static_cast<void*>(&m_viewToClip));
            }
            }
        }

        auto setSubrect = [&](const char* name, const sl::Extent& e, ID3D12Resource* res) {
            uint32_t w = e.width, h = e.height, x = e.left, y = e.top;
            if (!e && res) {
                D3D12_RESOURCE_DESC d = res->GetDesc();
                w = (uint32_t)d.Width;
                h = d.Height;
                x = y = 0;
            }
            char key[96];
            snprintf(key, sizeof key, "DLSSG.%sSubrectBaseX", name); p->Set(key, x);
            snprintf(key, sizeof key, "DLSSG.%sSubrectBaseY", name); p->Set(key, y);
            snprintf(key, sizeof key, "DLSSG.%sSubrectWidth", name); p->Set(key, w);
            snprintf(key, sizeof key, "DLSSG.%sSubrectHeight", name); p->Set(key, h);
        };
        setSubrect("Backbuffer", sl::Extent{}, backbuffer);
        setSubrect("Depth", in.depth.extent, in.depth.resource);
        setSubrect("MVecs", in.mvecs.extent, in.mvecs.resource);
        if (in.hudless.resource) setSubrect("HUDLess", in.hudless.extent, in.hudless.resource);

        NVSDK_NGX_Result res = m_pfnEvaluate(list, m_feature, p, nullptr);
        if (NVSDK_NGX_FAILED(res)) {
            if (m_evalFailures++ < 5) LOG_ERROR("framegen: NGX EvaluateFeature(FrameGeneration) failed: 0x%08X", res);
            return false;
        }
        if (!m_loggedFirstEval) {
            m_loggedFirstEval = true;
            LOG_INFO("framegen: first evaluate ok (sl mvecScale=%.4f,%.4f -> NGX MvecScale=%.1f,%.1f depthInv=%d hudless=%d)",
                     hc ? c.mvecScale.x : 1.0f, hc ? c.mvecScale.y : 1.0f, m_lastMvecScaleX, m_lastMvecScaleY,
                     (int)(hc && c.depthInverted == sl::Boolean::eTrue), (int)(in.hudless.resource != nullptr));
        }
        return true;
    }

    // ---- Frame capture (FGCapture=1, F10) ----
    struct CaptureItem {
        const char* name;
        ID3D12Resource* readback = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        D3D12_RESOURCE_DESC desc{};
    };

    bool PollCaptureHotkeyLocked() {
        if (!m_config.capture) return false;
        bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        // FGCapture=2: also trigger automatically once, after 120 frames (for tests)
        const bool autoTrigger = m_config.capture == 2 && ++m_captureFrameCounter == 120;
        if ((autoTrigger || (down && !m_captureKeyDown)) && m_captureRemaining == 0) {
            m_captureRemaining = 3;
            ++m_captureBurst;
            LOG_INFO("framegen: capture burst %u requested", m_captureBurst);
        }
        m_captureKeyDown = down;
        return m_captureRemaining > 0;
    }

    void AddCaptureCopyLocked(ID3D12GraphicsCommandList* list, const char* name, ID3D12Resource* res, D3D12_RESOURCE_STATES state) {
        if (!res) return;
        CaptureItem item;
        item.name = name;
        item.desc = res->GetDesc();
        m_device->GetCopyableFootprints(&item.desc, 0, 1, 0, &item.footprint, &item.rows, &item.rowBytes, nullptr);
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (UINT64)item.footprint.Footprint.RowPitch * item.rows;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&item.readback)))) return;
        Transition(list, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = item.readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = item.footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        m_captureItems.push_back(item);
    }

    void RecordCaptureLocked(ID3D12GraphicsCommandList* list, ID3D12Resource* real, ID3D12Resource* generated, const Inputs& in) {
        AddCaptureCopyLocked(list, "real", real, D3D12_RESOURCE_STATE_PRESENT);
        AddCaptureCopyLocked(list, "generated", generated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        AddCaptureCopyLocked(list, "hudless", in.hudless.resource, in.hudless.state);
        AddCaptureCopyLocked(list, "depth", in.depth.resource, in.depth.state);
        AddCaptureCopyLocked(list, "mvecs", in.mvecs.resource, in.mvecs.state);
    }

    // Waits for the capture copies (safe here: the game's frame is fully submitted at Present)
    void FinishCaptureLocked(const Inputs& in, bool reset) {
        WaitFenceLocked(m_fenceValue);
        std::wstring dir = CaptureDirectory();
        CreateDirectoryW(dir.c_str(), nullptr);
        const unsigned frameIndex = 3 - m_captureRemaining;
        wchar_t prefix[64];
        swprintf(prefix, 64, L"burst%02u_frame%u_", m_captureBurst, frameIndex);

        std::string meta;
        char line[512];
        for (auto& item : m_captureItems) {
            void* data = nullptr;
            D3D12_RANGE range = { 0, (SIZE_T)item.footprint.Footprint.RowPitch * item.rows };
            if (SUCCEEDED(item.readback->Map(0, &range, &data)) && data) {
                std::wstring path = dir + prefix + std::wstring(item.name, item.name + strlen(item.name)) + L".raw";
                HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(f, data, (DWORD)range.End, &written, nullptr);
                    CloseHandle(f);
                }
                item.readback->Unmap(0, nullptr);
            }
            snprintf(line, sizeof line, "%s width=%llu height=%u format=%d rowpitch=%u\n", item.name,
                     (unsigned long long)item.desc.Width, item.desc.Height, (int)item.desc.Format, item.footprint.Footprint.RowPitch);
            meta += line;
            item.readback->Release();
        }
        m_captureItems.clear();

        const sl::Constants& c = in.constants;
        snprintf(line, sizeof line, "reset=%d hasConstants=%d ngxMvecScale=%.6f,%.6f slMvecScale=%.6f,%.6f jitter=%.5f,%.5f depthInverted=%d mvecJittered=%d cameraMotionIncluded=%d\n",
                 (int)reset, (int)in.hasConstants, m_lastMvecScaleX, m_lastMvecScaleY, c.mvecScale.x, c.mvecScale.y,
                 c.jitterOffset.x, c.jitterOffset.y, (int)c.depthInverted, (int)c.motionVectorsJittered, (int)c.cameraMotionIncluded);
        meta += line;
        snprintf(line, sizeof line, "near=%.5f far=%.3f fov=%.6f aspect=%.6f pos=%.5f,%.5f,%.5f fwd=%.5f,%.5f,%.5f up=%.5f,%.5f,%.5f right=%.5f,%.5f,%.5f\n",
                 c.cameraNear, c.cameraFar, c.cameraFOV, c.cameraAspectRatio, c.cameraPos.x, c.cameraPos.y, c.cameraPos.z,
                 c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z, c.cameraUp.x, c.cameraUp.y, c.cameraUp.z,
                 c.cameraRight.x, c.cameraRight.y, c.cameraRight.z);
        meta += line;
        auto appendMatrix = [&](const char* name, const sl::float4x4& m) {
            const float* v = reinterpret_cast<const float*>(&m);
            snprintf(line, sizeof line, "%s=%g,%g,%g,%g;%g,%g,%g,%g;%g,%g,%g,%g;%g,%g,%g,%g\n", name,
                     v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
            meta += line;
        };
        appendMatrix("cameraViewToClip", c.cameraViewToClip);
        appendMatrix("clipToPrevClip", c.clipToPrevClip);
        appendMatrix("prevClipToClip", c.prevClipToClip);
        snprintf(line, sizeof line, "qpc_ms=%.3f frameIntervalEmaMs=%.3f\n", TicksToMs(Now()), m_frameIntervalMs);
        meta += line;

        std::wstring metaPath = dir + prefix + L"meta.txt";
        HANDLE f = CreateFileW(metaPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, meta.data(), (DWORD)meta.size(), &written, nullptr);
            CloseHandle(f);
        }
        --m_captureRemaining;
        LOG_INFO("framegen: captured burst %u frame %u", m_captureBurst, frameIndex);
    }

    static std::wstring CaptureDirectory() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&PinSelf), &self);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring dir(path);
        return dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"metalstream_capture\\";
    }

    bool EnsureDeviceLocked() {
        if (m_device) return true;
        return m_queue && SUCCEEDED(m_queue->GetDevice(IID_PPV_ARGS(&m_device))) && m_device;
    }

    bool EnsureQueueObjectsLocked() {
        if (m_fence) return true;
        if (!EnsureDeviceLocked()) return false;
        if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) return false;
        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (auto& slot : m_ring) {
            if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))) return false;
        }
        if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_ring[0].allocator, nullptr, IID_PPV_ARGS(&m_list)))) return false;
        m_list->Close();
        return true;
    }

    bool EnsureProxyBuffersLocked(IDXGISwapChain* sc) {
        if (m_fakeCount) return true;
        if (!EnsureDeviceLocked()) return false;
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (FAILED(sc->GetDesc(&desc)) || desc.BufferCount == 0 || desc.BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS) return false;
        for (UINT i = 0; i < desc.BufferCount; ++i) {
            ID3D12Resource* real = nullptr;
            if (FAILED(g_origGetBuffer(sc, i, IID_PPV_ARGS(&real))) || !real) break;
            D3D12_RESOURCE_DESC rd = real->GetDesc();
            real->Release();
            rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                                         IID_PPV_ARGS(&m_fake[i])))) {
                m_fake[i] = nullptr;
                break;
            }
            m_fakeCount = i + 1;
        }
        if (m_fakeCount != desc.BufferCount) {
            LOG_ERROR("framegen: failed to create proxy backbuffers (%u of %u)", m_fakeCount, desc.BufferCount);
            RetireProxyBuffersLocked();
            return false;
        }
        m_fakeIndex = 0;
        LOG_INFO("framegen: created %u proxy backbuffers (%ux%u fmt=%d) for swapchain %p",
                 m_fakeCount, desc.BufferDesc.Width, desc.BufferDesc.Height, (int)desc.BufferDesc.Format, sc);
        return true;
    }

    void RetireProxyBuffersLocked() {
        for (UINT i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; ++i) {
            RetireLocked(m_fake[i]);
            m_fake[i] = nullptr;
        }
        m_fakeCount = 0;
        m_fakeIndex = 0;
        ++m_epoch;
    }

    ID3D12Resource* AcquireRealBackbufferLocked(IDXGISwapChain3* sc3) {
        ID3D12Resource* real = nullptr;
        UINT index = g_origGetCurrentBackBufferIndex(sc3);
        if (FAILED(g_origGetBuffer(sc3, index, IID_PPV_ARGS(&real)))) return nullptr;
        return real;
    }

    // Copies src (in srcState, restored afterwards) into dst, a real backbuffer in PRESENT state
    void RecordCopyLocked(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState) {
        Transition(list, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list, dst, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(dst, src);
        Transition(list, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        Transition(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
    }

    bool CopyToRealBackbufferLocked(IDXGISwapChain3* sc3, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState) {
        if (!EnsureQueueObjectsLocked()) return false;
        ID3D12Resource* real = AcquireRealBackbufferLocked(sc3);
        if (!real) return false;
        // CopyResource between different sizes/formats is invalid (device removal): never record one
        D3D12_RESOURCE_DESC rd = real->GetDesc(), sd = src->GetDesc();
        if (rd.Width != sd.Width || rd.Height != sd.Height || rd.Format != sd.Format) {
            real->Release();
            return false;
        }
        ID3D12GraphicsCommandList* list = BeginListLocked();
        if (list) {
            RecordCopyLocked(list, real, src, srcState);
            EndListLocked();
        }
        real->Release();
        return list != nullptr;
    }

    ID3D12GraphicsCommandList* BeginListLocked() {
        if (!m_list) return nullptr;
        auto& slot = m_ring[m_ringIndex];
        WaitFenceLocked(slot.fenceValue);
        if (FAILED(slot.allocator->Reset()) || FAILED(m_list->Reset(slot.allocator, nullptr))) return nullptr;
        return m_list;
    }

    void EndListLocked() {
        m_list->Close();
        ID3D12CommandList* lists[] = { m_list };
        m_queue->ExecuteCommandLists(1, lists);
        m_queue->Signal(m_fence, ++m_fenceValue);
        m_ring[m_ringIndex].fenceValue = m_fenceValue;
        m_ringIndex = (m_ringIndex + 1) % kRingSize;
    }

    void WaitFenceLocked(UINT64 value) {
        if (!m_fence || value == 0 || m_fence->GetCompletedValue() >= value) return;
        m_fence->SetEventOnCompletion(value, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, 5000);
    }

    void FlushLocked() {
        if (!m_queue || !m_fence) return;
        m_queue->Signal(m_fence, ++m_fenceValue);
        WaitFenceLocked(m_fenceValue);
    }

    void RetireLocked(IUnknown* obj) {
        if (obj) m_retired.push_back({obj, nullptr, m_presentCount + kRetireDelayPresents});
    }

    // Releases objects retired long enough ago that our GPU work using them has finished
    void ProcessRetiredLocked(bool all) {
        for (size_t i = 0; i < m_retired.size();) {
            if (all || m_retired[i].releaseAt <= m_presentCount) {
                if (m_retired[i].object) m_retired[i].object->Release();
                if (m_retired[i].feature && m_pfnRelease) m_pfnRelease(m_retired[i].feature);
                m_retired[i] = m_retired.back();
                m_retired.pop_back();
            } else {
                ++i;
            }
        }
    }

    // Retires (deferred release, no GPU wait) everything tied to the backbuffer size
    void ReleaseSizeDependentLocked() {
        if (m_feature) m_retired.push_back({nullptr, m_feature, m_presentCount + kRetireDelayPresents});
        m_feature = nullptr;
        m_presenter.WaitIdle(); // it may still read the output slots
        for (auto& slot : m_slots) {
            RetireLocked(slot.interp);
            RetireLocked(slot.real);
            slot = OutputSlot{};
        }
        m_slotIndex = 0;
        RetireLocked(m_realOut);
        m_realOut = nullptr;
        m_width = m_height = 0;
        m_format = DXGI_FORMAT_UNKNOWN;
        m_havePrevFrame = false;
        ++m_epoch;
    }

    void ReleaseAllLocked() {
        m_presenter.Stop();
        FlushLocked();
        ReleaseSizeDependentLocked();
        RetireProxyBuffersLocked();
        m_proxyFor = nullptr;
        while (!m_orphans.empty()) RetireOrphanLocked(0);
        for (auto& kv : m_preserved) RetireLocked(kv.second.copy);
        m_preserved.clear();
        ProcessRetiredLocked(true);
        ReleaseNGXLocked();
        SafeRelease(m_list);
        for (auto& slot : m_ring) { SafeRelease(slot.allocator); slot.fenceValue = 0; }
        SafeRelease(m_fence);
        if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
        m_fenceValue = 0;
        m_ringIndex = 0;
        SafeRelease(m_device);
        SafeRelease(m_queue);
        m_swapChain = nullptr;
        m_active = false;
    }

    // Caller has released the feature (ProcessRetiredLocked(true))
    void ReleaseNGXLocked() {
        if (m_params && m_pfnDestroy) m_pfnDestroy(m_params);
        m_params = nullptr;
        // slShutdown frees the nvngx module after this; a later slInit loads it again (maybe elsewhere)
        m_pfnAllocate = nullptr;
        m_pfnDestroy = nullptr;
        m_pfnCreate = nullptr;
        m_pfnRelease = nullptr;
        m_pfnEvaluate = nullptr;
        m_ngxFailed = false;
    }

    // ---- Orphaned proxy buffers ----
    // When the game creates another swapchain on the same queue, we track the new one, but the old one
    // may still be alive and the game keeps rendering into its proxy buffers. Keep them and present them
    // without frame generation. We cannot tell a live swapchain from a destroyed one, so orphans that have
    // not presented for a while are released, and at most kMaxOrphans are kept.
    OrphanProxy* FindOrphanLocked(IDXGISwapChain* sc) {
        for (auto& o : m_orphans) {
            if (o.swapChain == sc) return &o;
        }
        return nullptr;
    }

    void OrphanProxyBuffersLocked() {
        if (m_orphans.size() >= kMaxOrphans) RetireOrphanLocked(0);
        OrphanProxy o;
        o.swapChain = m_swapChain;
        for (UINT i = 0; i < m_fakeCount; ++i) {
            o.fake[i] = m_fake[i];
            m_fake[i] = nullptr;
        }
        o.count = m_fakeCount;
        o.index = m_fakeIndex;
        o.lastPresent = m_presentCount;
        m_orphans.push_back(o);
        m_fakeCount = 0;
        m_fakeIndex = 0;
        ++m_epoch;
        LOG_INFO("framegen: swapchain %p keeps its %u proxy backbuffers without frame generation", o.swapChain, o.count);
    }

    void RetireOrphanLocked(size_t i) {
        for (UINT b = 0; b < m_orphans[i].count; ++b) RetireLocked(m_orphans[i].fake[b]);
        m_orphans.erase(m_orphans.begin() + i);
    }

    bool DropOrphanLocked(IDXGISwapChain* sc) {
        for (size_t i = 0; i < m_orphans.size(); ++i) {
            if (m_orphans[i].swapChain == sc) {
                RetireOrphanLocked(i);
                return true;
            }
        }
        return false;
    }

    // The orphan becomes the tracked, proxied swapchain again; the tracked one (if it has proxy buffers)
    // becomes an orphan in its place
    void PromoteOrphanLocked(IDXGISwapChain* sc) {
        size_t i = 0;
        while (m_orphans[i].swapChain != sc) ++i;
        OrphanProxy o = m_orphans[i];
        m_orphans.erase(m_orphans.begin() + i);
        m_presenter.Stop(); // it may hold jobs for the previous swapchain
        ReleaseSizeDependentLocked();
        if (m_fakeCount) OrphanProxyBuffersLocked(); else RetireProxyBuffersLocked();
        for (UINT b = 0; b < o.count; ++b) m_fake[b] = o.fake[b];
        m_fakeCount = o.count;
        m_fakeIndex = o.index;
        m_swapChain = sc;
        m_proxyFor = sc;
        m_lastTrackedPresent = m_presentCount;
        m_active = false;
        LOG_INFO("framegen: tracking swapchain %p again (the newer one stopped presenting)", sc);
    }

    void AgeOrphansLocked() {
        for (size_t i = 0; i < m_orphans.size();) {
            if (m_presentCount - m_orphans[i].lastPresent > kOrphanIdlePresents) {
                LOG_INFO("framegen: releasing the proxy backbuffers of idle swapchain %p", m_orphans[i].swapChain);
                RetireOrphanLocked(i);
            } else {
                ++i;
            }
        }
    }

    void MaybeLogStatsLocked(int64_t now) {
        if (m_stats.lastLogTicks == 0) { m_stats.lastLogTicks = now; return; }
        double elapsedMs = TicksToMs(now - m_stats.lastLogTicks);
        if (elapsedMs < 5000.0) return;
        uint64_t n = m_stats.generated ? m_stats.generated : 1;
        const uint64_t presentsPerGenerated = m_config.debugView == 1 ? 1 : 2;
        // With the present thread, count what it actually presented (it drops frames the display can't show)
        Presenter::PacingStats ps{};
        if (m_stats.threaded) ps = m_presenter.TakeStats();
        const double presentedFps = m_stats.threaded ? ps.presents * 1000.0 / elapsedMs
                                                     : (presentsPerGenerated * m_stats.generated + m_stats.realOnly) * 1000.0 / elapsedMs;
        LOG_INFO("framegen: %.1fs: generated=%llu realOnly=%llu (game %.1f fps -> %.1f presented fps, interval ema %.2fms, %s) | record %.2fms, present(interp) %.2fms, present(real) %.2fms",
                 elapsedMs / 1000.0, (unsigned long long)m_stats.generated, (unsigned long long)m_stats.realOnly,
                 (m_stats.generated + m_stats.realOnly) * 1000.0 / elapsedMs, presentedFps, m_frameIntervalMs,
                 m_stats.threaded ? "present thread" : "inline",
                 TicksToMs(m_stats.recordTicks) / n, TicksToMs(m_stats.presentInterpTicks) / n,
                 TicksToMs(m_stats.presentRealTicks) / n);
        if (m_stats.inflightSamples) {
            LOG_INFO("framegen:   GPU backlog %.2f command lists avg / %llu max",
                     (double)m_stats.inflightSum / m_stats.inflightSamples, (unsigned long long)m_stats.inflightMax);
        }
        if (m_stats.threaded) {
            // Ideal pacing: both gaps equal half the game frame interval, small deviation
            auto avg = [](double sum, uint64_t n) { return n ? sum / n : 0.0; };
            auto dev = [](double sum, double sq, uint64_t n) {
                if (!n) return 0.0;
                double m = sum / n, v = sq / n - m * m;
                return v > 0 ? std::sqrt(v) : 0.0;
            };
            LOG_INFO("framegen:   pacing: generated->real %.2fms (sd %.2f, max %.2f), real->generated %.2fms (sd %.2f, max %.2f), presents %llu, bursts %llu, skipped real %llu, real-only (fast game) %llu, dropped %llu, Present call %.2fms avg / %.2fms max",
                     avg(ps.genToRealSumMs, ps.genToRealCount), dev(ps.genToRealSumMs, ps.genToRealSqMs, ps.genToRealCount), ps.genToRealMaxMs,
                     avg(ps.realToGenSumMs, ps.realToGenCount), dev(ps.realToGenSumMs, ps.realToGenSqMs, ps.realToGenCount), ps.realToGenMaxMs,
                     (unsigned long long)ps.presents, (unsigned long long)ps.bursts, (unsigned long long)ps.skippedReal,
                     (unsigned long long)ps.realOnlyFast,
                     (unsigned long long)ps.dropped,
                     avg(ps.presentCallSumMs, ps.presentCalls), ps.presentCallMaxMs);
        }
        m_stats = Stats{};
        m_stats.lastLogTicks = now;
    }

    using PFN_Allocate = NVSDK_NGX_Result (NVSDK_CONV*)(NVSDK_NGX_Parameter**);
    using PFN_Destroy = NVSDK_NGX_Result (NVSDK_CONV*)(NVSDK_NGX_Parameter*);
    using PFN_Create = NVSDK_NGX_Result (NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using PFN_Release = NVSDK_NGX_Result (NVSDK_CONV*)(NVSDK_NGX_Handle*);
    using PFN_Evaluate = NVSDK_NGX_Result (NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);

    struct RingSlot {
        ID3D12CommandAllocator* allocator = nullptr;
        UINT64 fenceValue = 0;
    };

    std::mutex m_mutex;
    const Config& m_config; // metalstream.ini, see config.cpp
    double m_minFrameIntervalMs = 0.0;
    DWORD m_refreshHz = 60;
    std::atomic<int64_t> m_lastCapTicks{0};
    Presenter m_presenter;
    UINT m_lastSync = UINT_MAX;
    UINT m_lastFlags = UINT_MAX;
    int m_presentParamLogs = 0;
    int64_t m_lastEntryTicks = 0;
    double m_frameIntervalMs = 0.0;
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_threadedPresents{false};
    std::atomic<uint32_t> m_inlineShown{2};

    IDXGISwapChain* m_swapChain = nullptr; // not owned
    ID3D12CommandQueue* m_queue = nullptr;
    ID3D12Device* m_device = nullptr;
    ID3D12Fence* m_fence = nullptr;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    RingSlot m_ring[kRingSize];
    int m_ringIndex = 0;
    ID3D12GraphicsCommandList* m_list = nullptr;

    // Generated frame + copy of the real frame per in-flight game frame (the present thread reads them)
    static constexpr int kOutputSlots = 3;
    struct OutputSlot {
        ID3D12Resource* interp = nullptr;   // UNORDERED_ACCESS
        ID3D12Resource* real = nullptr;     // COPY_SOURCE
        uint64_t jobId = 0;
    };
    OutputSlot m_slots[kOutputSlots];
    int m_slotIndex = 0;
    ID3D12Resource* m_realOut = nullptr;
    // Proxy backbuffers handed to the game (see Present)
    IDXGISwapChain* m_proxyFor = nullptr;
    ID3D12Resource* m_fake[DXGI_MAX_SWAP_CHAIN_BUFFERS] = {};
    UINT m_fakeCount = 0;
    UINT m_fakeIndex = 0;
    uint64_t m_epoch = 0; // bumped whenever proxy buffers or size-dependent objects are replaced
    uint64_t m_lastTrackedPresent = 0; // m_presentCount of the tracked swapchain's last Present
    std::vector<OrphanProxy> m_orphans;
    UINT m_width = 0;
    UINT m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    std::unordered_map<std::pair<sl::BufferType, ID3D12Resource*>, PreservedCopy, PreservedKeyHash> m_preserved;
    std::vector<Retired> m_retired;
    uint64_t m_presentCount = 0;

    PFN_Allocate m_pfnAllocate = nullptr;
    PFN_Destroy m_pfnDestroy = nullptr;
    PFN_Create m_pfnCreate = nullptr;
    PFN_Release m_pfnRelease = nullptr;
    PFN_Evaluate m_pfnEvaluate = nullptr;
    NVSDK_NGX_Parameter* m_params = nullptr;
    NVSDK_NGX_Handle* m_feature = nullptr;
    bool m_ngxFailed = false;
    bool m_havePrevFrame = false;
    bool m_loggedFirstEval = false;
    bool m_warnedUntracked = false;
    uint32_t m_evalFailures = 0;
    sl::float4x4 m_viewToClip{};
    float m_lastMvecScaleX = 0.0f;
    std::vector<CaptureItem> m_captureItems;
    unsigned m_captureRemaining = 0;
    unsigned m_captureBurst = 0;
    bool m_captureKeyDown = false;
    unsigned m_captureFrameCounter = 0;
    float m_lastMvecScaleY = 0.0f;
    Stats m_stats;
};

// ---------------------------------------------------------------------------
// Hook entry points
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    return FrameGenerator::Get().Present(sc, sync, flags, nullptr, false);
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    return FrameGenerator::Get().Present(sc, sync, flags, params, true);
}

HRESULT STDMETHODCALLTYPE Hook_GetBuffer(IDXGISwapChain* sc, UINT index, REFIID riid, void** out) {
    HRESULT hr = S_OK;
    if (t_hookDepth == 0 && FrameGenerator::Get().GetProxyBuffer(sc, index, riid, out, &hr)) return hr;
    return g_origGetBuffer(sc, index, riid, out);
}

UINT STDMETHODCALLTYPE Hook_GetCurrentBackBufferIndex(IDXGISwapChain3* sc) {
    UINT index = 0;
    if (t_hookDepth == 0 && FrameGenerator::Get().GetProxyIndex(sc, &index)) return index;
    return g_origGetCurrentBackBufferIndex(sc);
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    FrameGenerator::Get().BeforeResize(sc);
    ++t_hookDepth; // anything the original does through the vtable must see the real buffers
    HRESULT hr = g_origResizeBuffers(sc, count, w, h, fmt, flags);
    --t_hookDepth;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                              const UINT* nodeMasks, IUnknown* const* queues) {
    FrameGenerator::Get().BeforeResize(sc);
    ++t_hookDepth;
    HRESULT hr = g_origResizeBuffers1(sc, count, w, h, fmt, flags, nodeMasks, queues);
    --t_hookDepth;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    FrameGenerator::Get().BeforeCreateSwapChain();
    HRESULT hr = g_origCreateSwapChain(factory, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) RegisterSwapChain(*out, device, true);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc, IDXGIOutput* output, IDXGISwapChain1** out) {
    FrameGenerator::Get().BeforeCreateSwapChain();
    HRESULT hr = g_origCreateSwapChainForHwnd(factory, device, hwnd, desc, fsDesc, output, out);
    if (SUCCEEDED(hr) && out && *out) RegisterSwapChain(*out, device, true);
    return hr;
}

} // namespace

void InstallFactoryHooks(IUnknown* factory) {
    if (!factory) return;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    IDXGIFactory* f = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f))) && f) {
        PatchSlot(f, kSlotFactoryCreateSwapChain, &Hook_CreateSwapChain, g_origCreateSwapChain, "IDXGIFactory::CreateSwapChain");
        f->Release();
    }
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f2))) && f2) {
        PatchSlot(f2, kSlotFactoryCreateSwapChainForHwnd, &Hook_CreateSwapChainForHwnd, g_origCreateSwapChainForHwnd,
                  "IDXGIFactory2::CreateSwapChainForHwnd");
        f2->Release();
    }
}

void RegisterSwapChain(IUnknown* swapChain, IUnknown* queueOrDevice, bool createdHere) {
    if (!swapChain) return;
    IDXGISwapChain* sc = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&sc))) || !sc) return;

    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        PatchSlot(sc, kSlotPresent, &Hook_Present, g_origPresent, "IDXGISwapChain::Present");
        PatchSlot(sc, kSlotResizeBuffers, &Hook_ResizeBuffers, g_origResizeBuffers, "IDXGISwapChain::ResizeBuffers");
        PatchSlot(sc, kSlotGetBuffer, &Hook_GetBuffer, g_origGetBuffer, "IDXGISwapChain::GetBuffer");
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
            PatchSlot(sc1, kSlotPresent1, &Hook_Present1, g_origPresent1, "IDXGISwapChain1::Present1");
            sc1->Release();
        }
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
            PatchSlot(sc3, kSlotResizeBuffers1, &Hook_ResizeBuffers1, g_origResizeBuffers1, "IDXGISwapChain3::ResizeBuffers1");
            PatchSlot(sc3, kSlotGetCurrentBackBufferIndex, &Hook_GetCurrentBackBufferIndex, g_origGetCurrentBackBufferIndex,
                      "IDXGISwapChain3::GetCurrentBackBufferIndex");
            sc3->Release();
        }
    }

    IDXGISwapChain1* desc1Source = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&desc1Source))) && desc1Source) {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        if (SUCCEEDED(desc1Source->GetDesc1(&d))) {
            LOG_INFO("framegen: swapchain %p desc: %ux%u fmt=%d buffers=%u swapEffect=%d flags=0x%X (waitable=%d, tearing=%d)",
                     sc, d.Width, d.Height, (int)d.Format, d.BufferCount, (int)d.SwapEffect, d.Flags,
                     (int)((d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0),
                     (int)((d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0));
        }
        desc1Source->Release();
    }

    ID3D12CommandQueue* queue = nullptr;
    if (queueOrDevice) queueOrDevice->QueryInterface(IID_PPV_ARGS(&queue));
    if (!queue) sc->GetDevice(IID_PPV_ARGS(&queue)); // D3D12 swapchains report their queue as the "device"
    if (queue) {
        FrameGenerator::Get().RegisterQueue(sc, queue, createdHere);
        // Only a swapchain we saw being created can be proxied: the game has not fetched its buffers yet
        if (createdHere && g_origGetBuffer && g_origGetCurrentBackBufferIndex) FrameGenerator::Get().EnableProxy(sc);
        queue->Release();
    } else {
        LOG_WARN("framegen: swapchain %p has no D3D12 command queue; frame generation unavailable for it", sc);
    }
    sc->Release();
}

void InstallGlobalHooks() {
    static std::once_flag once;
    std::call_once(once, [] {
        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); // our proxy export -> system dxgi.dll
        if (SUCCEEDED(hr) && factory) {
            factory->Release();
        } else {
            LOG_WARN("framegen: could not create a DXGI factory to install hooks (hr=0x%08X)", (uint32_t)hr);
        }
    });
}

ID3D12Resource* PreserveVolatileTag(ID3D12GraphicsCommandList* cmdList, sl::BufferType type, ID3D12Resource* resource,
                                    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_STATES* outState) {
    return FrameGenerator::Get().Preserve(cmdList, type, resource, state, outState);
}

bool IsActive() {
    return FrameGenerator::Get().IsActive();
}

uint32_t FramesPresentedPerGameFrame() {
    return FrameGenerator::Get().FramesPresentedPerGameFrame();
}

bool IsPreservedCopyAlive(ID3D12Resource* resource) {
    return FrameGenerator::Get().IsPreservedCopyAlive(resource);
}

void Shutdown() {
    FrameGenerator::Get().Shutdown();
}

} // namespace metalstream::framegen
