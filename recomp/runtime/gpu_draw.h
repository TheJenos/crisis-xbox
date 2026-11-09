// Draw recording on the command processor thread (gpu_draw.cpp).
#pragma once

#include "xenos_shader.h"

#include <cstdint>

struct DrawShaders
{
    uint64_t vsHash = 0, psHash = 0;
    const xenos::Translation* vs = nullptr; // null if missing or not translated
    const xenos::Translation* ps = nullptr;
};

// A DRAW_INDX / DRAW_INDX_2 packet. indexBase/indexSize: the DMA index buffer
// (physical address, VGT_DMA_SIZE-style word count with the swap mode in bits
// 30-31); immediate: inline index data for DRAW_INDX_2.
void RecordDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexSize,
                const uint32_t* immediate, uint32_t immediateDwords, const DrawShaders& shaders);

// End of a frame (the VdSwap marker): present the front buffer at frontAddress.
void RecordFrameEnd(uint32_t frontAddress, uint32_t width, uint32_t height);

// RENDER=0 disables draw recording (the command processor still runs).
bool RenderingEnabled();

// The command stream wrote ALU constant register 0x4000 + index (0-2047):
// keeps the recorder's copy of the constants current without re-reading all
// 2048 registers for every draw.
void NoteConstantWrite(uint32_t index, uint32_t value);
