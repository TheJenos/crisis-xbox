// WebGPU backend for recorded frames (render_wgpu.cpp). Shared by the native
// build (wgpu-native, display thread) and the browser build (emdawnwebgpu,
// page main thread): all calls must come from the thread that owns the device.
#pragma once

#include <cstdint>
#include <vector>
#include <webgpu/webgpu.h>

namespace render
{
    // Start accepting frames; until then SubmitFrame drops them.
    void BackendInit(WGPUDevice device, WGPUQueue queue);

    // Execute every queued frame. Returns the view of the most recently
    // presented texture (nullptr if none yet) and the region of it to show.
    WGPUTextureView BackendRunPending(uint32_t& showW, uint32_t& showH, uint32_t& texW, uint32_t& texH);

    // The presented texture (for frame dumps): size, whether RGBA16F (else RGBA8),
    // and how many frames have been executed so far.
    WGPUTexture BackendPresentTexture(uint32_t& w, uint32_t& h, bool& isFloat, uint64_t& frames);

    // Resolve outputs captured for FRAME_DUMP (native debugging): read-back
    // buffers already copied on the GPU, to map and save as PNGs.
    struct ResolveDump { WGPUBuffer buffer; uint32_t w, h, rowBytes; bool isFloat; bool isR32F; char name[96]; };
    std::vector<ResolveDump> BackendTakeResolveDumps();

    // Number of frames waiting (for the browser's scheduling).
    uint32_t BackendPendingFrames();
}
