// Host input -> Xbox 360 gamepad state (input.cpp).
#pragma once

#include <cstdint>

struct GamepadState
{
    uint32_t packet;      // changes whenever the state changes
    uint16_t buttons;     // XINPUT_GAMEPAD_* bits
    uint8_t leftTrigger, rightTrigger;
    int16_t thumbLX, thumbLY, thumbRX, thumbRY;
};

GamepadState InputGetGamepad();

// Host side: report a key (or button) change. `key` is a lowercase name such
// as "enter", "space", "w", "arrowup" (see input.cpp for the mapping).
void InputKey(const char* key, bool down);

// Touch / on-screen controls: hold or release pad buttons (XINPUT_GAMEPAD_*
// bits), and set an analog axis (1 LX, 2 LY, 3 RX, 4 RY: -1..1; 5 LT, 6 RT: 0..1).
void InputTouchButtons(uint16_t mask, bool down);
void InputTouchAxis(int axis, float value);

// Pad controls by number, for host-side remappable bindings (the browser page
// maps keys and mouse buttons to these): 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB,
// 6 BACK, 7 START, 8 left-stick click, 9 right-stick click, 10-13 D-pad
// up/down/left/right, 14 LT, 15 RT, 16-19 left stick up/down/left/right,
// 20-23 right stick up/down/left/right. A press stays visible until polled.
void InputControl(int id, bool down);

// Mouse look: relative movement in screen pixels (pointer lock), turned into
// right-stick deflection at each poll. Sensitivity scales it (1 = default);
// invertY flips vertical look.
void InputMouseMove(float dx, float dy);
void InputMouseConfig(float sensitivity, bool invertY);
