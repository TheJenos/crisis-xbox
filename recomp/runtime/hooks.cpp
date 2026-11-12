// Diagnostic call tracing for recompiled functions.
//
// XenonRecomp emits each function as a weak symbol sub_X that forwards to the
// body __imp__sub_X. Defining a strong sub_X here replaces it everywhere (direct
// calls and the function-pointer table), so a hook can log and then call the body.
// TRACE_HOOKS=1 enables the logging; otherwise hooks just forward.
#include "guest.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace
{
    const bool g_traceHooks = getenv("TRACE_HOOKS") != nullptr;

    // Copy a guest C string for logging if `addr` looks like one.
    std::string MaybeString(uint32_t addr)
    {
        if (addr < 0x40000000 || addr >= 0xC0000000)
            return {};
        std::string s = LoadGuestString(addr, 80);
        for (char c : s)
            if (c < 0x20 || c > 0x7E)
                return {};
        return s.size() >= 3 ? " \"" + s + "\"" : std::string();
    }
}

#define TRACE_HOOK(addr, what)                                                                          \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                             \
    PPC_FUNC(sub_##addr)                                                                                \
    {                                                                                                   \
        static std::atomic<int> calls{ 0 };                                                             \
        bool log = g_traceHooks && calls++ < 20;                                                        \
        uint64_t lr = ctx.lr;                                                                           \
        if (log)                                                                                        \
            Log("hook: %s (sub_" #addr ")(r3=%08X%s, r4=%08X%s, r5=%08X%s) from %08llX", what,         \
                ctx.r3.u32, MaybeString(ctx.r3.u32).c_str(), ctx.r4.u32, MaybeString(ctx.r4.u32).c_str(), \
                ctx.r5.u32, MaybeString(ctx.r5.u32).c_str(), lr);                                       \
        __imp__sub_##addr(ctx, base);                                                                   \
        if (log)                                                                                        \
            Log("hook: %s (sub_" #addr ") returned %08X", what, ctx.r3.u32);                            \
    }

TRACE_HOOK(828ACF90, "intro movies")
TRACE_HOOK(828ABD20, "play intro video")
TRACE_HOOK(828ABFF0, "wait for m_status")
// CFlashPlayer::Load(this, path, flags): log the player's vtable once (diagnostics).
extern "C" PPC_FUNC(__imp__sub_82316F60);
PPC_FUNC(sub_82316F60)
{
    uint32_t self = ctx.r3.u32;
    std::string path = MaybeString(ctx.r4.u32);
    uint64_t lr = ctx.lr;
    __imp__sub_82316F60(ctx, base);
    if (g_traceHooks)
    {
        Log("hook: flash load%s (player %08X) from %08llX -> %u", path.c_str(), self, lr, ctx.r3.u32);
        static bool dumped = false;
        if (!dumped)
        {
            dumped = true;
            uint32_t vt = Load<uint32_t>(self);
            std::string list;
            for (int i = 0; i < 96; i++)
            {
                char b[16];
                snprintf(b, sizeof(b), " %d:%08X", i, Load<uint32_t>(vt + i * 4));
                list += b;
            }
            Log("hook: flash player vtable %08X:%s", vt, list.c_str());
        }
    }
}
TRACE_HOOK(8231A018, "video buffer time setup")
TRACE_HOOK(83658640, "create video I/O object")

// Entries of the CRI file-system interface table at 82175B38 (index 6 creates the I/O object).
TRACE_HOOK(836583E8, "CRI fs [0]")
TRACE_HOOK(83657D00, "CRI fs [1]")
TRACE_HOOK(83658D58, "CRI fs [2]")
TRACE_HOOK(836587C0, "CRI fs [3]")
TRACE_HOOK(83657C90, "CRI fs [4]")
TRACE_HOOK(83658EC8, "CRI fs [5]")
TRACE_HOOK(83652850, "CRI fs [9]")
TRACE_HOOK(83658D60, "CRI fs [11]")

// IFlashPlayer methods (vtable 82015848), by index (CFlashPlayer only).
TRACE_HOOK(823149E0, "flash [3]")
TRACE_HOOK(82312B08, "flash [4]")
TRACE_HOOK(82312B78, "flash [5]")
TRACE_HOOK(82312BF0, "flash [6]")
TRACE_HOOK(82312F78, "flash [7]")
TRACE_HOOK(82312CB0, "flash [8]")
TRACE_HOOK(82312D30, "flash [9]")
TRACE_HOOK(82312DD0, "flash [10]")
TRACE_HOOK(82312E98, "flash [11]")
TRACE_HOOK(82313060, "flash [12]")
TRACE_HOOK(82313138, "flash [13]")
// [14] (sub_82313218) is Advance: hooked in prompts.cpp.
TRACE_HOOK(823132E0, "flash [15]")
TRACE_HOOK(82316AE8, "flash [16]")
TRACE_HOOK(82310A88, "flash [21]")
TRACE_HOOK(82310A90, "flash [22]")
TRACE_HOOK(82313408, "flash [23]")
TRACE_HOOK(82313468, "flash [24]")
TRACE_HOOK(823134D8, "flash [25]")
TRACE_HOOK(82313528, "flash [26]")
TRACE_HOOK(82313578, "flash [27]")
TRACE_HOOK(823135E8, "flash [28]")
TRACE_HOOK(82313658, "flash [29]")
TRACE_HOOK(82310A98, "flash [30]")
TRACE_HOOK(82310AA8, "flash [31]")
TRACE_HOOK(823136C8, "flash [32]")
TRACE_HOOK(823138B8, "flash [33]")
TRACE_HOOK(823139C0, "flash [34]")
TRACE_HOOK(82313A10, "flash [35]")
TRACE_HOOK(82313B00, "flash [36]")
TRACE_HOOK(82313A80, "flash [37]")
TRACE_HOOK(823151A8, "flash [38]")
TRACE_HOOK(82313BC0, "flash [39]")
TRACE_HOOK(82313CA0, "flash [40]")
TRACE_HOOK(82313D00, "flash [41]")
TRACE_HOOK(82313DD8, "flash [42]")
TRACE_HOOK(82313E38, "flash [43]")
TRACE_HOOK(82313F18, "flash [44]")
TRACE_HOOK(823152C0, "flash [45]")
TRACE_HOOK(823153E0, "flash [46]")
TRACE_HOOK(82315500, "flash [47]")
TRACE_HOOK(823156F8, "flash [48]")
TRACE_HOOK(82315DA0, "flash [49]")
TRACE_HOOK(823140A8, "flash [54]")
TRACE_HOOK(823140D0, "flash [55]")
TRACE_HOOK(823140E0, "flash [56]")
TRACE_HOOK(823141A8, "flash [57]")
TRACE_HOOK(82316D70, "flash [58]")
TRACE_HOOK(83107300, "render candidate")
TRACE_HOOK(83107938, "render candidate")
TRACE_HOOK(8310D2C8, "render candidate")
TRACE_HOOK(8310D788, "render candidate")
TRACE_HOOK(8310DA38, "render candidate")
TRACE_HOOK(8310EC78, "render candidate")
TRACE_HOOK(831114E8, "render candidate")
TRACE_HOOK(83112DB0, "render candidate")
TRACE_HOOK(831134C0, "render candidate")
TRACE_HOOK(83114530, "render candidate")
TRACE_HOOK(83114D20, "render candidate")
TRACE_HOOK(83115ED0, "render candidate")
TRACE_HOOK(831173C8, "render candidate")

// IFlashPlayer::Render (vtable slot 17): log the per-buffer display slots it checks.
extern "C" PPC_FUNC(__imp__sub_82313348);
PPC_FUNC(sub_82313348)
{
    uint32_t self = ctx.r3.u32;
    static std::atomic<int> calls{ 0 };
    int n = calls++;
    if (g_traceHooks && (n < 5 || n % 200 == 0))
    {
        uint32_t table = Load<uint32_t>(self + 184);
        uint32_t renderer = Load<uint32_t>(self + 132);
        std::string slots;
        for (int i = 0; i < 4; i++)
        {
            char b[48];
            snprintf(b, sizeof(b), " [%d]=%08X/%08X", i, Load<uint32_t>(table + i * 28), Load<uint32_t>(table + i * 28 + 4));
            slots += b;
        }
        Log("hook: flash Render #%d player %08X slots%s; renderer %08X thread-id field %08X, current thread %08X",
            n, self, slots.c_str(), renderer, renderer ? Load<uint32_t>(renderer + 776) : 0, CurrentThreadId());
        if (n == 0 && renderer)
        {
            // The object Render calls into (via 8333E3F8) is not necessarily the
            // renderer itself; log the vtable of what that helper returns instead.
        }
    }
    __imp__sub_82313348(ctx, base);
}

// The helper Render uses to get the renderer interface; log its vtable once.
extern "C" PPC_FUNC(__imp__sub_8333E3F8);
PPC_FUNC(sub_8333E3F8)
{
    uint64_t lr = ctx.lr;
    __imp__sub_8333E3F8(ctx, base);
    static bool logged = false;
    if (g_traceHooks && !logged && lr == 0x82313368 && ctx.r3.u32 >= 0x40000000 && ctx.r3.u32 < 0xC0000000)
    {
        logged = true;
        uint32_t vt = Load<uint32_t>(ctx.r3.u32);
        Log("hook: flash renderer interface %08X vtable %08X: [103]=%08X [104]=%08X [102]=%08X [175]=%08X",
            ctx.r3.u32, vt, Load<uint32_t>(vt + 412), Load<uint32_t>(vt + 416), Load<uint32_t>(vt + 408), Load<uint32_t>(vt + 700));
    }
}
TRACE_HOOK(836D78B0, "d3d shader magic user")
TRACE_HOOK(836F1E18, "d3d shader magic user")
TRACE_HOOK(836F2EB0, "d3d shader magic user")
TRACE_HOOK(836F53C8, "d3d shader magic user")
TRACE_HOOK(83709F10, "d3d shader magic user")
TRACE_HOOK(83731B60, "d3d shader magic user")
TRACE_HOOK(8378BE60, "d3d shader magic user")
TRACE_HOOK(837EFCF8, "d3d shader magic user")
TRACE_HOOK(8388CA98, "d3d shader magic user")

// Renderer "system resources" flag (renderer+2904): Flash rendering is skipped while it is non-zero.
extern "C" PPC_FUNC(__imp__sub_828AD0D0);
PPC_FUNC(sub_828AD0D0)
{
    uint32_t r = ctx.r3.u32;
    uint8_t a0 = Load<uint8_t>(r + 2904), b0 = Load<uint8_t>(r + 2905);
    __imp__sub_828AD0D0(ctx, base);
    if (g_traceHooks)
        Log("hook: renderer: init system render resources: flag %u/%u -> %u/%u, returned %u",
            a0, b0, Load<uint8_t>(r + 2904), Load<uint8_t>(r + 2905), ctx.r3.u32);
}
TRACE_HOOK(828BABB0, "renderer: start clearing render resources")
TRACE_HOOK(82874070, "renderer: function that sets the resources flag from r22")

TRACE_HOOK(82316C38, "flash RT render (player)")
extern "C" PPC_FUNC(__imp__sub_823204B0);
PPC_FUNC(sub_823204B0)
{
    static bool logged = false;
    if (g_traceHooks && !logged)
    {
        logged = true;
        uint32_t vt = Load<uint32_t>(ctx.r4.u32);
        std::string list;
        for (int i = 0; i < 80; i++)
        {
            char b[16];
            snprintf(b, sizeof(b), " %d:%08X", i, Load<uint32_t>(vt + i * 4));
            list += b;
        }
        Log("hook: GRenderer %08X vtable %08X:%s", ctx.r4.u32, vt, list.c_str());
        // The display list being replayed: log its first words.
        std::string words;
        for (int i = 0; i < 12; i++)
        {
            char b[12];
            snprintf(b, sizeof(b), " %08X", Load<uint32_t>(ctx.r3.u32 + i * 4));
            words += b;
        }
        Log("hook: display list object %08X:%s", ctx.r3.u32, words.c_str());
    }
    __imp__sub_823204B0(ctx, base);
}
TRACE_HOOK(82852CE8, "renderer RT FlashRender")

// GRenderer methods (vtable 82016440), by index.
TRACE_HOOK(8231BE18, "GRenderer [2]")
TRACE_HOOK(8231BE78, "GRenderer [3]")
TRACE_HOOK(8231F8D0, "GRenderer [12]")
TRACE_HOOK(8231E008, "GRenderer [13]")
TRACE_HOOK(8231E098, "GRenderer [14]")
TRACE_HOOK(8231E148, "GRenderer [16]")
TRACE_HOOK(8231E1D0, "GRenderer [17]")
TRACE_HOOK(8231E260, "GRenderer [18]")
TRACE_HOOK(8231E328, "GRenderer [21]")
TRACE_HOOK(8231E3B8, "GRenderer [22]")
TRACE_HOOK(8231E458, "GRenderer [23]")
TRACE_HOOK(8231BCA0, "GRenderer [24]")
TRACE_HOOK(8231E4F8, "GRenderer [26]")
TRACE_HOOK(8231E680, "GRenderer [27]")
TRACE_HOOK(8231CA18, "GRenderer [28]")
TRACE_HOOK(8231E7C0, "GRenderer [29]")
TRACE_HOOK(8231E928, "GRenderer [30]")
TRACE_HOOK(8231EA40, "GRenderer [32]")
TRACE_HOOK(8231EB00, "GRenderer [34]")
TRACE_HOOK(8231EBC0, "GRenderer [35]")
TRACE_HOOK(8231ECF8, "GRenderer [36]")
TRACE_HOOK(8231EF48, "GRenderer [37]")
TRACE_HOOK(8231F580, "GRenderer [39]")
TRACE_HOOK(8231F768, "GRenderer [40]")
TRACE_HOOK(8231F810, "GRenderer [41]")
TRACE_HOOK(8231B7C8, "GRenderer [45]")

// Screen status (Flash variable m_status) as the front end reads it.
extern "C" PPC_FUNC(__imp__sub_83122760);
PPC_FUNC(sub_83122760)
{
    uint32_t self = ctx.r3.u32;
    __imp__sub_83122760(ctx, base);

    static std::atomic<int> calls{ 0 };
    static uint32_t last = 0xFFFFFFFF;
    int n = calls++;
    if (g_traceHooks && (ctx.r3.u32 != last || n % 300 == 0))
    {
        last = ctx.r3.u32;
        Log("hook: screen %08X m_status = %d (call %d)", self, int(ctx.r3.u32), n);
        uint32_t bundle = Load<uint32_t>(0x82000748);
        Log("hook:   GetTickCount source: slot 82000748 -> %08X, +16 = %u, +0 = %llu", bundle,
            bundle ? Load<uint32_t>(bundle + 16) : 0, bundle ? (unsigned long long)Load<uint64_t>(bundle) : 0ull);
        // Probe other ActionScript values through the player's GetVariable
        // (vtable slot 39: player, path, variant*) -> variant { u32 type; ...; f64 number @8 }.
        uint32_t player = Load<uint32_t>(self + 120);
        static const char* paths[] = { "Legal_Text._alpha", "caurina.transitions.Tweener._currentTime",
                                       "caurina.transitions.Tweener._currentTimeFrame", "caurina.transitions.Tweener._tweenList.length",
                                       "_root._currentframe", "_root._totalframes" };
        for (const char* path : paths)
        {
            PPCContext c = ctx;
            uint32_t frame = (ctx.r1.u32 - 0x800) & ~0xFu;
            uint32_t str = frame + 0x400, var = frame + 0x500;
            memcpy(GuestPtr(str), path, strlen(path) + 1);
            memset(GuestPtr(var), 0, 16);
            Store<uint32_t>(var, 5);
            c.r1.u64 = frame;
            c.r3.u64 = player;
            c.r4.u64 = str;
            c.r5.u64 = var;
            GuestCall(c, 0x82313BC0);
            double num;
            uint64_t bits = Load<uint64_t>(var + 8);
            memcpy(&num, &bits, 8);
            Log("hook:   %s -> ok %u, type %u, words %08X %08X, number %g", path, c.r3.u32 & 0xFF,
                Load<uint32_t>(var), Load<uint32_t>(var + 8), Load<uint32_t>(var + 12), num);
        }
    }
}

// Sound code that crashes after START: log its arguments and the object it looks up.
extern "C" PPC_FUNC(__imp__sub_82C74C08);
PPC_FUNC(sub_82C74C08)
{
    if (g_traceHooks)
    {
        uint32_t sys = Load<uint32_t>(0x83B21C14);
        uint32_t vt = sys ? Load<uint32_t>(sys) : 0;
        Log("hook: sound 82C74C08(r3=%08X r4=%08X%s r5=%08X r6=%08X r7=%08X%s) from %08llX; system %08X vtable %08X [23]=%08X",
            ctx.r3.u32, ctx.r4.u32, MaybeString(ctx.r4.u32).c_str(), ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
            MaybeString(ctx.r7.u32).c_str(), ctx.lr, sys, vt, vt ? Load<uint32_t>(vt + 92) : 0);
        if (sys)
        {
            uint32_t slots = Load<uint32_t>(sys + 532), salts = sys + 620;
            uint32_t index = ctx.r7.u32 & 0xFFFF;
            uint32_t entry = slots ? Load<uint32_t>(slots + index * 4) : 0;
            Log("hook:   live vtable 820CFCB0: [5]=%08X [6]=%08X [7]=%08X (image: 82C7EF50 82C7FC40 82C7A338)",
                Load<uint32_t>(0x820CFCB0 + 20), Load<uint32_t>(0x820CFCB0 + 24), Load<uint32_t>(0x820CFCB0 + 28));
            Log("hook:   slots %08X (size word %08X), entry[%u] = %08X (vtable %08X), salt[%u] = %u; entries 4090..4095: %08X %08X %08X %08X %08X %08X",
                slots, Load<uint32_t>(sys + 536), index, entry, entry >= 0x40000000 ? Load<uint32_t>(entry) : 0, index,
                Load<uint16_t>(salts + index * 4), Load<uint32_t>(slots + 4090 * 4), Load<uint32_t>(slots + 4091 * 4),
                Load<uint32_t>(slots + 4092 * 4), Load<uint32_t>(slots + 4093 * 4), Load<uint32_t>(slots + 4094 * 4), Load<uint32_t>(slots + 4095 * 4));
        }
    }
    __imp__sub_82C74C08(ctx, base);
}

// FMOD event callback dispatch (event+124 = callback, +128 = userdata, +24 = handle).
extern "C" PPC_FUNC(__imp__sub_839C0458);
PPC_FUNC(sub_839C0458)
{
    if (g_traceHooks)
    {
        uint32_t ev = ctx.r3.u32;
        static std::atomic<int> n{ 0 };
        if (n++ < 40)
            Log("hook: fmod event %08X callback type %u: cb %08X userdata %08X handle %08X flags %08X from %08llX",
                ev, ctx.r4.u32, Load<uint32_t>(ev + 124), Load<uint32_t>(ev + 128), Load<uint32_t>(ev + 24), Load<uint32_t>(ev + 104), ctx.lr);
    }
    __imp__sub_839C0458(ctx, base);
}

// Sound start: the object at this+84 provides the ID used as FMOD callback userdata.
extern "C" PPC_FUNC(__imp__sub_82C792B8);
PPC_FUNC(sub_82C792B8)
{
    if (g_traceHooks)
    {
        uint32_t self = ctx.r3.u32, obj = Load<uint32_t>(self + 84);
        uint32_t vt = obj >= 0x40000000 ? Load<uint32_t>(obj) : 0;
        static std::atomic<int> n{ 0 };
        if (n++ < 10)
            Log("hook: sound start this %08X obj+84 %08X vtable %08X [28]=%08X; this words %08X %08X %08X %08X %08X %08X",
                self, obj, vt, vt ? Load<uint32_t>(vt + 112) : 0, Load<uint32_t>(self), Load<uint32_t>(self + 4),
                Load<uint32_t>(self + 8), Load<uint32_t>(self + 12), Load<uint32_t>(self + 16), Load<uint32_t>(self + 20));
    }
    __imp__sub_82C792B8(ctx, base);
}

// D3D GPU interrupt callback: log the pending GPU->CPU callback counter (device+11012).
extern "C" PPC_FUNC(__imp__sub_836DDF78);
PPC_FUNC(sub_836DDF78)
{
    uint32_t source = ctx.r3.u32, dev = ctx.r4.u32;
    uint32_t before = Load<uint32_t>(dev + 11012);
    uint32_t shared = Load<uint32_t>(dev + 10900);
    __imp__sub_836DDF78(ctx, base);
    if (g_traceHooks && source == 1)
    {
        static std::atomic<int> n{ 0 };
        static uint32_t lastPending = 0xFFFFFFFF;
        uint32_t after = Load<uint32_t>(dev + 11012);
        int k = n++;
        if (k < 10 || after != lastPending || k % 500 == 0)
        {
            lastPending = after;
            Log("hook: d3d interrupt #%d: pending %u -> %u, shared %08X cb %08X(%08X) mask %08X",
                k, before, after, shared, Load<uint32_t>(shared + 16), Load<uint32_t>(shared + 20), Load<uint32_t>(shared));
        }
    }
}

// D3D "block until the GPU passes fence r4": log target, current and completed fences.
extern "C" PPC_FUNC(__imp__sub_836DE3A0);
PPC_FUNC(sub_836DE3A0)
{
    if (g_traceHooks)
    {
        uint32_t dev = ctx.r3.u32, target = ctx.r4.u32;
        uint32_t fenceAddr = Load<uint32_t>(dev + 10896);
        static std::atomic<int> n{ 0 };
        int k = n++;
        if (k < 5 || k % 2000 == 0)
            Log("hook: d3d block on fence #%d: target %08X, issued %08X, completed %08X (at %08X), flags 13408=%08X from %08llX",
                k, target, Load<uint32_t>(dev + 10908), fenceAddr ? Load<uint32_t>(fenceAddr) : 0, fenceAddr,
                Load<uint32_t>(dev + 13408), ctx.lr);
    }
    __imp__sub_836DE3A0(ctx, base);
}
// Front end: set up the loading screen (this, playVideo, ?). Level videos don't
// decode yet, and the load waits for the video to end, so start it without one.
extern "C" PPC_FUNC(__imp__sub_8310E890);
PPC_FUNC(sub_8310E890)
{
    static const bool keepVideo = getenv("LEVEL_VIDEOS") != nullptr;
    if (!keepVideo && ctx.r4.u32)
    {
        Log("video: starting the loading screen without its level video");
        ctx.r4.u64 = 0;
    }
    __imp__sub_8310E890(ctx, base);
}
TRACE_HOOK(8310D4F8, "front end: level video path")

// XMA context lock (the title's XMA library): sub_83A140C0 spins until
// sub_83A139E8 reports that the hardware isn't on one of the group's contexts
// (register 0x606, "current context", compared with each context's id), and
// resets the decoder after 125 ms. With PROFILE=1, log how often it's called,
// how long it spins, how often it times out, and what the check compared.
#include "platform.h"
namespace
{
    std::atomic<uint64_t> g_xmaLockCalls{ 0 }, g_xmaLockNs{ 0 }, g_xmaLockTimeouts{ 0 }, g_xmaLockMaxNs{ 0 };
    std::atomic<uint64_t> g_xmaCheckCalls{ 0 }, g_xmaCheckBusy{ 0 };
}
extern "C" PPC_FUNC(__imp__sub_83A139E8);
PPC_FUNC(sub_83A139E8)
{
    uint32_t group = ctx.r3.u32;
    __imp__sub_83A139E8(ctx, base);
    if (!prof::Enabled())
        return;
    g_xmaCheckCalls++;
    if (ctx.r3.u32 == 0)
    {
        static std::atomic<int> logged{ 0 };
        if (g_xmaCheckBusy++ < 1000000 && logged++ < 12)
        {
            uint32_t raw = *(uint32_t*)GuestPtr(0x7FEA1818);
            uint32_t n = Load<uint32_t>(group), arr = Load<uint32_t>(group + 8);
            std::string ids;
            for (uint32_t i = 0; i < n && i < 8; i++)
            {
                char b[12];
                snprintf(b, sizeof(b), " %04X", Load<uint16_t>(arr + i * 96 + 80));
                ids += b;
            }
            Log("xma lock: busy; reg 0x606 raw %08X (title reads %08X), group %08X has %u contexts, ids:%s", raw, raw, group, n, ids.c_str());
        }
    }
}
extern "C" PPC_FUNC(__imp__sub_83A140C0);
PPC_FUNC(sub_83A140C0)
{
    XmaPump(); // let the decoder catch up before the title looks at its contexts
    if (!prof::Enabled())
    {
        __imp__sub_83A140C0(ctx, base);
        return;
    }
    uint64_t t0 = PlatformCounterNs();
    __imp__sub_83A140C0(ctx, base);
    uint64_t dt = PlatformCounterNs() - t0;
    g_xmaLockCalls++;
    g_xmaLockNs += dt;
    if (ctx.r3.u32) g_xmaLockTimeouts++;
    if (dt > g_xmaLockMaxNs) g_xmaLockMaxNs = dt;
    static std::atomic<uint64_t> lastLog{ 0 };
    uint64_t now = PlatformCounterNs(), prev = lastLog;
    if (now - prev > 10000000000ull && lastLog.compare_exchange_strong(prev, now))
        Log("xma lock: %llu locks (%.0f ms spinning, longest %.1f ms, %llu timeouts); checks %llu, busy %llu",
            (unsigned long long)g_xmaLockCalls.exchange(0), g_xmaLockNs.exchange(0) / 1e6, g_xmaLockMaxNs.exchange(0) / 1e6,
            (unsigned long long)g_xmaLockTimeouts.exchange(0), (unsigned long long)g_xmaCheckCalls.exchange(0),
            (unsigned long long)g_xmaCheckBusy.exchange(0));
}

// ---------------------------------------------------------------------------
// The title's warnings. The retail build formats CryWarning / GameWarning text
// (ISystem::WarningV, vtable slot 26 of the system object) but never shows it.
// GAME_WARNINGS=1 copies each message into the runtime log, which names the
// cause of failures such as a checkpoint the game refuses to load.
// ---------------------------------------------------------------------------
namespace
{
    const bool g_gameWarnings = getenv("GAME_WARNINGS") != nullptr;

    // printf for a guest format string and va_list (8-byte big-endian slots;
    // integers in the low word, doubles as their 64-bit pattern).
    std::string GuestFormat(uint32_t fmt, uint32_t args)
    {
        std::string f = LoadGuestString(fmt, 1024), out;
        auto next32 = [&]() { uint32_t v = Load<uint32_t>(args + 4); args += 8; return v; };
        auto next64 = [&]() { uint64_t v = Load<uint64_t>(args); args += 8; return v; };
        for (size_t i = 0; i < f.size(); i++)
        {
            if (f[i] != '%') { out += f[i]; continue; }
            size_t start = i++;
            std::string spec = "%";
            while (i < f.size() && strchr("-+ #0", f[i])) spec += f[i++];
            while (i < f.size() && (isdigit((unsigned char)f[i]) || f[i] == '.' || f[i] == '*'))
            {
                if (f[i] == '*') spec += std::to_string(int32_t(next32())); else spec += f[i];
                i++;
            }
            bool wide = false;
            while (i < f.size() && strchr("hlLqjzt", f[i])) { if (f[i] == 'l' || f[i] == 'L' || f[i] == 'q') wide = true; i++; }
            if (i + 2 < f.size() && f.compare(i, 3, "I64") == 0) { wide = true; i += 3; }
            if (i >= f.size()) { out += f.substr(start); break; }
            char c = f[i], buf[512];
            switch (c)
            {
            case '%': out += '%'; continue;
            case 's': { uint32_t p = next32(); snprintf(buf, sizeof(buf), (spec + "s").c_str(), p ? LoadGuestString(p, 400).c_str() : "(null)"); break; }
            case 'c': snprintf(buf, sizeof(buf), (spec + "c").c_str(), int(next32() & 0xFF)); break;
            case 'd': case 'i':
                if (wide) snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)next64());
                else snprintf(buf, sizeof(buf), (spec + "d").c_str(), int32_t(next32()));
                break;
            case 'u': case 'x': case 'X': case 'o':
                if (wide) snprintf(buf, sizeof(buf), (spec + "ll" + c).c_str(), (unsigned long long)next64());
                else snprintf(buf, sizeof(buf), (spec + c).c_str(), next32());
                break;
            case 'p': snprintf(buf, sizeof(buf), "%08X", next32()); break;
            case 'f': case 'F': case 'g': case 'G': case 'e': case 'E':
            {
                uint64_t bits = next64();
                double d;
                memcpy(&d, &bits, 8);
                snprintf(buf, sizeof(buf), (spec + c).c_str(), d);
                break;
            }
            default: snprintf(buf, sizeof(buf), "%%%c", c); break;
            }
            out += buf;
        }
        return out;
    }
}

// ISystem::WarningV(this, module, severity, flags, file, format, va_list)
extern "C" PPC_FUNC(__imp__sub_822A87C8);
PPC_FUNC(sub_822A87C8)
{
    if (g_gameWarnings && ctx.r8.u32)
    {
        static std::atomic<int> count{ 0 };
        if (count++ < 5000)
        {
            std::string file = ctx.r7.u32 ? " [" + LoadGuestString(ctx.r7.u32, 200) + "]" : std::string();
            Log("game warning (module %u, severity %u)%s: %s", ctx.r4.u32, ctx.r5.u32, file.c_str(),
                GuestFormat(ctx.r8.u32, ctx.r9.u32).c_str());
        }
    }
    __imp__sub_822A87C8(ctx, base);
}

// XMLCPB reader ReadBinaryFile(this, fileName) -> true when the file was read.
// A checkpoint (*.CSF) it can't read makes LoadGame fail, and Resume Game then
// sits on the loading screen forever; the page (saves.js) asks
// crysis_checkpoint_failures() and offers a way out.
namespace
{
    std::atomic<int> g_checkpointFailures{ 0 };
}
extern "C" PPC_FUNC(__imp__sub_8265BB30);
PPC_FUNC(sub_8265BB30)
{
    std::string name = LoadGuestString(ctx.r4.u32, 260);
    __imp__sub_8265BB30(ctx, base);
    std::string ext = name.size() >= 4 ? name.substr(name.size() - 4) : std::string();
    for (char& c : ext) c = char(tolower((unsigned char)c));
    bool checkpoint = ext == ".csf";
    if ((ctx.r3.u32 & 0xFF) == 0 && checkpoint)
    {
        g_checkpointFailures++;
        Log("save: the checkpoint %s can't be loaded (damaged or incomplete)", name.c_str());
    }
}
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern "C" EMSCRIPTEN_KEEPALIVE int crysis_checkpoint_failures() { return g_checkpointFailures.load(); }
#endif

// XMLCPB reader: ReadBytes(this, ISaveReader** reader, void* dest, u32 size).
// SAVE_TRACE=1 logs the first reads of each load (the footer is 48 bytes,
// signature 'PBX0') and the reader's vtable.
extern "C" PPC_FUNC(__imp__sub_8265AF28);
PPC_FUNC(sub_8265AF28)
{
    static const bool trace = getenv("SAVE_TRACE") != nullptr;
    uint32_t readerPtr = ctx.r4.u32, dest = ctx.r5.u32, size = ctx.r6.u32;
    __imp__sub_8265AF28(ctx, base);
    static std::atomic<int> logged{ 0 };
    if (trace && logged++ < 40)
    {
        uint32_t reader = Load<uint32_t>(readerPtr), vt = reader ? Load<uint32_t>(reader) : 0;
        std::string bytes;
        for (uint32_t i = 0; i < std::min<uint32_t>(size, 48); i++)
        {
            char b[4];
            snprintf(b, sizeof(b), "%02X", Load<uint8_t>(dest + i));
            bytes += b;
        }
        Log("save trace: read %u bytes -> %s (reader %08X vtable %08X: %08X %08X %08X %08X %08X %08X %08X %08X %08X)", size,
            bytes.c_str(), reader, vt, vt ? Load<uint32_t>(vt) : 0, vt ? Load<uint32_t>(vt + 4) : 0, vt ? Load<uint32_t>(vt + 8) : 0,
            vt ? Load<uint32_t>(vt + 12) : 0, vt ? Load<uint32_t>(vt + 16) : 0, vt ? Load<uint32_t>(vt + 20) : 0,
            vt ? Load<uint32_t>(vt + 24) : 0, vt ? Load<uint32_t>(vt + 28) : 0, vt ? Load<uint32_t>(vt + 32) : 0);
        // The reader decrypted the whole file into memory (inner object at +12:
        // vtable +4 = size, +16 = data). Save that plaintext for inspection.
        uint32_t inner = reader ? Load<uint32_t>(reader + 12) : 0;
        if (size == 48 && inner)
        {
            uint32_t ivt = Load<uint32_t>(inner);
            PPCContext c = ctx;
            c.r3.u64 = inner;
            GuestCall(c, Load<uint32_t>(ivt + 4));
            uint32_t total = c.r3.u32;
            c = ctx;
            c.r3.u64 = inner;
            GuestCall(c, Load<uint32_t>(ivt + 16));
            uint32_t data = c.r3.u32;
            static std::atomic<int> n{ 0 };
            char path[64];
            snprintf(path, sizeof(path), "/tmp/save_plain_%d.bin", n++);
            if (FILE* f = fopen(path, "wb"))
            {
                fwrite(GuestPtr(data), 1, total, f);
                fclose(f);
            }
            Log("save trace: plaintext %u bytes at %08X -> %s", total, data, path);
        }
    }
}

// Module._crysis_debug_save() (debugging saved games): the title saves a
// checkpoint at its next frame. CCryAction's frame update (sub_827E59D0) and
// SaveGame (sub_827E4158) are methods of the same object, so the hook calls
// SaveGame(this, path, quick = false, forceImmediate = true,
// reason = flow graph (1), ignoreDelay = true, checkpointName = null) first.
namespace
{
    std::atomic<int> g_debugSave{ 0 };
}
extern "C" PPC_FUNC(__imp__sub_827E59D0);
PPC_FUNC(sub_827E59D0)
{
    if (g_debugSave.exchange(0))
    {
        static uint32_t path = 0;
        static const char text[] = "%USER%/Profiles/SaveGames/Nomad.CSF";
        if (!path)
        {
            path = KernelAlloc(sizeof(text));
            memcpy(GuestPtr(path), text, sizeof(text));
        }
        PPCContext c = ctx;
        c.r4.u64 = path;
        c.r5.u64 = 0;
        c.r6.u64 = 1;
        c.r7.u64 = 1;
        c.r8.u64 = 1;
        c.r9.u64 = 0;
        Log("debug save: SaveGame(%s)", text);
        GuestCall(c, 0x827E4158);
        Log("debug save: SaveGame returned %u", c.r3.u32 & 0xFF);
    }
    __imp__sub_827E59D0(ctx, base);
}
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_debug_save() { g_debugSave = 1; }
#endif

// ---------------------------------------------------------------------------
// Checkpoint commits. A checkpoint is saved in two steps:
// 1. SaveGame (main thread) serializes the game into 32 KB blocks; the
//    ZLibCompressor thread (CCompressorThread, sub_82658C30) deflates each one
//    and appends it to an in-memory file (the platform layer's save writer,
//    vtable 82017E40), then appends the 48-byte table footer ('PBX0') and closes
//    the writer. SaveGame returns without waiting for that.
// 2. The platform save thread (sub_8233BF68) runs a commit pass (sub_8233A2D8)
//    every 100 ms while a commit is pending: it copies every changed in-memory
//    file to the storage device and clears the pending flag.
// On the console the compressor finishes long before the next commit pass. In
// the browser it can lag several blocks behind, so a pass could copy a
// checkpoint without its last blocks and footer. The storage then held a file
// the game can't load until the compressor finished and another pass ran
// (seconds, or much longer when the threads are starved); a reload in that
// window, or the copy saves.js kept of it, left Resume Game on the loading
// screen forever ("XMLCPB ERROR: file type signature not correct. Savegame File
// corrupted!"). So commit passes wait while a compressor stream is open.
// ---------------------------------------------------------------------------
namespace
{
    std::mutex g_streamMutex;
    std::unordered_map<uint32_t, uint64_t> g_openStreams; // save writer -> start (ms)
    std::unordered_map<uint32_t, uint64_t> g_writerBytes;  // SAVE_TRACE: bytes appended per writer
    const bool g_saveTrace = getenv("SAVE_TRACE") != nullptr;
    // SAVE_SLOW_APPEND_MS=<ms> (testing): the compressor thread pauses before
    // each append, which recreates the race above on demand.
    const uint32_t g_slowAppendMs = getenv("SAVE_SLOW_APPEND_MS") ? uint32_t(atoi(getenv("SAVE_SLOW_APPEND_MS"))) : 0;
    constexpr uint64_t kStreamTimeoutMs = 60000; // a stream never closed stops holding commits back
    // SAVE_COMMIT_WAIT=0 (testing): commit passes don't wait for open streams.
    const bool g_commitWait = !getenv("SAVE_COMMIT_WAIT") || atoi(getenv("SAVE_COMMIT_WAIT")) != 0;

    uint64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Writers that a compressor stream still appends to (stale ones dropped).
    size_t OpenStreams(std::string* list = nullptr)
    {
        std::lock_guard lock(g_streamMutex);
        uint64_t now = NowMs();
        for (auto it = g_openStreams.begin(); it != g_openStreams.end();)
        {
            if (now - it->second > kStreamTimeoutMs)
            {
                Log("save: compressor stream to writer %08X still open after %llu s; not holding commits back any longer",
                    it->first, (unsigned long long)(kStreamTimeoutMs / 1000));
                it = g_openStreams.erase(it);
                continue;
            }
            if (list)
            {
                char b[48];
                snprintf(b, sizeof(b), " %08X (%llu bytes)", it->first, (unsigned long long)g_writerBytes[it->first]);
                *list += b;
            }
            ++it;
        }
        return g_openStreams.size();
    }
}

// CCompressorThread: set up a stream (this; its file record lands at +48, with
// the save writer at +8) and queue it to the thread. Registered once this
// returns: the thread only starts appending after the main thread submits the
// first block, which comes later.
extern "C" PPC_FUNC(__imp__sub_82658A88);
PPC_FUNC(sub_82658A88)
{
    uint32_t self = ctx.r3.u32;
    __imp__sub_82658A88(ctx, base);
    uint32_t file = Load<uint32_t>(self + 48), writer = file ? Load<uint32_t>(file + 8) : 0;
    if (!writer)
        return;
    {
        std::lock_guard lock(g_streamMutex);
        g_openStreams[writer] = NowMs();
        g_writerBytes[writer] = 0;
    }
    if (g_saveTrace)
        Log("save trace: compressor stream to writer %08X opened", writer);
}

// Save writer AppendBytes(this, data, size).
extern "C" PPC_FUNC(__imp__sub_82341BA0);
PPC_FUNC(sub_82341BA0)
{
    if (g_saveTrace || g_slowAppendMs)
    {
        bool stream;
        {
            std::lock_guard lock(g_streamMutex);
            stream = g_openStreams.count(ctx.r3.u32) != 0;
            if (stream)
                g_writerBytes[ctx.r3.u32] += ctx.r5.u32;
        }
        if (stream && g_slowAppendMs)
            PlatformSleepMs(g_slowAppendMs);
    }
    __imp__sub_82341BA0(ctx, base);
}

// Save writer Close(this): from the compressor's finish path (after the footer,
// return address 82658F18) it ends that stream.
extern "C" PPC_FUNC(__imp__sub_82335C08);
PPC_FUNC(sub_82335C08)
{
    if (ctx.lr == 0x82658F18)
    {
        uint64_t bytes = 0;
        {
            std::lock_guard lock(g_streamMutex);
            g_openStreams.erase(ctx.r3.u32);
            bytes = g_writerBytes[ctx.r3.u32];
            g_writerBytes.erase(ctx.r3.u32);
        }
        if (g_saveTrace)
            Log("save trace: compressor stream to writer %08X closed (%llu bytes)", ctx.r3.u32, (unsigned long long)bytes);
    }
    __imp__sub_82335C08(ctx, base);
}

// Save thread commit pass(this) -> 1 when nothing is left pending (the thread
// then waits for the next request), 0 to run again after 100 ms.
extern "C" PPC_FUNC(__imp__sub_8233A2D8);
PPC_FUNC(sub_8233A2D8)
{
    static bool holding = false; // (only the save thread runs commit passes)
    uint32_t self = ctx.r3.u32;
    bool pending = Load<uint8_t>(self + 1868 + 621) != 0; // user 0's commit request
    std::string list;
    if (pending && OpenStreams(g_saveTrace ? &list : nullptr) && g_commitWait)
    {
        if (!holding || g_saveTrace)
            Log("save: commit held back while a checkpoint is still being compressed%s", list.c_str());
        holding = true;
        ctx.r3.u64 = 0;
        return;
    }
    holding = false;
    __imp__sub_8233A2D8(ctx, base);
    if (g_saveTrace && pending)
        Log("save trace: commit pass ->%s %u, user 0 still pending %u", list.empty() ? "" : " (streams open!)", ctx.r3.u32,
            Load<uint8_t>(self + 1868 + 621));
}

// D3D's wait for the GPU (render thread): one pass of the title's polling loop,
// 32 short delay instructions and then a check of the GPU's progress, called
// over and over until the GPU (the emulated command processor) catches up.
// Spinning there took a whole core; on phones, with few fast cores, that core
// was missing for the threads doing the work. Yield first: PlatformYield waits
// 50 us where the browser times short waits well (phones), and does nothing
// on Windows, so desktop timing is unchanged.
extern "C" PPC_FUNC(__imp__sub_836E0B60);
PPC_FUNC(sub_836E0B60)
{
    PlatformYield();
    __imp__sub_836E0B60(ctx, base);
}

// D3D's kick-off (render thread): it writes the ring buffer's write pointer
// (GPU register CP_RB_WPTR) for the command processor, which now waits for
// that instead of polling.
void GpuKickedOff();
extern "C" PPC_FUNC(__imp__sub_836DE880);
PPC_FUNC(sub_836DE880)
{
    __imp__sub_836DE880(ctx, base);
    GpuKickedOff();
}

// The render thread's handshake with the main thread (CryEngine's
// SRenderThread at r3: +32 m_nFlush, 1 while a frame's commands wait to be
// processed; +16 the quit flag). Both sides polled the flag: the render thread
// with Sleep(0), which does nothing in the browser, so it kept a core running
// flat out (on a 6-core PC, often the core the main thread shared, the thread
// that limits the frame rate); the main thread with Sleep(1), each a trip
// through the sleep timer. They now wait on the flag's word and wake each
// other where the title changes it. The originals still run afterwards and
// find the flag as they expect.
//   sub_828CBA58 main thread: hands over a frame, sets the flag at its end
//   sub_828CB7C0 main thread: waits for the flag to clear (FlushAndWait sets it just before)
//   sub_828CB830 render thread: waits for the flag
//   sub_828D01A0 render thread: processes the frame, clears the flag at its end
namespace
{
    volatile uint32_t* FlushWord(uint32_t self) { return (volatile uint32_t*)GuestPtr(self + 32); }
    bool RenderQuit(uint32_t self) { return Load<uint8_t>(self + 16) != 0; }
    // While the renderer shows a loading screen (flag 0x1000 at +18520 of the
    // renderer, 0x83AE42B0), the render thread's wait also redraws it on a
    // timer: leave that wait as it was.
    bool LoadingScreenWait()
    {
        uint32_t r = Load<uint32_t>(0x83AE42B0);
        return r && (Load<uint32_t>(r + 18520) & 0x1000) && !Load<uint8_t>(r + 19580);
    }
}
extern "C" PPC_FUNC(__imp__sub_828CBA58);
PPC_FUNC(sub_828CBA58)
{
    uint32_t self = ctx.r3.u32;
    __imp__sub_828CBA58(ctx, base);
    PlatformWakeWord(FlushWord(self));
}
extern "C" PPC_FUNC(__imp__sub_828CB7C0);
PPC_FUNC(sub_828CB7C0)
{
    uint32_t self = ctx.r3.u32;
    PlatformWakeWord(FlushWord(self));
    for (uint32_t v; (v = *FlushWord(self)) != 0 && !RenderQuit(self);)
        PlatformWaitWord(FlushWord(self), v, 2.0);
    __imp__sub_828CB7C0(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_828CB830);
PPC_FUNC(sub_828CB830)
{
    uint32_t self = ctx.r3.u32;
    while (*FlushWord(self) == 0 && !RenderQuit(self) && !LoadingScreenWait())
        PlatformWaitWord(FlushWord(self), 0, 2.0);
    __imp__sub_828CB830(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_828D01A0);
PPC_FUNC(sub_828D01A0)
{
    uint32_t self = ctx.r3.u32;
    __imp__sub_828D01A0(ctx, base);
    PlatformWakeWord(FlushWord(self));
}

// The main thread's occlusion test (r3: the coverage buffer) first waits for
// the job that rasterizes the buffer (+84 nonzero while it runs): up to 20
// times Sleep(1). In the browser each of those sleeps took ~1-6 ms, the main
// thread's time, and the main thread limits the frame rate. Watch the flag
// for up to 20 ms instead (yielding); the original then finds it clear, or
// waits and gives up as before.
extern "C" PPC_FUNC(__imp__sub_82A31BC8);
PPC_FUNC(sub_82A31BC8)
{
    uint32_t busy = ctx.r3.u32 + 84;
    if (Load<uint32_t>(busy) != 0)
    {
        uint64_t until = PlatformCounterNs() + 20000000ull;
        while (Load<uint32_t>(busy) != 0 && PlatformCounterNs() < until)
            PlatformYield();
    }
    __imp__sub_82A31BC8(ctx, base);
}

// The title's frame limiter (render thread, sys_MaxFPS): it waits until
// last + int(1000 / (fps + 0.5)) * 100 timer ticks (1/100000 s) by reading the
// game timer in a busy loop, so a capped frame rate kept a core running flat
// out (phones: battery and heat). Sleep most of the wait instead; the original
// then spins only the last 2 ms, so the frame timing stays the game's.
//   0x83C42800 gEnv (pTimer at +20), 0x83AD7358 the sys_MaxFPS ICVar,
//   0x83AD735C flags (bit 1: 0x83AD7350, the last frame's time, is set).
extern "C" PPC_FUNC(__imp__sub_82883F80);
PPC_FUNC(sub_82883F80)
{
    uint32_t env = Load<uint32_t>(0x83C42800), cvar = Load<uint32_t>(0x83AD7358);
    uint32_t timer = env ? Load<uint32_t>(env + 20) : 0;
    if (timer && cvar && (Load<uint32_t>(0x83AD735C) & 2))
    {
        PPCContext c = ctx;
        c.r3.u32 = cvar;
        GuestCall(c, Load<uint32_t>(Load<uint32_t>(cvar) + 8)); // ICVar::GetIValue
        int fps = c.r3.s32;
        if (fps > 0)
        {
            c = ctx;
            c.r1.u32 = ctx.r1.u32 - 512; // (scratch frame below the caller's)
            c.r3.u32 = c.r1.u32 + 128;   // CTimeValue result
            c.r4.u32 = timer;
            GuestCall(c, Load<uint32_t>(Load<uint32_t>(timer) + 20)); // ITimer::GetAsyncTime
            int64_t now = Load<int64_t>(c.r3.u32), last = Load<int64_t>(0x83AD7350);
            int64_t left = last + int64_t(int(1000.0f / (float(fps) + 0.5f))) * 100 - now; // ticks of 10 us
            if (left > 300 && left < 100000) // more than 3 ms (and sane: under a second)
                PlatformSleepMsPrecise(uint32_t((left - 200) / 100));
        }
    }
    __imp__sub_82883F80(ctx, base);
}
