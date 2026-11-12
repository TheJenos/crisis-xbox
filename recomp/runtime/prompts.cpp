// Controller-button prompts: which Xbox 360 buttons the game is showing on
// screen right now ("A Select" in menus, "Hold Y Look" on the HUD), so the page
// can show the keys bound to them (controls.js).
//
// Every button icon the title's UI shows is set by a CControllerButton: a C++
// object driving a Flash clip ("<path>.Icons") whose frame is the icon.
//   +60  Flash path, +200 button or action name, +244 action map name
//        (small-buffer strings: data pointer at +8)
//   +272 CFlashVariableObject* for the Icons clip, +280 type (1 action, 2 menu)
// sub_83049F28(this, player) looks the clip up in a Flash player;
// sub_830488F8(this) sets a menu button's icon, sub_83048E60(this) an action
// button's (from the action map); both end in Icons->GotoAndStop(frame)
// (CFlashVariableObject::GotoAndStop, sub_823128D0). Menu buttons are often
// temporary: the object sets the frame and is destroyed, the clip keeps it.
//
// So the icon's frame is caught at GotoAndStop, and whether it is on screen is
// read from Scaleform's own display objects:
// - CFlashVariableObject +12 GFxValue type (display object: 8 in the low
//   bits), +16 its GFxCharacterHandle { refcount, character, name, path };
//   the handle outlives the character, whose pointer is cleared when it dies.
//   One extra reference is taken on each handle seen, so it stays valid.
// - character +0x20 parent, +0x3C/+0x40 alpha multiply/add, +0xA0 flags
//   (bit 0: visible, set by the character's SetVisible, vtable +204).
// An icon counts while its clip and every parent up to the movie root are
// visible and not faded out, and its Flash player was advanced in the last
// 300 ms (CFlashPlayer::Advance, sub_82313218: the title advances the movies it
// shows every frame; menus aren't, during play).
//
// PROMPT_TRACE=1 logs each icon set and some player advances (debugging).
#include "guest.h"
#include "platform.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace
{
    const bool g_promptTrace = getenv("PROMPT_TRACE") != nullptr;

    struct Slot { uint32_t player; uint32_t buttons; };
    std::mutex g_mutex;
    std::unordered_map<uint32_t, Slot> g_slots;           // Icons clip (character handle) -> what it shows
    std::unordered_map<uint32_t, uint32_t> g_buttonPlayer; // CControllerButton -> its Flash player
    std::unordered_map<uint32_t, uint64_t> g_lastRender;   // Flash player -> last Advance (ms)
    thread_local uint32_t t_button = 0; // the CControllerButton setting its icon on this thread
    thread_local int t_frame = -1;      // ... and the frame it chose

    uint64_t NowMs() { return PlatformCounterNs() / 1000000; }
    bool IsGuest(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000; }

    // Icon frame -> pad controls (1 << InputControl number, input.h). From the
    // name tables in sub_830488F8 / sub_83048E60.
    uint32_t FrameButtons(int frame)
    {
        constexpr uint32_t dpad = (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13);
        switch (frame)
        {
        case 1: return 1u << 0;   // A
        case 2: return 1u << 1;   // B
        case 3: return 1u << 2;   // X
        case 4: return 1u << 3;   // Y
        case 5: return 1u << 7;   // START
        case 6: return 1u << 6;   // BACK
        case 7: return 0xFu << 16; // left stick
        case 8: return 0xFu << 20; // right stick
        case 9: return 1u << 4;   // LB
        case 10: return 1u << 5;  // RB
        case 11: return 1u << 14; // LT
        case 12: return 1u << 15; // RT
        case 13: return 1u << 8;  // left stick click
        case 14: return 1u << 9;  // right stick click
        case 15: return (1u << 12) | (1u << 13); // d-pad left/right
        case 16: return (1u << 10) | (1u << 11); // d-pad up/down
        case 17: return dpad;
        case 18: return 1u << 10; // d-pad up
        case 19: return 1u << 11; // d-pad down
        case 20: return 1u << 12; // d-pad left
        case 21: return 1u << 13; // d-pad right
        default: return 0;        // (22-25: other control schemes)
        }
    }

    std::string GuestStr(uint32_t stringObject)
    {
        uint32_t data = Load<uint32_t>(stringObject + 8);
        return IsGuest(data) ? LoadGuestString(data, 160) : std::string("?");
    }

    // The button `self` just set its Icons clip to `frame`.
    void Register(uint32_t self, int frame)
    {
        uint32_t icons = Load<uint32_t>(self + 272);
        if (!IsGuest(icons) || (Load<uint32_t>(icons + 12) & 0x8F) != 8) // GFxValue: display object
            return;
        uint32_t handle = Load<uint32_t>(icons + 16);
        if (!IsGuest(handle))
            return;
        std::lock_guard lock(g_mutex);
        auto it = g_slots.find(handle);
        if (it == g_slots.end())
        {
            Store<uint32_t>(handle, Load<uint32_t>(handle) + 1); // keep the handle (a weak pointer to the clip)
            it = g_slots.emplace(handle, Slot{}).first;
        }
        auto p = g_buttonPlayer.find(self);
        it->second = { p != g_buttonPlayer.end() ? p->second : 0, FrameButtons(frame) };
        if (g_promptTrace)
            Log("prompt: '%s' (%s) -> frame %d, buttons %05X, clip %s, player %08X", GuestStr(self + 200).c_str(),
                Load<uint32_t>(self + 280) == 1 ? "action" : "menu", frame, it->second.buttons, GuestStr(self + 60).c_str(),
                it->second.player);
    }

    // Whether the character and all its parents are visible and not faded out.
    bool Shown(uint32_t ch)
    {
        for (int depth = 0; depth < 64; depth++)
        {
            if (!(Load<uint32_t>(ch + 0xA0) & 1))
                return false;
            if (Load<float>(ch + 0x3C) < 0.02f && Load<float>(ch + 0x40) < 4.0f)
                return false;
            uint32_t parent = Load<uint32_t>(ch + 0x20);
            if (!parent)
                return true; // the movie's root
            if (!IsGuest(parent))
                return false;
            ch = parent;
        }
        return false;
    }

    template<class F> void SettingIcon(PPCContext& ctx, F body)
    {
        uint32_t self = ctx.r3.u32, outer = t_button;
        int outerFrame = t_frame;
        t_button = self;
        t_frame = -1;
        body();
        if (t_frame >= 0)
            Register(self, t_frame);
        t_button = outer;
        t_frame = outerFrame;
    }
}

// CControllerButton: look up the Icons clip in a Flash player (this, player).
extern "C" PPC_FUNC(__imp__sub_83049F28);
PPC_FUNC(sub_83049F28)
{
    {
        std::lock_guard lock(g_mutex);
        g_buttonPlayer[ctx.r3.u32] = ctx.r4.u32;
    }
    __imp__sub_83049F28(ctx, base);
}

// CControllerButton: set a menu button's icon from its name.
extern "C" PPC_FUNC(__imp__sub_830488F8);
PPC_FUNC(sub_830488F8) { SettingIcon(ctx, [&] { __imp__sub_830488F8(ctx, base); }); }

// CControllerButton::UpdateActionButton: set an action button's icon from the action map.
extern "C" PPC_FUNC(__imp__sub_83048E60);
PPC_FUNC(sub_83048E60) { SettingIcon(ctx, [&] { __imp__sub_83048E60(ctx, base); }); }

// CControllerButton destructor.
extern "C" PPC_FUNC(__imp__sub_83049D98);
PPC_FUNC(sub_83049D98)
{
    {
        std::lock_guard lock(g_mutex);
        g_buttonPlayer.erase(ctx.r3.u32);
    }
    __imp__sub_83049D98(ctx, base);
}

// CFlashVariableObject::GotoAndStop(this, frame): the frame a button picked.
extern "C" PPC_FUNC(__imp__sub_823128D0);
PPC_FUNC(sub_823128D0)
{
    if (t_button && ctx.r3.u32 == Load<uint32_t>(t_button + 272))
        t_frame = ctx.r4.s32;
    __imp__sub_823128D0(ctx, base);
}

// CFlashPlayer::Advance(this, dt): the title advances the movies it shows,
// every frame (menus only while they are up).
extern "C" PPC_FUNC(__imp__sub_82313218);
PPC_FUNC(sub_82313218)
{
    uint32_t player = ctx.r3.u32;
    {
        std::lock_guard lock(g_mutex);
        g_lastRender[player] = NowMs();
    }
    static const bool traceHooks = getenv("TRACE_HOOKS") != nullptr;
    static std::atomic<int> calls{ 0 };
    int n = calls++;
    if ((traceHooks && (n < 10 || n % 300 == 0)) || (g_promptTrace && n % 600 == 0))
        Log("hook: flash Advance #%d player %08X dt %f from %08llX", n, player, ctx.f1.f64, ctx.lr);
    __imp__sub_82313218(ctx, base);
}

#ifdef __EMSCRIPTEN__
// Module._crysis_prompt_buttons(): the pad controls whose icons are on screen
// now, as a mask (1 << InputControl number). Polled by the page (controls.js).
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t crysis_prompt_buttons()
{
    std::lock_guard lock(g_mutex);
    uint64_t now = NowMs();
    uint32_t mask = 0;
    for (auto it = g_slots.begin(); it != g_slots.end();)
    {
        uint32_t ch = Load<uint32_t>(it->first + 4);
        if (!IsGuest(ch))
        {
            it = g_slots.erase(it); // the clip is gone (its handle keeps our reference)
            continue;
        }
        const Slot& s = it->second;
        ++it;
        if (!s.buttons)
            continue;
        auto r = s.player ? g_lastRender.find(s.player) : g_lastRender.end();
        if (s.player && (r == g_lastRender.end() || now - r->second > 300))
            continue; // its movie isn't being drawn
        if (Shown(ch))
            mask |= s.buttons;
    }
    return mask;
}
#endif
