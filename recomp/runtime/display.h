// Host display window (display.cpp).
#pragma once

#include <cstdint>

// Open the window and start the display thread (no-op if NO_WINDOW is set).
void DisplayStart();

// Hand the display a frame to show (tightly packed RGBA8). Thread-safe; the
// latest frame wins. `title` is shown in the window's title bar.
void DisplaySubmitFrame(const uint8_t* rgba, uint32_t w, uint32_t h, const char* title);
