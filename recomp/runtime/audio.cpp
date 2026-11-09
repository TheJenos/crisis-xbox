// XAudio render-driver imports.
//
// The title registers a client callback; the real driver calls it each time it
// consumes a 256-sample frame (5.33 ms at 48 kHz), and the callback answers
// with XAudioSubmitRenderDriverFrame. Pumping the callback at that rate is what
// advances the title's audio clock (and anything synced to it, like video).
// Submitted samples are discarded for now: no host audio output yet.
#include "guest.h"
#include "platform.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>

namespace
{
    // The callback receives a pointer to a word holding its argument, not the
    // argument itself: `wrappedArg` is that word, in guest memory.
    struct AudioClient { uint32_t callback, arg, wrappedArg; };
    std::mutex g_clientsMutex;
    std::vector<AudioClient> g_clients;
    std::atomic<uint64_t> g_framesSubmitted{ 0 };
    std::once_flag g_pumpStarted;

    void StartAudioPump()
    {
        std::call_once(g_pumpStarted, [] {
            StartNativeGuestThread("XAudio driver", [](PPCContext& ctx) {
                using clock = std::chrono::steady_clock;
                auto next = clock::now();
                const auto frame = std::chrono::nanoseconds(5333333); // 256 samples at 48 kHz
                uint64_t ticks = 0;
                for (;;)
                {
                    next += frame;
                    std::this_thread::sleep_until(next);
                    std::vector<AudioClient> clients;
                    {
                        std::lock_guard lock(g_clientsMutex);
                        clients = g_clients;
                    }
                    for (const auto& c : clients)
                    {
                        ctx.r3.u64 = c.wrappedArg;
                        GuestCall(ctx, c.callback);
                    }
                    if (++ticks == 1 || ticks % 11250 == 0) // first call, then every minute
                        Log("audio: %llu driver callbacks, %llu frames submitted",
                            (unsigned long long)ticks, (unsigned long long)g_framesSubmitted.load());
                }
            });
        });
    }
}

// HRESULT XAudioGetSpeakerConfig(u32* config)
GUEST_IMPORT(XAudioGetSpeakerConfig)
{
    Store<uint32_t>(ctx.r3.u32, 0x00010001); // stereo, digital output
    ctx.r3.u64 = 0;
}

// HRESULT XAudioRegisterRenderDriverClient(u32* callbackAndArg, u32* driver)
GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    uint32_t callbackPtr = ctx.r3.u32, driverOut = ctx.r4.u32;
    AudioClient c{ Load<uint32_t>(callbackPtr), Load<uint32_t>(callbackPtr + 4), KernelAlloc(4) };
    Store<uint32_t>(c.wrappedArg, c.arg);
    uint32_t index;
    {
        std::lock_guard lock(g_clientsMutex);
        index = uint32_t(g_clients.size());
        g_clients.push_back(c);
    }
    Log("audio: render driver client %u: callback %08X, arg %08X", index, c.callback, c.arg);
    Store<uint32_t>(driverOut, 0x41550000 | index); // driver handle
    StartAudioPump();
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XAudioUnregisterRenderDriverClient)
{
    uint32_t index = ctx.r3.u32 & 0xFFFF;
    std::lock_guard lock(g_clientsMutex);
    if (index < g_clients.size())
        g_clients[index].callback = 0, g_clients.erase(g_clients.begin() + index);
    ctx.r3.u64 = 0;
}

// HRESULT XAudioSubmitRenderDriverFrame(u32 driver, void* samples): 6 channels x 256 float samples
GUEST_IMPORT(XAudioSubmitRenderDriverFrame)
{
    g_framesSubmitted++;
    ctx.r3.u64 = 0;
}

// HRESULT XAudioGetVoiceCategoryVolume(u32 category, float* volume)
GUEST_IMPORT(XAudioGetVoiceCategoryVolume)
{
    Store<float>(ctx.r4.u32, 1.0f);
    ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// XMA decoder (silent).
//
// Contexts are 64-byte structures in physical memory that the title fills and
// the XMA hardware processes (layout as in Xenia's XMA_CONTEXT_DATA). The
// hardware is driven through MMIO registers at 0x7FEA0000 + index*4, which in
// this runtime are plain memory, so a host thread polls them:
//   0x600 ContextArrayAddress (physical, read with lwbrx)
//   0x606 CurrentContextIndex (rotating; titles poll it while locking)
//   0x650-0x659 Kick, 0x690-0x699 Lock, 0x6A0-0x6A9 Clear (one bit per context, little-endian)
//
// Decoding produces silence with the real bookkeeping: input packets are
// consumed frame by frame (frame counts from the packet headers), input
// buffers are released and swapped, and 512-sample frames are written to the
// output ring. Titles wait on exactly this; real XMA (WMA Pro) decoding can
// replace the silent frames later.
// ---------------------------------------------------------------------------
namespace
{
    constexpr uint32_t XMA_CONTEXT_SIZE = 64, XMA_CONTEXT_COUNT = 320;
    constexpr uint32_t XMA_REG_BASE = 0x7FEA0000;
    constexpr uint32_t kBytesPerPacket = 2048, kBitsPerPacket = kBytesPerPacket * 8, kBytesPerFrameChannel = 512 * 2;
    std::mutex g_xmaMutex;
    uint32_t g_xmaArray = 0;
    std::vector<bool> g_xmaUsed;
    // Kick enables a context and lock disables it (as in Xenia's XMA decoder):
    // an enabled context keeps decoding whenever the title marks input valid,
    // without being kicked again.
    std::vector<bool> g_xmaEnabled;
    std::vector<uint32_t> g_xmaFramesDone; // frames already produced from the current packet, per context
    std::atomic<uint64_t> g_xmaFrames{ 0 };

    uint32_t* XmaReg(uint32_t index) { return (uint32_t*)GuestPtr(XMA_REG_BASE + index * 4); }
    uint32_t TakeReg(uint32_t index) { return __atomic_exchange_n(XmaReg(index), 0u, __ATOMIC_SEQ_CST); }
    // Registers hold little-endian values (titles use lwbrx/stwbrx); store the raw bytes.
    void SetRegRaw(uint32_t index, uint32_t v) { __atomic_store_n(XmaReg(index), v, __ATOMIC_SEQ_CST); }

    uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
    uint32_t SetBits(uint32_t v, int lo, int n, uint32_t x)
    {
        uint32_t m = ((1u << n) - 1) << lo;
        return (v & ~m) | ((x << lo) & m);
    }

    void XmaClear(uint32_t ctx)
    {
        uint32_t d0 = Load<uint32_t>(ctx), d1 = Load<uint32_t>(ctx + 4), d9 = Load<uint32_t>(ctx + 36);
        d0 = SetBits(SetBits(SetBits(d0, 20, 1, 0), 21, 1, 0), 27, 5, 0); // input valid bits, output write offset
        d1 = SetBits(d1, 31, 1, 0);                                       // output valid
        d9 = SetBits(d9, 0, 5, 0);                                        // output read offset
        Store<uint32_t>(ctx, d0);
        Store<uint32_t>(ctx + 4, d1);
        Store<uint32_t>(ctx + 36, d9);
    }

    // Process one kicked context.
    void XmaWork(uint32_t id)
    {
        uint32_t ctx = g_xmaArray + id * XMA_CONTEXT_SIZE;
        uint32_t d0 = Load<uint32_t>(ctx), d1 = Load<uint32_t>(ctx + 4), d2 = Load<uint32_t>(ctx + 8), d4 = Load<uint32_t>(ctx + 16);
        uint32_t d9 = Load<uint32_t>(ctx + 36);
        if (!Bits(d1, 31, 1)) // output buffer not valid
            return;
        if (!Bits(d0, 20, 1) && !Bits(d0, 21, 1))
        {
            Store<uint32_t>(ctx + 4, SetBits(d1, 31, 1, 0));
            return;
        }
        bool stereo = Bits(d1, 29, 1);
        uint32_t frameBytes = kBytesPerFrameChannel << (stereo ? 1 : 0);
        uint32_t capacity = Bits(d0, 22, 5) * 256;
        uint32_t readOff = Bits(d9, 0, 5) * 256, writeOff = Bits(d0, 27, 5) * 256;
        if (!capacity)
            return;
        // Free space in the output ring (read == write means empty).
        uint32_t free = readOff > writeOff ? readOff - writeOff : capacity - writeOff + readOff;
        free -= free % frameBytes;
        uint32_t out = 0xA0000000 | (Load<uint32_t>(ctx + 28) & 0x1FFFFFFF);
        uint32_t current = Bits(d4, 31, 1);
        uint32_t readBits = Bits(d2, 0, 26);
        uint32_t& framesDone = g_xmaFramesDone[id];

        while (free >= frameBytes)
        {
            bool valid0 = Bits(d0, 20, 1), valid1 = Bits(d0, 21, 1);
            if (!(current ? valid1 : valid0))
            {
                if (!(current ? valid0 : valid1))
                    break; // out of input
                current ^= 1;
                readBits = 0;
                framesDone = 0;
            }
            uint32_t packets = current ? Bits(d1, 0, 12) : Bits(d0, 0, 12);
            uint32_t base = 0xA0000000 | (Load<uint32_t>(ctx + (current ? 24 : 20)) & 0x1FFFFFFF);
            uint32_t packet = readBits / kBitsPerPacket;
            if (packet >= packets)
            {
                // Buffer used up: release it and continue with the other one.
                d0 = SetBits(d0, current ? 21 : 20, 1, 0);
                current ^= 1;
                readBits = 0;
                framesDone = 0;
                continue;
            }
            // Packet header (big-endian): frame count in the top 6 bits, skip count in the low 8.
            uint32_t header = Load<uint32_t>(base + packet * kBytesPerPacket);
            uint32_t frames = header >> 26, skip = header & 0xFF;
            if (framesDone >= frames)
            {
                packet += 1 + (skip == 0xFF ? 0 : skip);
                readBits = packet * kBitsPerPacket;
                framesDone = 0;
                continue;
            }
            // One silent frame into the ring.
            for (uint32_t done = 0; done < frameBytes;)
            {
                uint32_t n = std::min(frameBytes - done, capacity - writeOff);
                memset(GuestPtr(out + writeOff), 0, n);
                writeOff = (writeOff + n) % capacity;
                done += n;
            }
            free -= frameBytes;
            framesDone++;
            readBits = packet * kBitsPerPacket + 32;
            g_xmaFrames++;
        }
        d0 = SetBits(d0, 27, 5, writeOff / 256);
        d2 = SetBits(d2, 0, 26, readBits);
        d4 = SetBits(d4, 31, 1, current);
        if (!Bits(d0, 20, 1) && !Bits(d0, 21, 1))
            d1 = SetBits(d1, 31, 1, 0); // nothing left: the hardware drops the output buffer too
        Store<uint32_t>(ctx, d0);
        Store<uint32_t>(ctx + 4, d1);
        Store<uint32_t>(ctx + 8, d2);
        Store<uint32_t>(ctx + 16, d4);
    }

    // One pass of the decoder: apply clear/lock/kick bits, decode every enabled
    // context, advance the current-context register. Called with g_xmaMutex held.
    uint32_t g_rotating = 1;
    void XmaServiceLocked()
    {
                for (uint32_t g = 0; g < 10; g++)
                {
                    uint32_t clear = TakeReg(0x6A0 + g), lockBits = TakeReg(0x690 + g), kick = TakeReg(0x650 + g);
                    for (uint32_t b = 0; b < 32; b++)
                    {
                        uint32_t id = g * 32 + b;
                        if (id >= XMA_CONTEXT_COUNT)
                            break;
                        if ((clear >> b) & 1)
                        {
                            XmaClear(g_xmaArray + id * XMA_CONTEXT_SIZE);
                            g_xmaFramesDone[id] = 0;
                        }
                        if ((kick >> b) & 1)
                            g_xmaEnabled[id] = true;          // (a lock and kick in one poll: lock, update, kick)
                        else if ((lockBits >> b) & 1)
                            g_xmaEnabled[id] = false;
                    }
                }
                for (uint32_t id = 0; id < XMA_CONTEXT_COUNT; id++)
                    if (g_xmaEnabled[id] && g_xmaUsed[id])
                        XmaWork(id);
                g_rotating = (g_rotating + 1) % XMA_CONTEXT_COUNT;
                SetRegRaw(0x606, __builtin_bswap32(g_rotating));
    }

    void XmaThread()
    {
        uint64_t ticks = 0;
        for (;;)
        {
            {
                std::lock_guard lock(g_xmaMutex);
                XmaServiceLocked();
            }
            if (++ticks % 30000 == 0)
                Log("audio: XMA %llu silent frames produced", (unsigned long long)g_xmaFrames.load());
            PlatformSleepMs(1);
        }
    }
}

// Decode on demand: the title's XMA codec read submits input and then polls
// (lock, check, unlock: ~270k times a second) until the hardware has produced
// output. Real hardware answers in microseconds; the polling thread above
// only runs once per timer tick in the browser (~16 ms), so every read stalled
// a tick, the sound threads fell behind and held the sound library's lock the
// game's main thread needs. The lock hook calls this so the work happens at
// once (at most every 50 us; the polling thread still covers idle contexts).
void XmaPump()
{
    static std::atomic<uint64_t> last{ 0 };
    uint64_t now = PlatformCounterNs(), prev = last.load(std::memory_order_relaxed);
    if (now - prev < 50000 || !last.compare_exchange_strong(prev, now))
        return;
    std::unique_lock lock(g_xmaMutex, std::try_to_lock);
    if (lock.owns_lock() && g_xmaArray)
        XmaServiceLocked();
}

// Called once at startup, before the title runs: the context array must exist
// (and register 0x600 point at it) before the title's XMA library initializes.
void XmaStart()
{
    {
        std::lock_guard lock(g_xmaMutex);
        g_xmaArray = PhysicalAlloc(XMA_CONTEXT_SIZE * XMA_CONTEXT_COUNT, 0x1000);
        memset(GuestPtr(g_xmaArray), 0, XMA_CONTEXT_SIZE * XMA_CONTEXT_COUNT);
        g_xmaUsed.assign(XMA_CONTEXT_COUNT, false);
        g_xmaEnabled.assign(XMA_CONTEXT_COUNT, false);
        g_xmaFramesDone.assign(XMA_CONTEXT_COUNT, 0);
        SetRegRaw(0x600, __builtin_bswap32(g_xmaArray & 0x1FFFFFFF));
    }
    std::thread(XmaThread).detach();
    Log("audio: XMA decoder (silent) with %u contexts at %08X", XMA_CONTEXT_COUNT, g_xmaArray);
}

// NTSTATUS XMACreateContext(u32* context)
GUEST_IMPORT(XMACreateContext)
{
    uint32_t out = ctx.r3.u32, context = 0;
    {
        std::lock_guard lock(g_xmaMutex);
        for (uint32_t i = 0; i < XMA_CONTEXT_COUNT && g_xmaArray; i++)
            if (!g_xmaUsed[i])
            {
                g_xmaUsed[i] = true;
                g_xmaEnabled[i] = false;
                g_xmaFramesDone[i] = 0;
                context = g_xmaArray + i * XMA_CONTEXT_SIZE;
                memset(GuestPtr(context), 0, XMA_CONTEXT_SIZE);
                break;
            }
    }
    Store<uint32_t>(out, context);
    ctx.r3.u64 = context ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

// void XMAReleaseContext(u32 context)
GUEST_IMPORT(XMAReleaseContext)
{
    uint32_t context = ppc_fold_view(ctx.r3.u32);
    std::lock_guard lock(g_xmaMutex);
    if (g_xmaArray && context >= g_xmaArray && context < g_xmaArray + XMA_CONTEXT_SIZE * XMA_CONTEXT_COUNT)
    {
        g_xmaUsed[(context - g_xmaArray) / XMA_CONTEXT_SIZE] = false;
        g_xmaEnabled[(context - g_xmaArray) / XMA_CONTEXT_SIZE] = false;
        memset(GuestPtr(context), 0, XMA_CONTEXT_SIZE);
    }
    ctx.r3.u64 = 0;
}
