// WebAssembly (Emscripten) platform backend.
//
// - Guest memory is a 2 GB block of linear memory; base is offset so that
//   base + 0x40000000 is its start (pointer arithmetic wraps in wasm32).
// - Threads are pthreads (Web Workers sharing memory).
// - Files whose path starts with "http:", "https:" or "/" + "disc" URL scheme
//   below are read with synchronous HTTP range requests, which browsers allow
//   in workers; everything else uses Emscripten's in-memory file system.
#ifdef __EMSCRIPTEN__

#include "platform.h"
#include "guest.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Guest address space
// ---------------------------------------------------------------------------
bool PlatformReserveGuest(uint8_t** base)
{
    // Linear memory above the heap's end has never been handed out and reads
    // as zero. Clearing all 2 GB by hand made every page of it real memory at
    // startup, whether the title ever used it or not (gta4-web found the same:
    // its tab took 14 GB). Only the part below the old end, which the
    // allocator may have used before, is cleared.
    const uintptr_t oldEnd = uintptr_t(sbrk(0));
    void* window = aligned_alloc(0x10000, GUEST_HIGH - GUEST_LOW);
    if (!window)
        return false;
    const uintptr_t start = uintptr_t(window), end = start + (GUEST_HIGH - GUEST_LOW);
    static const bool clearAll = getenv("GUEST_CLEAR_ALL") != nullptr; // (the old way, for comparisons)
    if (clearAll)
        memset(window, 0, GUEST_HIGH - GUEST_LOW);
    else if (start < oldEnd)
        memset(window, 0, std::min(end, oldEnd) - start);
    Log("platform: guest memory at %08X, %s", uint32_t(start),
        clearAll ? "cleared in full" : start < oldEnd ? "cleared below the old heap end" : "fresh (not cleared)");
    *base = (uint8_t*)(uintptr_t(window) - GUEST_LOW); // wraps modulo 2^32
    return true;
}

bool PlatformCommit(uint8_t*, size_t) { return true; }      // linear memory is always accessible

// Decommitted ranges must read as zero again, but a written page of linear
// memory stays committed for good: only pages that aren't zero already are
// cleared. The title decommits ranges it never touched, which a plain memset
// turned into real memory. (Reading an untouched page costs nothing where the
// system maps a shared zero page: Linux, Android, ChromeOS, macOS.)
void PlatformDecommit(uint8_t* p, size_t n)
{
    static const bool clearAll = getenv("GUEST_CLEAR_ALL") != nullptr; // (the old way, for comparisons)
    if (clearAll)
    {
        memset(p, 0, n);
        return;
    }
    constexpr size_t kPage = 4096;
    for (size_t at = 0; at < n;)
    {
        size_t pageEnd = std::min(n, size_t(((uintptr_t(p + at) | (kPage - 1)) + 1) - uintptr_t(p)));
        const uint8_t* q = p + at;
        size_t len = pageEnd - at, i = 0;
        bool zero = true;
        for (; zero && i + 8 <= len; i += 8)
        {
            uint64_t w;
            memcpy(&w, q + i, 8);
            zero = w == 0;
        }
        for (; zero && i < len; i++)
            zero = q[i] == 0;
        if (!zero)
            memset(p + at, 0, len);
        at = pageEnd;
    }
}

// ---------------------------------------------------------------------------
// Threads and time
// ---------------------------------------------------------------------------
static void* ThreadTrampoline(void* param)
{
    auto* body = (std::function<void()>*)param;
    (*body)();
    delete body;
    return nullptr;
}

bool PlatformStartThread(std::function<void()> body, size_t stackSize)
{
    auto* heapBody = new std::function<void()>(std::move(body));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackSize);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int rc = pthread_create(&t, &attr, ThreadTrampoline, heapBody);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        delete heapBody;
        return false;
    }
    return true;
}

void PlatformSleepMs(uint32_t ms) { usleep(useconds_t(ms) * 1000); }

// Debug dumps: PUT to the local server's /upload/<name> (kept only when serve.py
// runs with DUMP_DIR). Works from any thread; copies out of shared memory first.
EM_JS(void, js_upload_dump, (const char* name, const void* data, int size), {
    var copy = new Uint8Array(size);
    copy.set(new Uint8Array(wasmMemory.buffer, data >>> 0, size));
    fetch('/upload/' + encodeURIComponent(UTF8ToString(name >>> 0)), { method: 'PUT', body: copy })
        .then(function (r) { if (!r.ok) console.error('dump upload failed: HTTP ' + r.status); })
        .catch(function (e) { console.error('dump upload failed', e); });
});

void PlatformUploadDump(const char* name, const void* data, size_t size) { js_upload_dump(name, data, int(size)); }

// Browsers on Windows time out a worker's waits at the system timer tick
// (15.6 ms), so the title's Sleep(1) took ~16 ms and Sleep(10) ~16 ms. It polls
// with short sleeps (render thread handshake, culling, the sound mixer's 10 ms
// blocks), which made frames crawl and the mixer fall behind. Sleep only the
// part that's longer than a tick, then wait out the rest against the
// high-resolution clock. (The runtime's own polling loops keep PlatformSleepMs.)
//
// The part shorter than a tick used to be spun by each sleeping thread itself,
// and the title's audio, streaming, main and render threads sleep 1-10 ms at
// a time: profiles showed four or five cores doing nothing but reading the
// clock, slowing the threads that shared their cores. Now one timer thread
// watches the clock for everyone. A sleeper waits on its own futex word
// (a wake takes well under a millisecond) and the timer thread wakes it at its
// deadline; it waits coarsely itself while the next deadline is a tick away.
namespace
{
    constexpr uint32_t kTickMs = 17; // a tick plus margin
    struct PreciseSleeper { double deadline; std::atomic<uint32_t>* word; };
    pthread_mutex_t g_sleepMutex = PTHREAD_MUTEX_INITIALIZER;
    std::vector<PreciseSleeper> g_sleepers;   // unsorted (a handful at a time)
    std::atomic<uint32_t> g_sleepersChanged{ 0 }; // bumped (and woken) when one is added
    pthread_once_t g_sleepTimerOnce = PTHREAD_ONCE_INIT;

    void* SleepTimerThread(void*)
    {
        // How long before a deadline the coarse wait has to end: the longest
        // recent overshoot of a coarse wait plus 1 ms. It starts at a whole tick
        // and shrinks by 2% a wait unless a wait overshoots more (on this
        // PC's browser a 1 ms wait takes ~5 ms): spinning only that stretch,
        // not the last 17 ms before every deadline, frees most of a core.
        double window = kTickMs;
        // SLEEP_WINDOW=<ms>: a fixed window instead (comparisons).
        static const double fixedWindow = getenv("SLEEP_WINDOW") ? atof(getenv("SLEEP_WINDOW")) : 0.0;
        if (fixedWindow > 0) window = fixedWindow;
        // (PROFILE: how late the wakes are, from this thread's side.)
        double statsFrom = emscripten_get_now(), lateSum = 0, lateMax = 0;
        uint32_t wakes = 0;
        for (;;)
        {
            double now = emscripten_get_now(), next = INFINITY;
            if (prof::Enabled() && now - statsFrom > 10000)
            {
                Log("PROFILE sleep timer: %.0f wakes/s, late %.2f ms on average (at most %.1f), spin window %.1f ms",
                    wakes * 1000.0 / (now - statsFrom), wakes ? lateSum / wakes : 0.0, lateMax, window);
                statsFrom = now, lateSum = lateMax = 0, wakes = 0;
            }
            pthread_mutex_lock(&g_sleepMutex);
            for (size_t i = 0; i < g_sleepers.size();)
            {
                if (g_sleepers[i].deadline <= now)
                {
                    wakes++, lateSum += now - g_sleepers[i].deadline, lateMax = std::max(lateMax, now - g_sleepers[i].deadline);
                    g_sleepers[i].word->store(1);
                    emscripten_futex_wake(g_sleepers[i].word, 1);
                    g_sleepers[i] = g_sleepers.back();
                    g_sleepers.pop_back();
                }
                else
                    next = std::min(next, g_sleepers[i].deadline), i++;
            }
            uint32_t changed = g_sleepersChanged.load();
            pthread_mutex_unlock(&g_sleepMutex);
            if (next == INFINITY)
                emscripten_futex_wait(&g_sleepersChanged, changed, INFINITY); // until someone sleeps
            else if (next - now > window + 1.0)
            {
                // Coarse (a new sleeper wakes it). Waits shorter than 1 ms are
                // not used: some browsers stretch those to a whole tick.
                double ask = next - now - window;
                int r = emscripten_futex_wait(&g_sleepersChanged, changed, ask);
                if (r == -ETIMEDOUT && fixedWindow <= 0)
                {
                    double over = emscripten_get_now() - now - ask;
                    window = std::clamp(std::max(over + 1.0, window * 0.98), 2.0, double(kTickMs));
                }
            }
            else
                while (emscripten_get_now() < next && g_sleepersChanged.load(std::memory_order_relaxed) == changed)
                    ; // the last stretch against the clock (or a new, maybe earlier, deadline)
        }
        return nullptr;
    }
    void StartSleepTimer()
    {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 << 10);
        pthread_create(&t, &attr, SleepTimerThread, nullptr);
        pthread_attr_destroy(&attr);
    }

    // How short sleeps are done, decided once from how long a 1 ms wait takes:
    // - Wait: the browser times short waits well (Android, macOS, Linux), so a
    //   sleep is just a timed wait and nothing spins. Phones have few fast cores,
    //   and spinning threads there slowed the ones doing the work.
    // - Timer: waits end on a coarse tick (browsers on Windows): the timer
    //   thread above.
    // - Spin: each sleeper spins on the clock (the old way, for comparisons).
    // PRECISE_SLEEP=wait|timer|spin forces one.
    enum class SleepMode { Wait, Timer, Spin };
    bool g_yieldWaits = false; // zero-length sleeps wait 50 us instead of spinning (set with the mode)
    SleepMode DecideSleepMode()
    {
        const char* v = getenv("PRECISE_SLEEP");
        if (v && !strcmp(v, "spin")) return SleepMode::Spin;
        if (v && !strcmp(v, "timer")) return SleepMode::Timer;
        if (v && !strcmp(v, "wait")) return g_yieldWaits = true, SleepMode::Wait;
        static std::atomic<uint32_t> never{ 0 };
        auto shortest = [](double ms) {
            double best = INFINITY;
            for (int i = 0; i < 3; i++)
            {
                double t0 = emscripten_get_now();
                emscripten_futex_wait(&never, 0, ms);
                best = std::min(best, emscripten_get_now() - t0);
            }
            return best;
        };
        double one = shortest(1.0), tenth = shortest(0.1);
        bool precise = one < 4.0;
        g_yieldWaits = precise && tenth < 0.5; // (only if even very short waits end on time)
        Log("platform: a 1 ms wait takes %.2f ms, a 0.1 ms wait %.2f ms: %s%s", one, tenth,
            precise ? "sleeps are timed waits" : "a timer thread ends short sleeps against the clock",
            g_yieldWaits ? ", yields wait 50 us" : "");
        return precise ? SleepMode::Wait : SleepMode::Timer;
    }
    SleepMode Mode()
    {
        static const SleepMode mode = DecideSleepMode();
        return mode;
    }
    thread_local std::atomic<uint32_t> t_idle{ 0 }; // never signalled: a timed wait's word
}

void PlatformSleepMsPrecise(uint32_t ms)
{
    if (ms == 0)
        return;
    double target = emscripten_get_now() + double(ms);
    if (Mode() == SleepMode::Wait)
    {
        for (double left = ms; left > 0; left = target - emscripten_get_now())
            emscripten_futex_wait(&t_idle, 0, left);
        return;
    }
    if (ms > kTickMs)
        usleep(useconds_t(ms - kTickMs) * 1000);
    if (Mode() == SleepMode::Spin)
    {
        while (emscripten_get_now() < target)
            ;
        return;
    }
    pthread_once(&g_sleepTimerOnce, StartSleepTimer);
    static thread_local std::atomic<uint32_t> word;
    word.store(0);
    pthread_mutex_lock(&g_sleepMutex);
    g_sleepers.push_back({ target, &word });
    g_sleepersChanged.fetch_add(1);
    pthread_mutex_unlock(&g_sleepMutex);
    emscripten_futex_wake(&g_sleepersChanged, 1);
    while (word.load() == 0)
    {
        double left = target - emscripten_get_now();
        if (left < -100.0) // (never expected: the timer thread is late or gone)
        {
            pthread_mutex_lock(&g_sleepMutex);
            for (size_t i = 0; i < g_sleepers.size(); i++)
                if (g_sleepers[i].word == &word) { g_sleepers[i] = g_sleepers.back(); g_sleepers.pop_back(); break; }
            pthread_mutex_unlock(&g_sleepMutex);
            break;
        }
        emscripten_futex_wait(&word, 0, std::max(left, 0.0) + 20.0); // the timer thread's wake, or a safety timeout
    }
}
// The title's zero-length sleeps (spin-wait loops). sched_yield does nothing in
// WebAssembly, so such a loop keeps a core busy; where short waits are timed
// well, each yield waits 50 microseconds instead and leaves the core to others.
void PlatformYield()
{
    if (Mode() == SleepMode::Wait && g_yieldWaits)
        emscripten_futex_wait(&t_idle, 0, 0.05);
    else
        sched_yield();
}

void PlatformWaitWord(volatile uint32_t* p, uint32_t expected, double ms) { emscripten_futex_wait(p, expected, ms); }
void PlatformWakeWord(volatile uint32_t* p) { emscripten_futex_wake(p, INT_MAX); }

// The browser's monotonic clock counts from 1970 (performance.timeOrigin), so
// the guest's 50 MHz time base would read like a console that has been on for
// decades, and the title's 64-bit tick arithmetic (ticks * 100000 / frequency)
// overflows. Count from startup instead, plus a minute, like a fresh boot.
// (emscripten_get_now directly: the title reads its clock hundreds of
// thousands of times a second, and clock_gettime adds the WASI layer and a
// BigInt conversion to each read. Same clock: timeOrigin + performance.now().)
static uint64_t RawMonotonicNs()
{
    return uint64_t(emscripten_get_now() * 1e6);
}

uint64_t PlatformCounterNs()
{
    static const uint64_t start = RawMonotonicNs() - 60ull * 1000000000ull;
    return RawMonotonicNs() - start;
}

int64_t PlatformFileTimeNow()
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t(ts.tv_sec) * 10000000ll + ts.tv_nsec / 100) + 116444736000000000ll;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
// Synchronous ranged GET into linear memory; returns bytes read, or -(HTTP
// status) on an HTTP error, or -1 if the request failed outright.
// Pointers arrive as signed 32-bit values; `>>> 0` makes them unsigned (memory above 2 GB).
EM_JS(int, js_http_read, (const char* url, double offset, int size, void* dest), {
    try {
        var xhr = new XMLHttpRequest();
        xhr.open('GET', UTF8ToString(url >>> 0), false);
        xhr.responseType = 'arraybuffer';
        xhr.setRequestHeader('Range', 'bytes=' + offset + '-' + (offset + size - 1));
        xhr.send(null);
        if (xhr.status != 206) return xhr.status ? -xhr.status : -1; // (200 = whole file: range ignored)
        var data = new Uint8Array(xhr.response);
        new Uint8Array(wasmMemory.buffer, dest >>> 0, Math.min(data.length, size)).set(data.subarray(0, size));
        return Math.min(data.length, size);
    } catch (e) {
        console.error('disc read failed', e);
        return -1;
    }
});

// Size of a URL via a HEAD request, or -1.
EM_JS(double, js_http_size, (const char* url), {
    try {
        var xhr = new XMLHttpRequest();
        xhr.open('HEAD', UTF8ToString(url >>> 0), false);
        xhr.send(null);
        if (xhr.status != 200) return -1;
        return Number(xhr.getResponseHeader('Content-Length'));
    } catch (e) {
        return -1;
    }
});

struct PlatformFile
{
    std::string url; // non-empty: read-only HTTP-backed file
    int fd = -1;
    uint64_t size = 0;
};

static bool IsUrl(const std::string& p) { return p.rfind("http:", 0) == 0 || p.rfind("https:", 0) == 0 || p.rfind("url:", 0) == 0; }

PlatformFile* PlatformOpenFile(const std::string& path, OpenMode mode)
{
    if (IsUrl(path))
    {
        if (mode != OpenMode::Read)
            return nullptr;
        auto* f = new PlatformFile();
        f->url = path.rfind("url:", 0) == 0 ? path.substr(4) : path;
        double size = js_http_size(f->url.c_str());
        if (size < 0)
        {
            delete f;
            return nullptr;
        }
        f->size = uint64_t(size);
        return f;
    }
    int flags = mode == OpenMode::Read ? O_RDONLY : O_RDWR;
    if (mode == OpenMode::CreateReadWrite) flags |= O_CREAT;
    if (mode == OpenMode::TruncateReadWrite) flags |= O_CREAT | O_TRUNC;
    int fd = open(path.c_str(), flags, 0644);
    if (fd < 0)
        return nullptr;
    auto* f = new PlatformFile();
    f->fd = fd;
    return f;
}

// Disc streaming activity for the page's loading indicator (read by JS on the
// main thread through crysis_disc_stats).
static std::atomic<uint64_t> g_discBytes{ 0 };
static std::atomic<uint32_t> g_discBusy{ 0 };

extern "C" EMSCRIPTEN_KEEPALIVE double crysis_disc_bytes() { return double(g_discBytes.load()); }
extern "C" EMSCRIPTEN_KEEPALIVE int crysis_disc_busy() { return int(g_discBusy.load()); }

bool PlatformReadAt(PlatformFile* f, uint64_t offset, void* dest, uint32_t size)
{
    if (!f->url.empty())
    {
        // Large reads are split so each request stays reasonable.
        uint8_t* out = (uint8_t*)dest;
        while (size > 0)
        {
            uint32_t chunk = size < (8u << 20) ? size : (8u << 20);
            // Requests can fail transiently (connection limits, a busy server):
            // retry with back-off, since a lost read can stall the title for good.
            int got = -1;
            for (int attempt = 0; attempt < 8; attempt++)
            {
                g_discBusy++;
                got = js_http_read(f->url.c_str(), double(offset), int(chunk), out);
                g_discBusy--;
                if (got == int(chunk))
                    break;
                Log("disc: read of %s @ %llu (%u bytes) failed (result %d), attempt %d", f->url.c_str(),
                    (unsigned long long)offset, chunk, got, attempt + 1);
                PlatformSleepMs(50u << std::min(attempt, 5));
            }
            if (got > 0)
                g_discBytes += uint64_t(got);
            if (got != int(chunk))
                return false;
            out += chunk;
            offset += chunk;
            size -= chunk;
        }
        return true;
    }
    return pread(f->fd, dest, size, off_t(offset)) == ssize_t(size);
}

bool PlatformWriteAt(PlatformFile* f, uint64_t offset, const void* src, uint32_t size)
{
    return f->url.empty() && pwrite(f->fd, src, size, off_t(offset)) == ssize_t(size);
}

void PlatformCloseFile(PlatformFile* f)
{
    if (!f) return;
    if (f->fd >= 0) close(f->fd);
    delete f;
}

uint64_t PlatformFileSize(PlatformFile* f)
{
    if (!f->url.empty())
        return f->size;
    struct stat st;
    return fstat(f->fd, &st) == 0 ? uint64_t(st.st_size) : 0;
}

bool PlatformSetFileSize(PlatformFile* f, uint64_t size) { return f->url.empty() && ftruncate(f->fd, off_t(size)) == 0; }

PathKind PlatformPathKind(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return PathKind::Missing;
    return S_ISDIR(st.st_mode) ? PathKind::Directory : PathKind::File;
}

bool PlatformListDirectory(const std::string& path, std::vector<PlatformDirEntry>& out)
{
    out.clear();
    DIR* d = opendir(path.c_str());
    if (!d)
        return false;
    while (dirent* e = readdir(d))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        struct stat st;
        std::string full = path + "/" + e->d_name;
        bool ok = stat(full.c_str(), &st) == 0;
        out.push_back({ e->d_name, ok && S_ISDIR(st.st_mode), ok ? uint64_t(st.st_size) : 0 });
    }
    closedir(d);
    return true;
}

bool PlatformRemoveFile(const std::string& path) { return unlink(path.c_str()) == 0; }

bool PlatformMakeDirectory(const std::string& path) { return mkdir(path.c_str(), 0755) == 0 || PlatformPathKind(path) == PathKind::Directory; }
char PlatformPathSeparator() { return '/'; }

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
void PlatformExit(int code)
{
    fflush(nullptr);
    emscripten_force_exit(code);
    __builtin_unreachable();
}

void PlatformInstallCrashHandler() {}

#endif // __EMSCRIPTEN__
