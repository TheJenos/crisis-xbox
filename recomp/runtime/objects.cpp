// Threads, handles and dispatcher objects (events, semaphores, mutants, timers).
//
// Objects live in guest memory as their real kernel structures, all starting with
// a DISPATCHER_HEADER: { u8 type, u8 absolute, u8 size, u8 inserted, s32 signalState,
// LIST_ENTRY waitList }. The signal state in guest memory is the source of truth;
// one host lock + condition variable guards every wait and signal.
#include "guest.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "platform.h"

namespace
{
    enum : uint8_t
    {
        TYPE_NOTIFICATION_EVENT = 0,
        TYPE_SYNCHRONIZATION_EVENT = 1,
        TYPE_MUTANT = 2,
        TYPE_SEMAPHORE = 5,
        TYPE_THREAD = 6,
        TYPE_NOTIFICATION_TIMER = 8,
        TYPE_SYNCHRONIZATION_TIMER = 9,
    };

    std::mutex g_dispatchMutex;
    std::condition_variable g_dispatchCv; // thread resume only; waits use per-thread waiters below

    // Threads blocked in WaitForObjects, by each object they wait on, so a
    // signal wakes only that object's waiters. (One shared condition variable
    // woke every blocked thread on every signal: ~60k wake-ups a second.)
    // Guarded by g_dispatchMutex.
    struct Waiter { std::condition_variable cv; };
    std::unordered_multimap<uint32_t, Waiter*> g_waiters;

    void WakeLocked(uint32_t obj)
    {
        auto [a, b] = g_waiters.equal_range(obj);
        for (; a != b; ++a)
            a->second->cv.notify_one();
    }
    void Wake(uint32_t obj)
    {
        std::lock_guard lock(g_dispatchMutex);
        WakeLocked(obj);
    }
    void WakeAll()
    {
        std::lock_guard lock(g_dispatchMutex);
        for (auto& [obj, w] : g_waiters)
            w->cv.notify_one();
    }

    uint8_t ObjType(uint32_t obj) { return Load<uint8_t>(obj); }
    int32_t ObjState(uint32_t obj) { return Load<int32_t>(obj + 4); }
    void SetObjState(uint32_t obj, int32_t s) { Store<int32_t>(obj + 4, s); }

    void InitHeader(uint32_t obj, uint8_t type, uint8_t sizeDwords, int32_t state)
    {
        Store<uint8_t>(obj + 0, type);
        Store<uint8_t>(obj + 1, 0);
        Store<uint8_t>(obj + 2, sizeDwords);
        Store<uint8_t>(obj + 3, 0);
        SetObjState(obj, state);
        Store<uint32_t>(obj + 8, obj + 8); // empty wait list points to itself
        Store<uint32_t>(obj + 12, obj + 8);
    }

    // KMUTANT: header, LIST_ENTRY mutantList @0x10, owner @0x18, abandoned @0x1C
    constexpr uint32_t MUTANT_OWNER = 0x18;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
struct GuestThread
{
    uint32_t handle = 0, id = 0;
    uint32_t kthread = 0, pcr = 0;
    uint32_t stackBase = 0, stackLimit = 0;
    uint32_t tlsBlock = 0;
    uint32_t startup = 0, entry = 0, arg = 0;
    PPCContext* ctx = nullptr;
    int suspendCount = 0;
    uint32_t tlsSlots[64] = {}; // KeTlsGetValue/SetValue
    // Diagnostics for DumpThreads()
    char name[64] = {};
    std::atomic<uint32_t> waitCount{ 0 }, waitObject{ 0 }, waitCs{ 0 };
    std::atomic<bool> sleeping{ false }; // in KeDelayExecutionThread (for PROFILE)
    // KeDelayExecutionThread statistics (for PROFILE): non-zero sleeps, their
    // requested and actual total time, the longest one, and zero-time yields.
    std::atomic<uint64_t> sleepCount{ 0 }, sleepReqNs{ 0 }, sleepActNs{ 0 }, sleepMaxNs{ 0 }, yields{ 0 };
};

// PROFILE=1 counters (relaxed; only read for the periodic report).
namespace prof
{
    std::atomic<uint64_t> csEnter{ 0 }, csContended{ 0 }, csWakeups{ 0 }, waits{ 0 }, waitWakeups{ 0 }, signals{ 0 };
    // Host-side costs, filled in by gpu.cpp / gpu_draw.cpp / kernel_misc.cpp when PROFILE is set.
    std::atomic<uint64_t> cpBusyNs{ 0 }, cpWaitNs{ 0 }, submitWaitNs{ 0 }, timebaseReads{ 0 };
    std::atomic<uint64_t> cpPhaseNs[CP_PHASES]{}, cpCount[CPC_COUNTS]{};
    // Read at first use (after startup): the browser's environment isn't set up during static init.
    bool Enabled() { static const bool e = getenv("PROFILE") != nullptr; return e; }
}

namespace
{
    // PROFILE: zero-time yields by (thread id, return address, two callers).
    std::mutex g_yieldSitesMutex;
    std::map<std::array<uint32_t, 4>, uint64_t> g_yieldSites;
}

namespace
{
    thread_local GuestThread* t_thread = nullptr;
    std::atomic<uint32_t> g_nextThreadId{ 1 };
    std::mutex g_threadsMutex;
    std::condition_variable g_resumeCv;
    std::unordered_map<uint32_t, GuestThread*> g_threadsByKThread;

    struct ThreadExit { uint32_t code; };

    // Host stack for threads running recompiled code. Native threads only
    // reserve it; WebAssembly stacks live in linear memory, so they are smaller.
#ifdef __EMSCRIPTEN__
    constexpr size_t kGuestHostStack = 8u << 20;
#else
    constexpr size_t kGuestHostStack = 64u << 20;
#endif

    // KTHREAD fields the runtime fills in (offsets from Xenia's X_KTHREAD).
    constexpr uint32_t KTHREAD_SIZE = 0xAB0;
    constexpr uint32_t KTHREAD_STACK_BASE = 0x5C, KTHREAD_STACK_LIMIT = 0x60, KTHREAD_TLS = 0x68,
                       KTHREAD_ID = 0x14C, KTHREAD_EXIT_STATUS = 0x150;
    // KPCR fields, addressed through r13.
    constexpr uint32_t PCR_SIZE = 0x2D8;
    constexpr uint32_t PCR_TLS = 0x0, PCR_SELF = 0x30, PCR_STACK_BASE = 0x70, PCR_STACK_END = 0x74,
                       PCR_CURRENT_THREAD = 0x100, PCR_CURRENT_CPU = 0x10C;
}

GuestThread* CurrentThread() { return t_thread; }

bool g_importFirst = getenv("IMPORT_FIRST") != nullptr;

void ImportFirstSeen(const char* name, PPCContext& ctx)
{
    GuestThread* t = t_thread;
    if (!t)
        return;
    uint32_t callers[3] = {};
    uint32_t frame = ctx.r1.u32;
    for (int d = 0; d < 3; d++)
    {
        if (frame < t->stackLimit || frame + 4 > t->stackBase)
            break;
        uint32_t next = Load<uint32_t>(frame);
        if (next <= frame || next > t->stackBase)
            break;
        callers[d] = Load<uint32_t>(next - 8);
        frame = next;
    }
    uint64_t key = uint64_t(uintptr_t(name)) * 0x9E3779B97F4A7C15ull ^ t->entry;
    for (uint32_t v : { uint32_t(ctx.lr), callers[0], callers[1], callers[2] })
        key = (key ^ v) * 0x100000001B3ull;
    static std::mutex m;
    static std::unordered_set<uint64_t> seen;
    {
        std::lock_guard lock(m);
        if (!seen.insert(key).second)
            return;
    }
    Log("FIRST %s|%08X %s %08X %08X %08X %08X", t->name[0] ? t->name : "?", t->entry, name, uint32_t(ctx.lr),
        callers[0], callers[1], callers[2]);
}
uint32_t CurrentKThread() { return t_thread ? t_thread->kthread : 0; }
uint32_t CurrentThreadId() { return t_thread ? t_thread->id : 0; }

void GuestCall(PPCContext& ctx, uint32_t addr)
{
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, addr);
    fn(ctx, g_base);
}

static void SetupGuestThread(GuestThread* t, uint32_t stackSize)
{
    stackSize = std::max<uint32_t>((stackSize + 0xFFFF) & ~0xFFFFu, 0x40000);
    t->stackLimit = VirtualAlloc(stackSize);
    if (!t->stackLimit)
        Fatal("out of memory allocating a %X-byte guest stack", stackSize);
    t->stackBase = t->stackLimit + stackSize;

    t->kthread = KernelAlloc(KTHREAD_SIZE);
    InitHeader(t->kthread, TYPE_THREAD, KTHREAD_SIZE / 4, 0);
    Store<uint32_t>(t->kthread + KTHREAD_STACK_BASE, t->stackBase);
    Store<uint32_t>(t->kthread + KTHREAD_STACK_LIMIT, t->stackLimit);
    Store<uint32_t>(t->kthread + KTHREAD_ID, t->id);

    // Static TLS: the slot area followed by a copy of the TLS template; the PCR
    // points at the template copy.
    uint32_t slotBytes = g_xex.tlsSlots * 4;
    uint32_t dataSize = (std::max(g_xex.tlsDataSize, g_xex.tlsRawDataSize) + 15) & ~15u;
    t->tlsBlock = KernelAlloc(slotBytes + dataSize + 16);
    uint32_t tlsStatic = t->tlsBlock + slotBytes;
    if (g_xex.tlsRawData && g_xex.tlsRawDataSize)
        memcpy(GuestPtr(tlsStatic), GuestPtr(g_xex.tlsRawData), g_xex.tlsRawDataSize);
    Store<uint32_t>(t->kthread + KTHREAD_TLS, tlsStatic);

    t->pcr = KernelAlloc(PCR_SIZE);
    Store<uint32_t>(t->pcr + PCR_TLS, tlsStatic);
    Store<uint32_t>(t->pcr + PCR_SELF, t->pcr);
    Store<uint32_t>(t->pcr + PCR_STACK_BASE, t->stackBase);
    Store<uint32_t>(t->pcr + PCR_STACK_END, t->stackLimit);
    Store<uint32_t>(t->pcr + PCR_CURRENT_THREAD, t->kthread);
    Store<uint8_t>(t->pcr + PCR_CURRENT_CPU, uint8_t(t->id % 6));

    t->ctx = new PPCContext();
    t->ctx->r1.u64 = t->stackBase - 0x100;
    t->ctx->r13.u64 = t->pcr;
    t->ctx->fpscr.loadFromHost();

    std::lock_guard lock(g_threadsMutex);
    g_threadsByKThread[t->kthread] = t;
}

static void FinishThread(GuestThread* t, uint32_t exitCode)
{
    {
        std::lock_guard lock(g_dispatchMutex);
        Store<uint32_t>(t->kthread + KTHREAD_EXIT_STATUS, exitCode);
        SetObjState(t->kthread, 1);
        WakeLocked(t->kthread);
    }
}

static void HostThreadMain(GuestThread* t)
{
    t_thread = t;
    {
        std::unique_lock lock(g_threadsMutex);
        g_resumeCv.wait(lock, [&] { return t->suspendCount <= 0; });
    }

    uint32_t exitCode = 0;
    try
    {
        PPCContext& ctx = *t->ctx;
        if (t->startup)
        {
            ctx.r3.u64 = t->entry;
            ctx.r4.u64 = t->arg;
            GuestCall(ctx, t->startup);
        }
        else
        {
            ctx.r3.u64 = t->arg;
            GuestCall(ctx, t->entry);
        }
        exitCode = ctx.r3.u32;
    }
    catch (const ThreadExit& e)
    {
        exitCode = e.code;
    }
    Log("thread %u exited with %X", t->id, exitCode);
    FinishThread(t, exitCode);
}

uint32_t CreateGuestThread(uint32_t stackSize, uint32_t startup, uint32_t entry, uint32_t arg,
                           bool suspended, uint32_t* threadIdOut, bool isMain)
{
    auto* t = new GuestThread();
    t->id = g_nextThreadId++;
    t->startup = startup;
    t->entry = entry;
    t->arg = arg;
    t->suspendCount = suspended ? 1 : 0;
    SetupGuestThread(t, stackSize);
    t->handle = CreateHandle(ObjectKind::Thread, t->kthread);
    if (threadIdOut)
        *threadIdOut = t->id;

    // Recompiled code uses the host stack for its frames, so give it plenty.
    if (!PlatformStartThread([t] { HostThreadMain(t); }, kGuestHostStack))
        Fatal("could not start a host thread");
    Log("created thread %u: entry %08X startup %08X arg %08X%s", t->id, entry, startup, arg, suspended ? " (suspended)" : "");
    if (isMain)
    {
        uint32_t kthread = t->kthread; // signaled when the thread finishes
        WaitForObjects(&kthread, 1, false, 0, false);
    }
    return t->handle;
}

// PROFILE=1: sample every guest thread each millisecond. Every 10 s, log how
// busy each thread was (not waiting, sleeping or exited) and, for the busy
// ones, which recompiled functions their latest call sites (link register)
// fall in. Also logs dispatcher traffic (critical sections, waits, signals).
static void StartProfiler()
{
    if (!getenv("PROFILE"))
        return;
    PlatformStartThread([] {
        size_t nfuncs = 0;
        while (PPCFuncMappings[nfuncs].host) nfuncs++;
        auto funcOf = [&](uint32_t addr) -> uint32_t {
            size_t lo = 0, hi = nfuncs;
            while (hi - lo > 1) { size_t mid = (lo + hi) / 2; (PPCFuncMappings[mid].guest <= addr ? lo : hi) = mid; }
            return nfuncs && PPCFuncMappings[lo].guest <= addr ? uint32_t(PPCFuncMappings[lo].guest) : 0;
        };
        // at: samples by latest call site; incl: samples in which a function was
        // anywhere on the guest call stack (walked through the r1 back chain).
        struct Stat { uint64_t busy = 0, total = 0; std::unordered_map<uint32_t, uint32_t> at, incl; std::string name; GuestThread* thread = nullptr; };
        std::map<uint32_t, Stat> stats; // by thread id
        uint64_t start = PlatformCounterNs();
        uint64_t last[6] = {};
        for (;;)
        {
            PlatformSleepMs(1);
            {
                std::lock_guard lock(g_threadsMutex);
                for (auto& [k, t] : g_threadsByKThread)
                {
                    Stat& s = stats[t->id];
                    s.thread = t;
                    if (s.name.empty() && t->name[0]) s.name = t->name;
                    s.total++;
                    bool idle = t->waitCount || t->waitCs || t->sleeping || Load<int32_t>(t->kthread + 4) > 0;
                    if (!idle)
                    {
                        s.busy++;
                        if (t->ctx)
                        {
                            uint32_t lr = uint32_t(t->ctx->lr);
                            s.at[lr]++;
                            uint32_t funcs[20];
                            int n = 0;
                            funcs[n++] = funcOf(lr);
                            uint32_t frame = t->ctx->r1.u32;
                            for (int d = 0; d < 18 && frame >= t->stackLimit && frame + 4 <= t->stackBase; d++)
                            {
                                uint32_t next = Load<uint32_t>(frame);
                                if (next <= frame || next > t->stackBase)
                                    break;
                                uint32_t f = funcOf(Load<uint32_t>(next - 8));
                                if (std::find(funcs, funcs + n, f) == funcs + n)
                                    funcs[n++] = f;
                                frame = next;
                            }
                            for (int i = 0; i < n; i++)
                                s.incl[funcs[i]]++;
                        }
                    }
                }
            }
            uint64_t now = PlatformCounterNs();
            if (now - start < 10000000000ull)
                continue;
            double secs = double(now - start) / 1e9;
            uint64_t cur[6] = { prof::csEnter, prof::csContended, prof::csWakeups, prof::waits, prof::waitWakeups, prof::signals };
            Log("PROFILE dispatcher /s: cs enter %.0f contended %.0f wakeups %.0f; waits %.0f wakeups %.0f; signals %.0f",
                (cur[0] - last[0]) / secs, (cur[1] - last[1]) / secs, (cur[2] - last[2]) / secs, (cur[3] - last[3]) / secs,
                (cur[4] - last[4]) / secs, (cur[5] - last[5]) / secs);
            memcpy(last, cur, sizeof(last));
            static uint64_t lastCp = 0, lastCpWait = 0, lastSubmitWait = 0, lastTb = 0;
            uint64_t cp = prof::cpBusyNs, cw = prof::cpWaitNs, sw = prof::submitWaitNs, tb = prof::timebaseReads;
            double span = double(now - start);
            Log("PROFILE host: GPU command processor busy %.0f%% (waiting on memory %.0f%%, frame submit waiting %.0f%%); timebase reads %.0f/s",
                100.0 * double(cp - lastCp) / span, 100.0 * double(cw - lastCpWait) / span, 100.0 * double(sw - lastSubmitWait) / span,
                double(tb - lastTb) / secs);
            lastCp = cp; lastCpWait = cw; lastSubmitWait = sw; lastTb = tb;
            {
                // Where the command processor's time goes (gpu_draw.cpp phases).
                static const char* kCpPhaseNames[prof::CP_PHASES] = { "indices", "state", "vertex", "constants", "textures",
                                                                       "record", "resolves", "frame end" };
                static uint64_t lastPhase[prof::CP_PHASES] = {}, lastCount[prof::CPC_COUNTS] = {};
                std::string line;
                char buf[64];
                for (int i = 0; i < prof::CP_PHASES; i++)
                {
                    uint64_t v = prof::cpPhaseNs[i];
                    snprintf(buf, sizeof(buf), " %s %.1f%%", kCpPhaseNames[i], 100.0 * double(v - lastPhase[i]) / span);
                    line += buf;
                    lastPhase[i] = v;
                }
                uint64_t c[prof::CPC_COUNTS];
                for (int i = 0; i < prof::CPC_COUNTS; i++) { c[i] = prof::cpCount[i] - lastCount[i]; lastCount[i] = prof::cpCount[i]; }
                Log("PROFILE host CP phases:%s", line.c_str());
                Log("PROFILE host CP work /s: textures hashed %.0f MB, decoded %.1f MB; vertex copied %.0f MB, checked %.0f MB; "
                    "constant blocks %.0f; indices %.0fk",
                    c[prof::CPC_TEX_HASH_BYTES] / secs / 1e6, c[prof::CPC_TEX_DECODE_BYTES] / secs / 1e6,
                    c[prof::CPC_VTX_COPY_BYTES] / secs / 1e6, c[prof::CPC_VTX_CHECK_BYTES] / secs / 1e6,
                    c[prof::CPC_CONST_UPLOADS] / secs, c[prof::CPC_INDICES] / secs / 1e3);
            }
            for (auto& [id, s] : stats)
            {
                if (!s.total) continue;
                double busy = 100.0 * s.busy / s.total;
                if (busy >= 3.0)
                {
                    std::unordered_map<uint32_t, uint32_t> byFunc;
                    for (auto& [lr, n] : s.at) byFunc[funcOf(lr)] += n;
                    std::vector<std::pair<uint32_t, uint32_t>> top;
                    for (auto& [f, n] : byFunc) top.push_back({ n, f });
                    std::sort(top.rbegin(), top.rend());
                    std::string line;
                    char buf[48];
                    for (size_t i = 0; i < top.size() && i < 12; i++)
                    {
                        snprintf(buf, sizeof(buf), " %08X:%.0f%%", top[i].second, 100.0 * top[i].first / s.busy);
                        line += buf;
                    }
                    Log("PROFILE t%u %s busy %.0f%%:%s", id, s.name.c_str(), busy, line.c_str());
                    if (busy >= 30.0 && !s.incl.empty())
                    {
                        top.clear();
                        for (auto& [f, n] : s.incl) top.push_back({ n, f });
                        std::sort(top.rbegin(), top.rend());
                        line.clear();
                        for (size_t i = 0; i < top.size() && i < 24; i++)
                        {
                            snprintf(buf, sizeof(buf), " %08X:%.0f%%", top[i].second, 100.0 * top[i].first / s.busy);
                            line += buf;
                        }
                        Log("PROFILE t%u inclusive:%s", id, line.c_str());
                    }
                }
                if (GuestThread* t = s.thread)
                {
                    uint64_t n = t->sleepCount.exchange(0), req = t->sleepReqNs.exchange(0), act = t->sleepActNs.exchange(0);
                    uint64_t mx = t->sleepMaxNs.exchange(0), y = t->yields.exchange(0);
                    if (act > uint64_t(span) / 20 || y > 10000)
                        Log("PROFILE t%u %s sleeps: %llu calls, requested %.0f ms, slept %.0f ms (longest %.1f ms); %llu zero-time yields",
                            id, s.name.c_str(), (unsigned long long)n, req / 1e6, act / 1e6, mx / 1e6, (unsigned long long)y);
                }
                s.busy = s.total = 0;
                s.at.clear();
                s.incl.clear();
            }
            {
                // Where zero-time yields come from (spin-waits): thread, return
                // address and two callers, busiest first.
                std::vector<std::pair<uint64_t, std::array<uint32_t, 4>>> top;
                {
                    std::lock_guard lock(g_yieldSitesMutex);
                    for (auto& [k, n] : g_yieldSites) top.push_back({ n, k });
                    g_yieldSites.clear();
                }
                std::sort(top.rbegin(), top.rend());
                for (size_t i = 0; i < top.size() && i < 6; i++)
                {
                    const auto& k = top[i].second;
                    Log("PROFILE yields t%u: %.0f/s at %08X (in %08X) <- %08X (in %08X) <- %08X (in %08X)", k[0],
                        top[i].first / secs, k[1], funcOf(k[1]), k[2], funcOf(k[2]), k[3], funcOf(k[3]));
                }
            }
            start = now;
        }
    }, 1u << 20);
}

void RunMainThread(uint32_t entry)
{
    StartProfiler();
    CreateGuestThread(g_xex.stackSize, 0, entry, 0, false, nullptr, true);
}

static GuestThread* ThreadFromHandle(uint32_t handle)
{
    if (handle == 0xFFFFFFFE)
        return t_thread;
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(handle, &kind, &obj) || kind != ObjectKind::Thread)
        return nullptr;
    std::lock_guard lock(g_threadsMutex);
    auto it = g_threadsByKThread.find(obj);
    return it == g_threadsByKThread.end() ? nullptr : it->second;
}

// NTSTATUS ExCreateThread(u32* handle, u32 stackSize, u32* threadId, u32 xapiStartup,
//                         u32 startAddress, u32 startContext, u32 creationFlags)
GUEST_IMPORT(ExCreateThread)
{
    uint32_t handlePtr = ctx.r3.u32, stackSize = ctx.r4.u32, idPtr = ctx.r5.u32;
    uint32_t startup = ctx.r6.u32, entry = ctx.r7.u32, arg = ctx.r8.u32, flags = ctx.r9.u32;
    uint32_t id = 0;
    uint32_t handle = CreateGuestThread(stackSize ? stackSize : g_xex.stackSize, startup, entry, arg, flags & 1, &id);
    if (handlePtr) Store<uint32_t>(handlePtr, handle);
    if (idPtr) Store<uint32_t>(idPtr, id);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(ExTerminateThread) { throw ThreadExit{ ctx.r3.u32 }; }

// NTSTATUS NtResumeThread(u32 handle, u32* previousSuspendCount)
GUEST_IMPORT(NtResumeThread)
{
    GuestThread* t = ThreadFromHandle(ctx.r3.u32);
    if (!t) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int prev;
    {
        std::lock_guard lock(g_threadsMutex);
        prev = t->suspendCount;
        if (t->suspendCount > 0) t->suspendCount--;
    }
    g_resumeCv.notify_all();
    if (ctx.r4.u32) Store<uint32_t>(ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// Only threads that haven't started yet can be suspended in this runtime.
GUEST_IMPORT(NtSuspendThread)
{
    GuestThread* t = ThreadFromHandle(ctx.r3.u32);
    if (!t) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int prev;
    {
        std::lock_guard lock(g_threadsMutex);
        prev = t->suspendCount++;
    }
    if (t != t_thread)
        Log("NtSuspendThread on running thread %u is not supported", t->id);
    if (ctx.r4.u32) Store<uint32_t>(ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS KeDelayExecutionThread(u32 mode, u32 alertable, i64* interval)
GUEST_IMPORT(KeDelayExecutionThread)
{
    int64_t interval = Load<int64_t>(ctx.r5.u32);
    GuestThread* t = t_thread;
    if (interval == 0)
    {
        PlatformYield();
        if (t) t->yields.fetch_add(1, std::memory_order_relaxed);
        if (t && prof::Enabled())
        {
            // Remember the call chain for the PROFILE report (spin-wait sites).
            std::array<uint32_t, 4> key{ t->id, uint32_t(ctx.lr), 0, 0 };
            uint32_t frame = ctx.r1.u32;
            for (int k = 2; k < 4; k++)
            {
                if (frame < t->stackLimit || frame + 4 > t->stackBase)
                    break;
                uint32_t next = Load<uint32_t>(frame);
                if (next <= frame || next > t->stackBase)
                    break;
                key[k] = Load<uint32_t>(next - 8);
                frame = next;
            }
            std::lock_guard lock(g_yieldSitesMutex);
            g_yieldSites[key]++;
        }
    }
    else
    {
        uint32_t ms = uint32_t(std::min<int64_t>(interval < 0 ? -interval / 10000 : 0, 0x7FFFFFFF));
        uint64_t start = PlatformCounterNs();
        if (t) t->sleeping = true;
        PlatformSleepMsPrecise(ms);
        if (t)
        {
            t->sleeping = false;
            uint64_t took = PlatformCounterNs() - start;
            t->sleepCount.fetch_add(1, std::memory_order_relaxed);
            t->sleepReqNs.fetch_add(uint64_t(ms) * 1000000ull, std::memory_order_relaxed);
            t->sleepActNs.fetch_add(took, std::memory_order_relaxed);
            if (took > t->sleepMaxNs.load(std::memory_order_relaxed))
                t->sleepMaxNs.store(took, std::memory_order_relaxed);
        }
    }
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(KeSetAffinityThread) { if (ctx.r5.u32) Store<uint32_t>(ctx.r5.u32, 1u << (CurrentThreadId() % 6)); ctx.r3.u64 = STATUS_SUCCESS; }
GUEST_IMPORT(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KeQueryBasePriorityThread) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KeGetCurrentProcessType) { ctx.r3.u64 = 1; } // title process
GUEST_IMPORT(KeSetCurrentProcessType) {}
GUEST_IMPORT(KeEnterCriticalRegion) {}
GUEST_IMPORT(KeLeaveCriticalRegion) {}

// ---------------------------------------------------------------------------
// TLS slots (KeTlsAlloc etc.), stored host-side per thread
// ---------------------------------------------------------------------------
namespace { std::mutex g_tlsMutex; uint64_t g_tlsUsed = 0; }

GUEST_IMPORT(KeTlsAlloc)
{
    std::lock_guard lock(g_tlsMutex);
    for (uint32_t i = 0; i < 64; i++)
        if (!(g_tlsUsed & (1ull << i)))
        {
            g_tlsUsed |= 1ull << i;
            ctx.r3.u64 = i;
            return;
        }
    ctx.r3.u64 = 0xFFFFFFFF;
}
GUEST_IMPORT(KeTlsFree)
{
    std::lock_guard lock(g_tlsMutex);
    if (ctx.r3.u32 < 64) g_tlsUsed &= ~(1ull << ctx.r3.u32);
    ctx.r3.u64 = 1;
}
GUEST_IMPORT(KeTlsGetValue) { ctx.r3.u64 = ctx.r3.u32 < 64 ? t_thread->tlsSlots[ctx.r3.u32] : 0; }
GUEST_IMPORT(KeTlsSetValue)
{
    if (ctx.r3.u32 < 64) t_thread->tlsSlots[ctx.r3.u32] = ctx.r4.u32;
    ctx.r3.u64 = ctx.r3.u32 < 64;
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------
namespace
{
    struct HandleEntry { ObjectKind kind; uint32_t object; };
    std::mutex g_handleMutex;
    std::unordered_map<uint32_t, HandleEntry> g_handles;
    uint32_t g_nextHandle = 0xF8000004;
    std::map<std::string, uint32_t> g_namedObjects; // name -> object
}

uint32_t CreateHandle(ObjectKind kind, uint32_t object)
{
    std::lock_guard lock(g_handleMutex);
    uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = { kind, object };
    return h;
}

bool LookupHandle(uint32_t handle, ObjectKind* kind, uint32_t* object)
{
    if (handle == 0xFFFFFFFE && t_thread)
    {
        *kind = ObjectKind::Thread;
        *object = t_thread->kthread;
        return true;
    }
    std::lock_guard lock(g_handleMutex);
    auto it = g_handles.find(handle);
    if (it == g_handles.end())
        return false;
    *kind = it->second.kind;
    *object = it->second.object;
    return true;
}

bool CloseHandle(uint32_t handle)
{
    bool lastFileHandle = false;
    uint32_t object = 0;
    {
        std::lock_guard lock(g_handleMutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end())
            return false;
        HandleEntry e = it->second;
        g_handles.erase(it);
        if (e.kind == ObjectKind::File)
        {
            object = e.object;
            lastFileHandle = std::none_of(g_handles.begin(), g_handles.end(), [&](const auto& h) {
                return h.second.kind == ObjectKind::File && h.second.object == e.object;
            });
        }
    }
    if (lastFileHandle)
        FsFileClosed(object);
    return true;
}

GUEST_IMPORT(NtClose) { ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE; }

// NTSTATUS NtDuplicateObject(u32 handle, u32* newHandle, u32 options)
GUEST_IMPORT(NtDuplicateObject)
{
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(ctx.r3.u32, &kind, &obj)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r4.u32) Store<uint32_t>(ctx.r4.u32, CreateHandle(kind, obj));
    if (ctx.r5.u32 & 1) CloseHandle(ctx.r3.u32); // DUPLICATE_CLOSE_SOURCE
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS ObReferenceObjectByHandle(u32 handle, u32 objectType, u32* object)
GUEST_IMPORT(ObReferenceObjectByHandle)
{
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(ctx.r3.u32, &kind, &obj)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (ctx.r5.u32) Store<uint32_t>(ctx.r5.u32, obj);
    ctx.r3.u64 = STATUS_SUCCESS;
}
GUEST_IMPORT(ObReferenceObject) { ctx.r3.u64 = 0; }
GUEST_IMPORT(ObDereferenceObject) {}
GUEST_IMPORT(ObIsTitleObject) { ctx.r3.u64 = 1; }

// Name from OBJECT_ATTRIBUTES { u32 rootDirectory, u32 objectName (ANSI_STRING*), u32 attributes }
static std::string ObjectName(uint32_t attrs)
{
    if (!attrs) return {};
    uint32_t name = Load<uint32_t>(attrs + 4);
    return name ? LoadAnsiString(name) : std::string();
}

// Create a named or anonymous object; returns the existing object for a known name.
static uint32_t CreateObject(uint32_t attrs, uint32_t size, uint8_t type, int32_t state, bool* existed)
{
    std::string name = ObjectName(attrs);
    std::lock_guard lock(g_handleMutex);
    if (!name.empty())
    {
        auto it = g_namedObjects.find(name);
        if (it != g_namedObjects.end())
        {
            *existed = true;
            return it->second;
        }
    }
    *existed = false;
    uint32_t obj = KernelAlloc(size);
    InitHeader(obj, type, uint8_t(size / 4), state);
    if (!name.empty())
        g_namedObjects[name] = obj;
    return obj;
}

// ---------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------
static bool IsSignaled(uint32_t obj, uint32_t kthread)
{
    uint8_t type = ObjType(obj);
    if (type == TYPE_MUTANT)
        return ObjState(obj) > 0 || Load<uint32_t>(obj + MUTANT_OWNER) == kthread;
    return ObjState(obj) > 0;
}

// Apply the side effects of a satisfied wait.
static void Acquire(uint32_t obj, uint32_t kthread)
{
    switch (ObjType(obj))
    {
    case TYPE_SYNCHRONIZATION_EVENT:
    case TYPE_SYNCHRONIZATION_TIMER:
        SetObjState(obj, 0);
        break;
    case TYPE_SEMAPHORE:
        SetObjState(obj, ObjState(obj) - 1);
        break;
    case TYPE_MUTANT:
        SetObjState(obj, ObjState(obj) - 1);
        Store<uint32_t>(obj + MUTANT_OWNER, kthread);
        break;
    default:
        break;
    }
}

static int64_t NowFileTime() { return PlatformFileTimeNow(); }

uint32_t WaitForObjects(const uint32_t* objects, uint32_t count, bool waitAll, uint32_t timeoutPtr, bool alertable)
{
    using clock = std::chrono::steady_clock;
    bool infinite = timeoutPtr == 0;
    clock::time_point deadline = clock::now();
    if (!infinite)
    {
        int64_t t = Load<int64_t>(timeoutPtr);
        int64_t rel100ns = t < 0 ? -t : std::max<int64_t>(0, t - NowFileTime());
        deadline += std::chrono::microseconds(rel100ns / 10);
    }

    uint32_t me = CurrentKThread();
    struct WaitMark
    {
        GuestThread* t;
        WaitMark(GuestThread* t, uint32_t count, uint32_t obj) : t(t) { if (t) { t->waitObject = obj; t->waitCount = count; } }
        ~WaitMark() { if (t) t->waitCount = 0; }
    } mark(t_thread, count, count ? objects[0] : 0);
    prof::waits.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(g_dispatchMutex);
    // Registered on first block, removed on return (still under the lock).
    thread_local Waiter waiter;
    struct Registration
    {
        const uint32_t* objects; uint32_t count; bool active = false;
        void Add() { for (uint32_t i = 0; i < count; i++) g_waiters.emplace(objects[i], &waiter); active = true; }
        ~Registration()
        {
            if (!active) return;
            for (uint32_t i = 0; i < count; i++)
            {
                auto [a, b] = g_waiters.equal_range(objects[i]);
                for (; a != b; ++a)
                    if (a->second == &waiter) { g_waiters.erase(a); break; }
            }
        }
    } registration{ objects, count };
    for (bool first = true;; first = false)
    {
        if (!first) prof::waitWakeups.fetch_add(1, std::memory_order_relaxed);
        if (waitAll)
        {
            bool all = true;
            for (uint32_t i = 0; i < count && all; i++)
                all = IsSignaled(objects[i], me);
            if (all)
            {
                for (uint32_t i = 0; i < count; i++)
                    Acquire(objects[i], me);
                return STATUS_SUCCESS;
            }
        }
        else
        {
            for (uint32_t i = 0; i < count; i++)
                if (IsSignaled(objects[i], me))
                {
                    Acquire(objects[i], me);
                    return STATUS_SUCCESS + i;
                }
        }

        if (!infinite && clock::now() >= deadline)
            return STATUS_TIMEOUT;
        if (!registration.active)
            registration.Add();
        // Guest code can change signal states without a kernel call, so re-check periodically.
        auto wake = clock::now() + std::chrono::milliseconds(5);
        waiter.cv.wait_until(lock, infinite ? wake : std::min(wake, deadline));
    }
}

void SignalObjectsChanged() { prof::signals.fetch_add(1, std::memory_order_relaxed); WakeAll(); }

// NTSTATUS KeWaitForSingleObject(u32 object, u32 reason, u32 mode, u32 alertable, i64* timeout)
GUEST_IMPORT(KeWaitForSingleObject)
{
    uint32_t obj = ctx.r3.u32;
    ctx.r3.u64 = WaitForObjects(&obj, 1, false, ctx.r7.u32, ctx.r6.u32);
}

// NTSTATUS KeWaitForMultipleObjects(u32 count, u32* objects, u32 waitType, u32 reason, u32 mode,
//                                   u32 alertable, i64* timeout, u32 waitBlocks)
GUEST_IMPORT(KeWaitForMultipleObjects)
{
    uint32_t count = ctx.r3.u32, arr = ctx.r4.u32;
    std::vector<uint32_t> objs(count);
    for (uint32_t i = 0; i < count; i++) objs[i] = Load<uint32_t>(arr + i * 4);
    ctx.r3.u64 = WaitForObjects(objs.data(), count, ctx.r5.u32 == 0, ctx.r9.u32, ctx.r8.u32);
}

static bool HandlesToObjects(uint32_t arr, uint32_t count, std::vector<uint32_t>& out)
{
    out.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        ObjectKind kind;
        if (!LookupHandle(Load<uint32_t>(arr + i * 4), &kind, &out[i]))
            return false;
    }
    return true;
}

// NTSTATUS NtWaitForSingleObjectEx(u32 handle, u32 mode, u32 alertable, i64* timeout)
GUEST_IMPORT(NtWaitForSingleObjectEx)
{
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(ctx.r3.u32, &kind, &obj)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t handle = ctx.r3.u32, timeout = ctx.r6.u32;
    ctx.r3.u64 = WaitForObjects(&obj, 1, false, ctx.r6.u32, ctx.r5.u32);
    if (g_importTrace && g_importSample)
        Log("IMPORT_TRACE: NtWaitForSingleObjectEx(handle %08X obj %08X, timeout %lld) -> %08X from %08llX", handle, obj,
            timeout ? (long long)Load<int64_t>(timeout) : -1LL, ctx.r3.u32, (unsigned long long)ctx.lr);
}

// NTSTATUS NtWaitForMultipleObjectsEx(u32 count, u32* handles, u32 waitType, u32 mode, u32 alertable, i64* timeout)
GUEST_IMPORT(NtWaitForMultipleObjectsEx)
{
    std::vector<uint32_t> objs;
    if (!HandlesToObjects(ctx.r4.u32, ctx.r3.u32, objs)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    ctx.r3.u64 = WaitForObjects(objs.data(), uint32_t(objs.size()), ctx.r5.u32 == 0, ctx.r8.u32, ctx.r7.u32);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
static int32_t SetEvent(uint32_t obj)
{
    int32_t prev;
    {
        std::lock_guard lock(g_dispatchMutex);
        prev = ObjState(obj);
        SetObjState(obj, 1);
        WakeLocked(obj);
    }
    return prev;
}

static int32_t ResetEvent(uint32_t obj)
{
    std::lock_guard lock(g_dispatchMutex);
    int32_t prev = ObjState(obj);
    SetObjState(obj, 0);
    return prev;
}

// NTSTATUS NtCreateEvent(u32* handle, OBJECT_ATTRIBUTES* attrs, u32 eventType, u32 initialState)
GUEST_IMPORT(NtCreateEvent)
{
    bool existed;
    uint32_t obj = CreateObject(ctx.r4.u32, 16, uint8_t(ctx.r5.u32 ? TYPE_SYNCHRONIZATION_EVENT : TYPE_NOTIFICATION_EVENT), ctx.r6.u32 ? 1 : 0, &existed);
    Store<uint32_t>(ctx.r3.u32, CreateHandle(ObjectKind::Event, obj));
    ctx.r3.u64 = existed ? 0x40000000 : STATUS_SUCCESS; // STATUS_OBJECT_NAME_EXISTS
}

GUEST_IMPORT(NtSetEvent)
{
    if (g_importTrace && g_importSample)
        Log("IMPORT_TRACE: NtSetEvent(handle %08X) from %08llX", ctx.r3.u32, (unsigned long long)ctx.lr);
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(ctx.r3.u32, &kind, &obj)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int32_t prev = SetEvent(obj);
    if (ctx.r4.u32) Store<int32_t>(ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtClearEvent)
{
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(ctx.r3.u32, &kind, &obj)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    ResetEvent(obj);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(KeSetEvent) { ctx.r3.u64 = uint32_t(SetEvent(ctx.r3.u32)); }
GUEST_IMPORT(KeResetEvent) { ctx.r3.u64 = uint32_t(ResetEvent(ctx.r3.u32)); }

// NTSTATUS NtSignalAndWaitForSingleObjectEx(u32 signalHandle, u32 waitHandle, u32 mode, u32 alertable, i64* timeout)
GUEST_IMPORT(NtSignalAndWaitForSingleObjectEx)
{
    ObjectKind kind;
    uint32_t sig, wait;
    if (!LookupHandle(ctx.r3.u32, &kind, &sig) || !LookupHandle(ctx.r4.u32, &kind, &wait)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint8_t t = ObjType(sig);
    if (t == TYPE_SEMAPHORE)
    {
        std::lock_guard lock(g_dispatchMutex);
        SetObjState(sig, ObjState(sig) + 1);
        WakeLocked(sig);
    }
    else if (t == TYPE_MUTANT)
    {
        std::lock_guard lock(g_dispatchMutex);
        SetObjState(sig, ObjState(sig) + 1);
        if (ObjState(sig) > 0) Store<uint32_t>(sig + MUTANT_OWNER, 0);
        WakeLocked(sig);
    }
    else
        SetEvent(sig);
    ctx.r3.u64 = WaitForObjects(&wait, 1, false, ctx.r7.u32, ctx.r6.u32);
}

// ---------------------------------------------------------------------------
// Semaphores and mutants
// ---------------------------------------------------------------------------
// NTSTATUS NtCreateSemaphore(u32* handle, OBJECT_ATTRIBUTES* attrs, i32 initial, i32 maximum)
GUEST_IMPORT(NtCreateSemaphore)
{
    bool existed;
    uint32_t obj = CreateObject(ctx.r4.u32, 20, TYPE_SEMAPHORE, ctx.r5.s32, &existed);
    if (!existed) Store<int32_t>(obj + 16, ctx.r6.s32); // Limit
    Store<uint32_t>(ctx.r3.u32, CreateHandle(ObjectKind::Semaphore, obj));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtReleaseSemaphore(u32 handle, i32 count, i32* previous)
GUEST_IMPORT(NtReleaseSemaphore)
{
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(ctx.r3.u32, &kind, &obj)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int32_t prev;
    {
        std::lock_guard lock(g_dispatchMutex);
        prev = ObjState(obj);
        if (prev + ctx.r4.s32 > Load<int32_t>(obj + 16))
        {
            ctx.r3.u64 = STATUS_SEMAPHORE_LIMIT_EXCEEDED;
            return;
        }
        SetObjState(obj, prev + ctx.r4.s32);
        WakeLocked(obj);
    }
    if (ctx.r5.u32) Store<int32_t>(ctx.r5.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtCreateMutant(u32* handle, OBJECT_ATTRIBUTES* attrs, u32 initialOwner)
GUEST_IMPORT(NtCreateMutant)
{
    bool existed;
    bool owned = ctx.r5.u32 != 0;
    uint32_t obj = CreateObject(ctx.r4.u32, 32, TYPE_MUTANT, owned ? 0 : 1, &existed);
    if (!existed && owned) Store<uint32_t>(obj + MUTANT_OWNER, CurrentKThread());
    Store<uint32_t>(ctx.r3.u32, CreateHandle(ObjectKind::Mutant, obj));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtReleaseMutant(u32 handle, i32* previous)
GUEST_IMPORT(NtReleaseMutant)
{
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(ctx.r3.u32, &kind, &obj)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int32_t prev;
    {
        std::lock_guard lock(g_dispatchMutex);
        if (Load<uint32_t>(obj + MUTANT_OWNER) != CurrentKThread())
        {
            ctx.r3.u64 = STATUS_MUTANT_NOT_OWNED;
            return;
        }
        prev = ObjState(obj);
        SetObjState(obj, prev + 1);
        if (prev + 1 > 0)
            Store<uint32_t>(obj + MUTANT_OWNER, 0);
        WakeLocked(obj);
    }
    if (ctx.r4.u32) Store<int32_t>(ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Timers: a host thread signals the timer object when it fires. APC callbacks
// are not delivered yet.
// ---------------------------------------------------------------------------
namespace
{
    struct TimerState { std::atomic<uint64_t> generation{ 0 }; };
    std::mutex g_timerMutex;
    std::unordered_map<uint32_t, TimerState*> g_timers;
}

// NTSTATUS NtCreateTimer(u32* handle, OBJECT_ATTRIBUTES* attrs, u32 timerType)
GUEST_IMPORT(NtCreateTimer)
{
    bool existed;
    uint32_t obj = CreateObject(ctx.r4.u32, 40, uint8_t(ctx.r5.u32 ? TYPE_SYNCHRONIZATION_TIMER : TYPE_NOTIFICATION_TIMER), 0, &existed);
    {
        std::lock_guard lock(g_timerMutex);
        if (!g_timers.count(obj)) g_timers[obj] = new TimerState();
    }
    Store<uint32_t>(ctx.r3.u32, CreateHandle(ObjectKind::Timer, obj));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtSetTimerEx(u32 handle, i64* dueTime, u32 apcRoutine, u32 apcMode, u32 apcContext,
//                       u32 resume, i32 periodMs, u32* previousState)
GUEST_IMPORT(NtSetTimerEx)
{
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(ctx.r3.u32, &kind, &obj)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int64_t due = Load<int64_t>(ctx.r4.u32);
    int64_t rel100ns = due < 0 ? -due : std::max<int64_t>(0, due - NowFileTime());
    int32_t period = ctx.r9.s32;
    if (ctx.r5.u32)
        Log("NtSetTimerEx: APC routine %08X not delivered (not implemented)", ctx.r5.u32);

    TimerState* ts;
    {
        std::lock_guard lock(g_timerMutex);
        ts = g_timers[obj];
    }
    uint64_t gen = ++ts->generation;
    { std::lock_guard lock(g_dispatchMutex); SetObjState(obj, 0); }
    std::thread([=] {
        PlatformSleepMs(uint32_t(rel100ns / 10000));
        do
        {
            if (ts->generation != gen) return;
            { std::lock_guard lock(g_dispatchMutex); SetObjState(obj, 1); WakeLocked(obj); }
            if (period > 0) PlatformSleepMs(uint32_t(period));
        } while (period > 0);
    }).detach();
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtCancelTimer)
{
    ObjectKind kind;
    uint32_t obj;
    if (!LookupHandle(ctx.r3.u32, &kind, &obj)) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard lock(g_timerMutex);
    if (auto it = g_timers.find(obj); it != g_timers.end()) it->second->generation++;
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Critical sections: RTL_CRITICAL_SECTION { DISPATCHER_HEADER header; i32 lockCount @0x10;
// i32 recursionCount @0x14; u32 owningThread @0x18 }. Kept consistent in guest memory.
// ---------------------------------------------------------------------------
static void InitCriticalSection(uint32_t cs, uint32_t spinCount)
{
    InitHeader(cs, TYPE_SYNCHRONIZATION_EVENT, 4, 0);
    Store<uint8_t>(cs + 1, uint8_t((spinCount + 255) / 256)); // spin count lives in the "absolute" byte
    Store<int32_t>(cs + 0x10, -1);
    Store<int32_t>(cs + 0x14, 0);
    Store<uint32_t>(cs + 0x18, 0);
}

GUEST_IMPORT(RtlInitializeCriticalSection) { InitCriticalSection(ctx.r3.u32, 0); }
GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount) { InitCriticalSection(ctx.r3.u32, ctx.r4.u32); ctx.r3.u64 = STATUS_SUCCESS; }

// The owner word is claimed with an atomic compare-and-swap, so an uncontended
// enter/leave (the vast majority: ~16k a second in gameplay) takes no lock and
// wakes nobody. Contended enters spin briefly, then sleep on g_csCv, which a
// leave notifies only when someone is waiting. Recursion and lock counts are
// written only by the owner.
namespace
{
    std::mutex g_csMutex;
    std::condition_variable g_csCv;
    std::atomic<uint32_t> g_csWaiters{ 0 };

    std::atomic<uint32_t>* CsOwner(uint32_t cs) { return (std::atomic<uint32_t>*)GuestPtr(cs + 0x18); }

    void CsTookOwnership(uint32_t cs)
    {
        Store<int32_t>(cs + 0x14, Load<int32_t>(cs + 0x14) + 1);
        Store<int32_t>(cs + 0x10, Load<int32_t>(cs + 0x10) + 1);
    }
}

GUEST_IMPORT(RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t me = ByteSwapT(CurrentKThread()); // the owner word is big-endian in guest memory
    auto* owner = CsOwner(cs);
    prof::csEnter.fetch_add(1, std::memory_order_relaxed);
    uint32_t cur = owner->load(std::memory_order_relaxed);
    if (cur == me) // recursive enter
    {
        CsTookOwnership(cs);
        return;
    }
    uint32_t expected = 0;
    if (!owner->compare_exchange_strong(expected, me, std::memory_order_acquire))
    {
        prof::csContended.fetch_add(1, std::memory_order_relaxed);
        bool got = false;
        for (int i = 0; i < 100 && !got; i++) // short spin: most holds are brief
        {
            expected = 0;
            got = owner->load(std::memory_order_relaxed) == 0 && owner->compare_exchange_weak(expected, me, std::memory_order_acquire);
        }
        if (!got)
        {
            if (t_thread) t_thread->waitCs = cs;
            g_csWaiters.fetch_add(1);
            std::unique_lock lock(g_csMutex);
            for (;;)
            {
                expected = 0;
                if (owner->compare_exchange_strong(expected, me))
                    break;
                // (The timeout covers owners that release without RtlLeaveCriticalSection.)
                g_csCv.wait_for(lock, std::chrono::milliseconds(5));
                prof::csWakeups.fetch_add(1, std::memory_order_relaxed);
            }
            g_csWaiters.fetch_sub(1);
            if (t_thread) t_thread->waitCs = 0;
        }
    }
    CsTookOwnership(cs);
}

GUEST_IMPORT(RtlTryEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t me = ByteSwapT(CurrentKThread());
    auto* owner = CsOwner(cs);
    uint32_t expected = 0;
    if (owner->load(std::memory_order_relaxed) != me && !owner->compare_exchange_strong(expected, me, std::memory_order_acquire))
    {
        ctx.r3.u64 = 0;
        return;
    }
    CsTookOwnership(cs);
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    int32_t rec = Load<int32_t>(cs + 0x14) - 1;
    Store<int32_t>(cs + 0x14, rec);
    Store<int32_t>(cs + 0x10, Load<int32_t>(cs + 0x10) - 1);
    if (rec > 0)
        return;
    CsOwner(cs)->store(0); // seq_cst: ordered before the waiter check below
    if (g_csWaiters.load() > 0)
    {
        // Taking the mutex orders this notify after a waiter's failed claim
        // (it holds the mutex from its claim attempt until it sleeps).
        { std::lock_guard lock(g_csMutex); }
        g_csCv.notify_all();
    }
}

// ---------------------------------------------------------------------------
// Spinlocks: the lock word holds the owning KTHREAD while held.
// ---------------------------------------------------------------------------
static void SpinAcquire(uint32_t lockAddr)
{
    auto* word = (std::atomic<uint32_t>*)GuestPtr(lockAddr);
    uint32_t me = ByteSwapT(CurrentKThread());
    uint32_t expected = 0;
    while (!word->compare_exchange_weak(expected, me))
    {
        expected = 0;
        PlatformYield();
    }
}
static void SpinRelease(uint32_t lockAddr) { ((std::atomic<uint32_t>*)GuestPtr(lockAddr))->store(0); }

GUEST_IMPORT(KfAcquireSpinLock) { SpinAcquire(ctx.r3.u32); ctx.r3.u64 = 0; } // returns old IRQL
GUEST_IMPORT(KfReleaseSpinLock) { SpinRelease(ctx.r3.u32); }
GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql) { SpinAcquire(ctx.r3.u32); }
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql) { SpinRelease(ctx.r3.u32); }

// SLIST_HEADER { u32 next; u16 depth; u16 sequence } popped with a 64-bit CAS,
// matching the ldarx/stdcx. sequence the game uses to push.
GUEST_IMPORT(InterlockedPopEntrySList)
{
    auto* header = (std::atomic<uint64_t>*)GuestPtr(ctx.r3.u32);
    uint64_t old = header->load();
    for (;;)
    {
        uint64_t v = ByteSwapT(old); // big-endian: next in the high 32 bits
        uint32_t next = uint32_t(v >> 32);
        if (next == 0)
        {
            ctx.r3.u64 = 0;
            return;
        }
        uint16_t depth = uint16_t(v >> 16), seq = uint16_t(v);
        uint64_t nv = (uint64_t(Load<uint32_t>(next)) << 32) | (uint64_t(uint16_t(depth - 1)) << 16) | uint16_t(seq + 1);
        if (header->compare_exchange_weak(old, ByteSwapT(nv)))
        {
            ctx.r3.u64 = next;
            return;
        }
    }
}

void LogGuestRegisters()
{
    GuestThread* t = t_thread;
    if (!t || !t->ctx)
        return;
    PPCContext& c = *t->ctx;
    // Registers XenonRecomp keeps as function locals (config/Crysis2.toml *_as_local)
    // are not in the context and are left out.
    Log("  guest thread %u: lr %08llX r1 %08X r3 %08X r4 %08X r5 %08X r6 %08X r13 %08X",
        t->id, c.lr, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r13.u32);
#ifndef PPC_CONFIG_CTR_AS_LOCAL
    Log("  ctr %08X", c.ctr.u32);
#endif
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
    Log("  r11 %08X r12 %08X", c.r11.u32, c.r12.u32);
#endif
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
    Log("  r31 %08X", c.r31.u32);
#endif
    Log("  stack %08X-%08X, pcr %08X, kthread %08X", t->stackLimit, t->stackBase, t->pcr, t->kthread);
}

// ---------------------------------------------------------------------------
// Runtime-owned guest threads (GPU, interrupts, DPCs): full guest thread state,
// but the body is host code that calls into the guest as needed.
// ---------------------------------------------------------------------------
namespace
{
    struct NativeThreadStart
    {
        GuestThread* thread;
        std::function<void(PPCContext&)> body;
    };

    void NativeThreadMain(NativeThreadStart* start)
    {
        t_thread = start->thread;
        try
        {
            start->body(*start->thread->ctx);
        }
        catch (const ThreadExit&)
        {
        }
        FinishThread(start->thread, 0);
        delete start;
    }
}

void StartNativeGuestThread(const char* name, std::function<void(PPCContext&)> body)
{
    auto* t = new GuestThread();
    t->id = g_nextThreadId++;
    SetupGuestThread(t, 0x40000);
    t->handle = CreateHandle(ObjectKind::Thread, t->kthread);
    auto* start = new NativeThreadStart{ t, std::move(body) };
    if (!PlatformStartThread([start] { NativeThreadMain(start); }, kGuestHostStack))
        Fatal("could not start a host thread for %s", name);
    Log("started runtime thread %u (%s)", t->id, name);
}

// Signal a guest event object (e.g. the completion event of an I/O request).
void SignalEventObject(uint32_t obj) { SetEvent(obj); }

uint32_t CreateDispatcherObject(uint32_t size, uint8_t type, int32_t state)
{
    uint32_t obj = KernelAlloc(size);
    InitHeader(obj, type, uint8_t(size / 4), state);
    return obj;
}

void SetThreadName(uint32_t threadId, const char* name)
{
    GuestThread* t = nullptr;
    if (threadId == 0xFFFFFFFF || (t_thread && threadId == t_thread->id))
        t = t_thread;
    else
    {
        std::lock_guard lock(g_threadsMutex);
        for (auto& [k, th] : g_threadsByKThread)
            if (th->id == threadId) t = th;
    }
    if (t)
        strncpy(t->name, name, sizeof(t->name) - 1);
}

void DumpThreads()
{
    std::vector<GuestThread*> threads;
    {
        std::lock_guard lock(g_threadsMutex);
        for (auto& [k, t] : g_threadsByKThread) threads.push_back(t);
    }
    std::sort(threads.begin(), threads.end(), [](GuestThread* a, GuestThread* b) { return a->id < b->id; });
    Log("==== thread dump (%zu threads) ====", threads.size());
    static const char* filter = getenv("DUMP_THREAD_FILTER");
    for (GuestThread* t : threads)
    {
        if (filter && !strstr(t->name, filter))
            continue;
        char what[160] = "running";
        if (Load<int32_t>(t->kthread + 4) > 0)
            snprintf(what, sizeof(what), "exited");
        else if (uint32_t cs = t->waitCs)
            snprintf(what, sizeof(what), "waiting for critical section %08X (owner kthread %08X)", cs, Load<uint32_t>(cs + 0x18));
        else if (uint32_t n = t->waitCount)
        {
            uint32_t o = t->waitObject;
            snprintf(what, sizeof(what), "waiting on %u object(s), first %08X (type %u, state %d)", n, o, Load<uint8_t>(o), Load<int32_t>(o + 4));
        }
        Log("  t%-3u %-36s lr %08llX  %s", t->id, t->name[0] ? t->name : "?", t->ctx ? (unsigned long long)t->ctx->lr : 0ull, what);
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL // r14-r31 are function locals otherwise
        if (t->ctx)
            Log("        r3 %08X r28 %08X r29 %08X r30 %08X r31 %08X", t->ctx->r3.u32, t->ctx->r28.u32, t->ctx->r29.u32,
                t->ctx->r30.u32, t->ctx->r31.u32);
#else
        if (t->ctx)
            Log("        r3 %08X", t->ctx->r3.u32);
#endif

        // Walk the guest stack: each frame's first word links to the caller's
        // frame, and the return address is saved 8 bytes below that frame.
        if (t->ctx)
        {
            std::string chain;
            uint32_t frame = t->ctx->r1.u32;
            for (int depth = 0; depth < 24; depth++)
            {
                if (frame < t->stackLimit || frame + 4 > t->stackBase)
                    break;
                uint32_t next = Load<uint32_t>(frame);
                if (next <= frame || next > t->stackBase)
                    break;
                char buf[16];
                snprintf(buf, sizeof(buf), " %08X", Load<uint32_t>(next - 8));
                chain += buf;
                frame = next;
            }
            if (!chain.empty())
                Log("       callers:%s", chain.c_str());
        }
    }
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Called from the page (debugging): log every guest thread's state.
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_dump_threads() { DumpThreads(); }
// Read a big-endian guest word (diagnostics from the browser console).
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t crysis_peek32(uint32_t addr) { return Load<uint32_t>(addr); }
#endif
