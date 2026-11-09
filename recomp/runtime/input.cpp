// Keyboard -> Xbox 360 gamepad (controller port 0).
//
//   Enter      START          Tab / Backspace  BACK
//   Space      A              Escape           B
//   R          X              F                Y
//   Arrows     D-pad          W A S D          left stick
//   I J K L    right stick    Q / E            LB / RB
//   Shift      left trigger   Ctrl             right trigger
//   C          left stick click   V            right stick click
//
// Mouse (browser, after clicking the game to capture the pointer): movement
// turns the right stick; buttons and wheel are mapped in index.html (left RT,
// right LT, middle right-stick click, wheel Y).
//
// INPUT_AUTOPRESS=start:30,a:40 taps buttons at those times (seconds after
// start), for unattended test runs.
#include "input.h"
#include "guest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

uint32_t CurrentThreadId();

namespace
{
    enum : uint16_t
    {
        DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008, START = 0x0010, BACK = 0x0020,
        LTHUMB = 0x0040, RTHUMB = 0x0080, LB = 0x0100, RB = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000,
    };

    struct Binding { const char* key; uint16_t button; int axis; int dir; };
    // axis: 0 none, 1 LX, 2 LY, 3 RX, 4 RY, 5 left trigger, 6 right trigger
    const Binding kBindings[] = {
        { "enter", START, 0, 0 }, { "tab", BACK, 0, 0 }, { "backspace", BACK, 0, 0 },
        { "space", A, 0, 0 }, { "escape", B, 0, 0 }, { "r", X, 0, 0 }, { "f", Y, 0, 0 },
        { "arrowup", DPAD_UP, 0, 0 }, { "arrowdown", DPAD_DOWN, 0, 0 }, { "arrowleft", DPAD_LEFT, 0, 0 }, { "arrowright", DPAD_RIGHT, 0, 0 },
        { "q", LB, 0, 0 }, { "e", RB, 0, 0 }, { "c", LTHUMB, 0, 0 }, { "v", RTHUMB, 0, 0 },
        { "w", 0, 2, 1 }, { "s", 0, 2, -1 }, { "a", 0, 1, -1 }, { "d", 0, 1, 1 },
        { "i", 0, 4, 1 }, { "k", 0, 4, -1 }, { "j", 0, 3, -1 }, { "l", 0, 3, 1 },
        { "shift", 0, 5, 1 }, { "control", 0, 6, 1 },
    };

    std::mutex g_mutex;
    std::atomic<uint32_t> g_packet{ 1 };

    // A press stays visible until the title has polled it once, so quick taps
    // aren't lost when frames are slow (the browser build). Only once: the
    // title polls once per frame from its main thread, and a tap seen in two
    // frames 100-400 ms apart reads as a held button and auto-repeats in menus
    // (one tap moved the cursor two entries). Guarded by g_mutex.
    struct Held
    {
        bool down = false, releaseRequested = false;
        uint32_t polls = 0;

        bool Ready() const { return polls >= 1; }
        void Press()
        {
            if (!down) { down = true; polls = 0; g_packet++; }
            releaseRequested = false;
        }
        void Release()
        {
            if (!down) return;
            if (Ready()) { down = false; g_packet++; }
            else releaseRequested = true;
        }
        void Polled()
        {
            if (!down) return;
            polls++;
            if (releaseRequested && Ready()) { down = false; releaseRequested = false; g_packet++; }
        }
    };
    Held g_keys[std::size(kBindings)];
    Held g_touch[16]; // gamepad button bits

    // Pad controls by number (InputControl; see input.h): button, or axis + direction.
    struct Control { uint16_t button; int axis; int dir; };
    const Control kControls[] = {
        { A, 0, 0 }, { B, 0, 0 }, { X, 0, 0 }, { Y, 0, 0 }, { LB, 0, 0 }, { RB, 0, 0 }, { BACK, 0, 0 }, { START, 0, 0 },
        { LTHUMB, 0, 0 }, { RTHUMB, 0, 0 }, { DPAD_UP, 0, 0 }, { DPAD_DOWN, 0, 0 }, { DPAD_LEFT, 0, 0 }, { DPAD_RIGHT, 0, 0 },
        { 0, 5, 1 }, { 0, 6, 1 },
        { 0, 2, 1 }, { 0, 2, -1 }, { 0, 1, -1 }, { 0, 1, 1 },
        { 0, 4, 1 }, { 0, 4, -1 }, { 0, 3, -1 }, { 0, 3, 1 },
    };
    Held g_controls[std::size(kControls)];

    struct AutoPress { uint16_t button; double at; };
    std::vector<AutoPress> g_auto;
    std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

    void ParseAutoPress()
    {
        static bool parsed = false;
        if (parsed) return;
        parsed = true;
        const char* spec = getenv("INPUT_AUTOPRESS");
        if (!spec) return;
        std::string s = spec;
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t end = s.find(',', pos);
            std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            size_t colon = item.find(':');
            if (colon != std::string::npos)
            {
                std::string name = item.substr(0, colon);
                double at = atof(item.c_str() + colon + 1);
                static const std::pair<const char*, uint16_t> names[] = { { "start", START }, { "back", BACK }, { "a", A }, { "b", B },
                    { "x", X }, { "y", Y }, { "up", DPAD_UP }, { "down", DPAD_DOWN }, { "left", DPAD_LEFT }, { "right", DPAD_RIGHT } };
                for (auto& n : names)
                    if (name == n.first) g_auto.push_back({ n.second, at });
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        Log("input: %zu automatic button presses scheduled", g_auto.size());
    }
}

namespace
{
    float g_touchAxis[7] = {};     // guarded by g_mutex

    // Mouse look. The title only reads a gamepad, whose right stick sets the
    // turn *speed*. Each poll converts the mouse movement since the previous
    // poll into the turn rate that covers it (degrees per pixel times pixels
    // per second) and picks the stick deflection that gives that rate in
    // Crysis 2, from the response measured in game (below). The game
    // multiplies by its frame time, so the view turns by an amount
    // proportional to the mouse movement whatever the frame rate. Movement the
    // stick can't deliver this frame is carried into the next ones.
    float g_mouseDX = 0, g_mouseDY = 0;     // pixels not yet turned into stick input; guarded by g_mutex
    float g_mouseSens = 1.0f;               // guarded by g_mutex
    bool g_mouseInvertY = false;            // guarded by g_mutex

    // Crysis 2 right stick -> horizontal turn rate (degrees/s), measured by
    // holding each deflection in the first mission. Dead below ~0.21; 0.98+
    // switches to a full-tilt boost that ramps from ~140 to ~250 deg/s over
    // half a second. Vertical turning follows the same curve at ~78%.
    struct StickRate { float stick, rate; };
    constexpr StickRate kYawResponse[] = {
        { 0.21f, 0.0f }, { 0.25f, 2.0f }, { 0.30f, 5.9f }, { 0.35f, 7.5f }, { 0.40f, 12.1f }, { 0.50f, 20.0f },
        { 0.60f, 33.2f }, { 0.70f, 47.6f }, { 0.80f, 64.6f }, { 0.90f, 85.6f }, { 0.92f, 95.4f }, { 0.95f, 103.0f },
        { 0.97f, 106.1f },
    };
    constexpr float kPitchScale = 0.78f;
    constexpr float kBoostRate = 180.0f;    // average full-tilt rate, used for big flicks
    constexpr float kBoostPending = 25.0f;  // degrees still to turn before full tilt is used

    // Stick deflection (0..0.97) for a turn rate, and the rate it actually gives.
    float StickForRate(float rate, float& achieved)
    {
        const auto& t = kYawResponse;
        constexpr size_t n = std::size(kYawResponse);
        if (rate <= 0.0f) { achieved = 0.0f; return 0.0f; }
        if (rate >= t[n - 1].rate) { achieved = t[n - 1].rate; return t[n - 1].stick; }
        for (size_t i = 1; i < n; i++)
            if (rate <= t[i].rate)
            {
                float f = (rate - t[i - 1].rate) / (t[i].rate - t[i - 1].rate);
                achieved = rate;
                return t[i - 1].stick + f * (t[i].stick - t[i - 1].stick);
            }
        achieved = t[n - 1].rate;
        return t[n - 1].stick;
    }

    float EnvFloat(const char* name, float fallback)
    {
        const char* v = getenv(name);
        return v ? float(atof(v)) : fallback;
    }
}

void InputMouseMove(float dx, float dy)
{
    std::lock_guard lock(g_mutex);
    g_mouseDX += dx;
    g_mouseDY += dy;
}

void InputMouseConfig(float sensitivity, bool invertY)
{
    std::lock_guard lock(g_mutex);
    g_mouseSens = std::clamp(sensitivity, 0.05f, 20.0f);
    g_mouseInvertY = invertY;
    Log("input: mouse sensitivity %.2f%s", g_mouseSens, invertY ? ", inverted Y" : "");
}

void InputTouchButtons(uint16_t mask, bool down)
{
    std::lock_guard lock(g_mutex);
    for (int b = 0; b < 16; b++)
        if ((mask >> b) & 1)
        {
            if (down) g_touch[b].Press();
            else g_touch[b].Release();
        }
}

void InputTouchAxis(int axis, float value)
{
    if (axis < 1 || axis > 6)
        return;
    std::lock_guard lock(g_mutex);
    if (g_touchAxis[axis] != value)
    {
        g_touchAxis[axis] = value;
        g_packet++;
    }
}

void InputControl(int id, bool down)
{
    if (id < 0 || id >= int(std::size(kControls)))
        return;
    std::lock_guard lock(g_mutex);
    if (down) g_controls[id].Press();
    else g_controls[id].Release();
}

void InputKey(const char* key, bool down)
{
    std::string k = key;
    for (auto& c : k) c = char(tolower((unsigned char)c));
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < std::size(kBindings); i++)
        if (k == kBindings[i].key)
        {
            if (down) g_keys[i].Press();
            else g_keys[i].Release();
        }
}

GamepadState InputGetGamepad()
{
    ParseAutoPress();
    GamepadState s{};
    int axes[7] = {};
    {
        std::lock_guard lock(g_mutex);
        for (size_t i = 0; i < std::size(kBindings); i++)
            if (g_keys[i].down)
            {
                s.buttons |= kBindings[i].button;
                axes[kBindings[i].axis] += kBindings[i].dir;
                g_keys[i].Polled();
            }
        for (size_t i = 0; i < std::size(kControls); i++)
            if (g_controls[i].down)
            {
                s.buttons |= kControls[i].button;
                axes[kControls[i].axis] += kControls[i].dir;
                g_controls[i].Polled();
            }
    }
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    for (const auto& a : g_auto)
        if (t >= a.at && t < a.at + 0.12)
        {
            s.buttons |= a.button;
            static double lastLogged = -1;
            if (lastLogged != a.at)
            {
                lastLogged = a.at;
                Log("input: automatic press %04X at %.1f s", a.button, a.at);
            }
        }
    // Analog values: keys give full deflection; touch sticks give their own value.
    float analog[7] = {};
    {
        std::lock_guard lock(g_mutex);
        for (int b = 0; b < 16; b++)
            if (g_touch[b].down)
            {
                s.buttons |= uint16_t(1u << b);
                g_touch[b].Polled();
            }
        for (int i = 1; i < 7; i++)
            analog[i] = g_touchAxis[i] != 0.0f ? g_touchAxis[i] : float(std::clamp(axes[i], -1, 1));
    }
    // Mouse look -> right stick (overrides keys/touch while the mouse moves).
    {
        // MOUSE_DEG_PER_PX (env, experiments): degrees of turn per pixel at sensitivity 1.
        static const float degPerPxBase = EnvFloat("MOUSE_DEG_PER_PX", 0.06f);
        static std::chrono::steady_clock::time_point lastPoll = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        float dt = std::clamp(std::chrono::duration<float>(now - lastPoll).count(), 1.0f / 240.0f, 0.1f);
        lastPoll = now;
        std::lock_guard lock(g_mutex);
        if (g_mouseDX != 0.0f || g_mouseDY != 0.0f)
        {
            float degPerPx = degPerPxBase * g_mouseSens;
            // One axis: pixels still to turn -> stick deflection; consumes
            // what this frame's turning covers. scale: the axis's rate
            // relative to the measured horizontal curve.
            auto axis = [&](float& pending, float scale) {
                float deg = pending * degPerPx;
                if (std::fabs(deg) < 0.02f) { pending = 0.0f; return 0.0f; }
                float rate = std::fabs(deg) / dt / scale; // as a horizontal rate
                float stick, achieved;
                if (std::fabs(deg) > kBoostPending && rate > kYawResponse[std::size(kYawResponse) - 1].rate)
                {
                    stick = 1.0f; // big flick: full tilt
                    achieved = kBoostRate;
                }
                else
                    stick = StickForRate(rate, achieved);
                float turned = std::min(std::fabs(deg), achieved * scale * dt);
                pending -= std::copysign(turned / degPerPx, pending);
                // Carry at most half a turn.
                float cap = 180.0f / degPerPx;
                pending = std::clamp(pending, -cap, cap);
                return std::copysign(stick, deg);
            };
            analog[3] = axis(g_mouseDX, 1.0f);
            float pitch = axis(g_mouseDY, kPitchScale);
            analog[4] = g_mouseInvertY ? pitch : -pitch; // mouse down = look down (stick down)
        }
    }
    auto stick = [](float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
    auto trigger = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    s.thumbLX = stick(analog[1]);
    s.thumbLY = stick(analog[2]);
    s.thumbRX = stick(analog[3]);
    s.thumbRY = stick(analog[4]);
    s.leftTrigger = trigger(analog[5]);
    s.rightTrigger = trigger(analog[6]);
    // INPUT_TRACE=1: log every poll that reports a button (which thread polls, how often).
    static const bool trace = getenv("INPUT_TRACE") != nullptr;
    if (trace && s.buttons)
        Log("input: poll t%u buttons %04X at %.3f s", CurrentThreadId(), s.buttons, t);
    // The packet number changes exactly when the reported state changes
    // (titles compare packets to detect new input).
    {
        static std::mutex packetMutex;
        static GamepadState last{};
        static uint32_t packet = 1;
        std::lock_guard lock(packetMutex);
        if (s.buttons != last.buttons || s.leftTrigger != last.leftTrigger || s.rightTrigger != last.rightTrigger ||
            s.thumbLX != last.thumbLX || s.thumbLY != last.thumbLY || s.thumbRX != last.thumbRX || s.thumbRY != last.thumbRY)
        {
            last = s;
            packet++;
        }
        s.packet = packet;
    }
    return s;
}
