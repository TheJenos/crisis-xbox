// Host display: a Win32 window presenting frames through WebGPU (wgpu-native).
//
// All WebGPU calls happen on the display thread. Other threads hand it frames
// with DisplaySubmitFrame(); it shows the latest one, letterboxed, every vsync.
//
// DUMP_FRAMES=<dir>: also write every 60th submitted frame there as PNG.
#ifdef _WIN32

#include "guest.h"
#include "display.h"

#include <condition_variable>
#include <mutex>
#include <vector>
#include <windows.h>
#include <webgpu/webgpu.h>
#include "render_backend.h"
#include "input.h"
#include <webgpu/wgpu.h>

namespace
{
    std::mutex g_frameMutex;
    std::vector<uint8_t> g_pending;
    uint32_t g_pendingW = 0, g_pendingH = 0;
    bool g_hasPending = false;
    std::string g_title;
    uint64_t g_submitted = 0;

    HWND g_hwnd = nullptr;

    // ---------------------------------------------------------------------
    // PNG writer (stored deflate blocks, no compression) for frame dumps.
    // ---------------------------------------------------------------------
    uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0)
    {
        static uint32_t table[256];
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; i++)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            init = true;
        }
        crc = ~crc;
        for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    void WritePng(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h)
    {
        std::vector<uint8_t> raw;
        raw.reserve(size_t(h) * (w * 4 + 1));
        for (uint32_t y = 0; y < h; y++)
        {
            raw.push_back(0); // filter: none
            raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
        }
        std::vector<uint8_t> z = { 0x78, 0x01 };
        uint32_t a = 1, b = 0;
        for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
        for (size_t off = 0; off < raw.size(); off += 65535)
        {
            size_t len = std::min<size_t>(65535, raw.size() - off);
            z.push_back(off + len == raw.size() ? 1 : 0);
            z.push_back(uint8_t(len)); z.push_back(uint8_t(len >> 8));
            z.push_back(uint8_t(~len)); z.push_back(uint8_t(~len >> 8));
            z.insert(z.end(), raw.begin() + off, raw.begin() + off + len);
        }
        uint32_t adler = (b << 16) | a;
        for (int i = 3; i >= 0; i--) z.push_back(uint8_t(adler >> (i * 8)));

        FILE* f = fopen(path, "wb");
        if (!f) return;
        auto be32 = [&](uint32_t v) { uint8_t x[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) }; fwrite(x, 1, 4, f); };
        auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
            be32(uint32_t(data.size()));
            std::vector<uint8_t> td(type, type + 4);
            td.insert(td.end(), data.begin(), data.end());
            fwrite(td.data(), 1, td.size(), f);
            be32(Crc32(td.data(), td.size()));
        };
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
        fwrite(sig, 1, 8, f);
        std::vector<uint8_t> ihdr = { uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                      uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0 };
        chunk("IHDR", ihdr);
        chunk("IDAT", z);
        chunk("IEND", {});
        fclose(f);
    }

    // ---------------------------------------------------------------------
    // WebGPU
    // ---------------------------------------------------------------------
    WGPUStringView Str(const char* s) { return { s, strlen(s) }; }

    const char* kShader = R"(
@group(0) @binding(0) var frameTex: texture_2d<f32>;
@group(0) @binding(1) var frameSampler: sampler;
@group(0) @binding(2) var<uniform> uvScale: vec4f;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs(@builtin(vertex_index) i: u32) -> VOut {
    var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    var o: VOut;
    o.pos = vec4f(p[i], 0.0, 1.0);
    o.uv = vec2f((p[i].x + 1.0) * 0.5, (1.0 - p[i].y) * 0.5);
    return o;
}

@fragment fn fs(v: VOut) -> @location(0) vec4f {
    return vec4f(textureSample(frameTex, frameSampler, v.uv * uvScale.xy).rgb, 1.0);
}
)";

    struct Gpu
    {
        WGPUInstance instance = nullptr;
        WGPUSurface surface = nullptr;
        WGPUAdapter adapter = nullptr;
        WGPUDevice device = nullptr;
        WGPUQueue queue = nullptr;
        WGPUTextureFormat surfaceFormat = WGPUTextureFormat_Undefined;
        WGPURenderPipeline pipeline = nullptr;
        WGPUSampler sampler = nullptr;
        WGPUTexture frameTex = nullptr;
        WGPUTextureView frameView = nullptr;
        WGPUBindGroup bindGroup = nullptr;
        uint32_t texW = 0, texH = 0;
        uint32_t surfW = 0, surfH = 0;
        WGPUBuffer uvBuffer = nullptr;
        WGPUBuffer uvOne = nullptr;
        WGPUTextureView presentView = nullptr; // rendered front buffer (render_wgpu.cpp)
        WGPUBindGroup presentBg = nullptr;
    } g;

    void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView msg, void* ud1, void*)
    {
        if (status == WGPURequestAdapterStatus_Success)
            g.adapter = adapter;
        else
            Log("display: adapter request failed: %.*s", int(msg.length), msg.data ? msg.data : "");
        *(bool*)ud1 = true;
    }

    void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView msg, void* ud1, void*)
    {
        if (status == WGPURequestDeviceStatus_Success)
            g.device = device;
        else
            Log("display: device request failed: %.*s", int(msg.length), msg.data ? msg.data : "");
        *(bool*)ud1 = true;
    }

    void OnDeviceError(WGPUDevice const*, WGPUErrorType type, WGPUStringView msg, void*, void*)
    {
        Log("display: WebGPU error %d: %.*s", int(type), int(msg.length), msg.data ? msg.data : "");
    }

    void ConfigureSurface(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0)
            return;
        WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
        cfg.device = g.device;
        cfg.format = g.surfaceFormat;
        cfg.usage = WGPUTextureUsage_RenderAttachment;
        cfg.width = w;
        cfg.height = h;
        cfg.presentMode = WGPUPresentMode_Fifo;
        cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(g.surface, &cfg);
        g.surfW = w;
        g.surfH = h;
    }

    WGPUBuffer UniformVec4(float x, float y);
    WGPUBindGroup MakeBlitBindGroup(WGPUTextureView view, WGPUBuffer uv);

    bool InitGpu()
    {
        g.instance = wgpuCreateInstance(nullptr);
        if (!g.instance)
            return false;

        WGPUSurfaceSourceWindowsHWND src = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
        src.hinstance = ::GetModuleHandleA(nullptr);
        src.hwnd = g_hwnd;
        WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
        sd.nextInChain = &src.chain;
        g.surface = wgpuInstanceCreateSurface(g.instance, &sd);

        bool done = false;
        WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
        ao.compatibleSurface = g.surface;
        ao.powerPreference = WGPUPowerPreference_HighPerformance;
        WGPURequestAdapterCallbackInfo aci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
        aci.mode = WGPUCallbackMode_AllowProcessEvents;
        aci.callback = OnAdapter;
        aci.userdata1 = &done;
        wgpuInstanceRequestAdapter(g.instance, &ao, aci);
        while (!done) wgpuInstanceProcessEvents(g.instance);
        if (!g.adapter)
            return false;

        WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
        if (wgpuAdapterGetInfo(g.adapter, &info) == WGPUStatus_Success)
            Log("display: adapter \"%.*s\" (%.*s), backend %d", int(info.device.length), info.device.data,
                int(info.description.length), info.description.data, int(info.backendType));

        done = false;
        WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
        dd.uncapturedErrorCallbackInfo.callback = OnDeviceError;
        // Filterable 32-bit float textures hold resolved depth at full precision.
        static const WGPUFeatureName kFloat32Filterable = WGPUFeatureName_Float32Filterable;
        if (wgpuAdapterHasFeature(g.adapter, kFloat32Filterable))
        {
            dd.requiredFeatureCount = 1;
            dd.requiredFeatures = &kFloat32Filterable;
        }
        WGPURequestDeviceCallbackInfo dci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
        dci.mode = WGPUCallbackMode_AllowProcessEvents;
        dci.callback = OnDevice;
        dci.userdata1 = &done;
        wgpuAdapterRequestDevice(g.adapter, &dd, dci);
        while (!done) wgpuInstanceProcessEvents(g.instance);
        if (!g.device)
            return false;
        g.queue = wgpuDeviceGetQueue(g.device);

        WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
        wgpuSurfaceGetCapabilities(g.surface, g.adapter, &caps);
        g.surfaceFormat = caps.formatCount ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
        // Prefer a non-sRGB format: frames are already gamma-encoded.
        for (size_t i = 0; i < caps.formatCount; i++)
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm || caps.formats[i] == WGPUTextureFormat_RGBA8Unorm)
            {
                g.surfaceFormat = caps.formats[i];
                break;
            }

        WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
        wgsl.code = Str(kShader);
        WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        smd.nextInChain = &wgsl.chain;
        WGPUShaderModule module = wgpuDeviceCreateShaderModule(g.device, &smd);

        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = g.surfaceFormat;
        WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
        fs.module = module;
        fs.entryPoint = Str("fs");
        fs.targetCount = 1;
        fs.targets = &target;
        WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        pd.vertex.module = module;
        pd.vertex.entryPoint = Str("vs");
        pd.fragment = &fs;
        pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        g.pipeline = wgpuDeviceCreateRenderPipeline(g.device, &pd);

        WGPUSamplerDescriptor samp = WGPU_SAMPLER_DESCRIPTOR_INIT;
        samp.magFilter = WGPUFilterMode_Linear;
        samp.minFilter = WGPUFilterMode_Linear;
        g.sampler = wgpuDeviceCreateSampler(g.device, &samp);

        g.uvOne = UniformVec4(1, 1);
        RECT rc;
        ::GetClientRect(g_hwnd, &rc);
        ConfigureSurface(uint32_t(rc.right - rc.left), uint32_t(rc.bottom - rc.top));
        if (g.pipeline && (!getenv("RENDER") || atoi(getenv("RENDER")) != 0))
            render::BackendInit(g.device, g.queue);
        return g.pipeline != nullptr;
    }

    WGPUBuffer UniformVec4(float x, float y)
    {
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.size = 16;
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        WGPUBuffer b = wgpuDeviceCreateBuffer(g.device, &bd);
        float v[4] = { x, y, 0, 0 };
        wgpuQueueWriteBuffer(g.queue, b, 0, v, 16);
        return b;
    }

    WGPUBindGroup MakeBlitBindGroup(WGPUTextureView view, WGPUBuffer uv)
    {
        WGPUBindGroupEntry entries[3] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
        entries[0].binding = 0;
        entries[0].textureView = view;
        entries[1].binding = 1;
        entries[1].sampler = g.sampler;
        entries[2].binding = 2;
        entries[2].buffer = uv;
        entries[2].size = 16;
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = wgpuRenderPipelineGetBindGroupLayout(g.pipeline, 0);
        bgd.entryCount = 3;
        bgd.entries = entries;
        return wgpuDeviceCreateBindGroup(g.device, &bgd);
    }

    // (Re)create the frame texture and its bind group for a new size.
    void EnsureFrameTexture(uint32_t w, uint32_t h)
    {
        if (g.frameTex && g.texW == w && g.texH == h)
            return;
        if (g.bindGroup) wgpuBindGroupRelease(g.bindGroup);
        if (g.frameView) wgpuTextureViewRelease(g.frameView);
        if (g.frameTex) wgpuTextureRelease(g.frameTex);

        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.size = { w, h, 1 };
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        g.frameTex = wgpuDeviceCreateTexture(g.device, &td);
        g.frameView = wgpuTextureCreateView(g.frameTex, nullptr);
        g.texW = w;
        g.texH = h;

        g.bindGroup = MakeBlitBindGroup(g.frameView, g.uvOne);
    }

    void RenderFrame()
    {
        {
            std::lock_guard lock(g_frameMutex);
            if (g_hasPending)
            {
                EnsureFrameTexture(g_pendingW, g_pendingH);
                WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                dst.texture = g.frameTex;
                WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
                layout.bytesPerRow = g_pendingW * 4;
                layout.rowsPerImage = g_pendingH;
                WGPUExtent3D size = { g_pendingW, g_pendingH, 1 };
                wgpuQueueWriteTexture(g.queue, &dst, g_pending.data(), g_pending.size(), &layout, &size);
                g_hasPending = false;
            }
        }

        // Rendered frames take over from the debug view once a front buffer exists.
        uint32_t showW = 0, showH = 0, fullW = 0, fullH = 0;
        WGPUTextureView rendered = render::BackendRunPending(showW, showH, fullW, fullH);
        WGPUBindGroup bg = g.bindGroup;
        float contentW = float(g.texW), contentH = float(g.texH);
        if (rendered)
        {
            static uint32_t lastShowW = 0, lastShowH = 0;
            if (rendered != g.presentView || showW != lastShowW || showH != lastShowH)
            {
                if (g.presentBg) wgpuBindGroupRelease(g.presentBg);
                if (!g.uvBuffer) g.uvBuffer = UniformVec4(1, 1);
                float uv[4] = { float(showW) / float(fullW), float(showH) / float(fullH), 0, 0 };
                wgpuQueueWriteBuffer(g.queue, g.uvBuffer, 0, uv, 16);
                g.presentBg = MakeBlitBindGroup(rendered, g.uvBuffer);
                g.presentView = rendered;
                lastShowW = showW;
                lastShowH = showH;
            }
            bg = g.presentBg;
            contentW = float(showW);
            contentH = float(showH);
        }

        WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(g.surface, &st);
        if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
        {
            if (st.texture) wgpuTextureRelease(st.texture);
            ConfigureSurface(g.surfW, g.surfH);
            return;
        }
        WGPUTextureView view = wgpuTextureCreateView(st.texture, nullptr);
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);

        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = view;
        ca.loadOp = WGPULoadOp_Clear;
        ca.storeOp = WGPUStoreOp_Store;
        ca.clearValue = { 0.0, 0.0, 0.0, 1.0 };
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        if (bg && contentW > 0 && contentH > 0)
        {
            // Letterbox: keep the frame's aspect ratio inside the window.
            float sw = float(g.surfW), sh = float(g.surfH);
            float scale = std::min(sw / contentW, sh / contentH);
            float vw = contentW * scale, vh = contentH * scale;
            wgpuRenderPassEncoderSetViewport(pass, (sw - vw) / 2, (sh - vh) / 2, vw, vh, 0.0f, 1.0f);
            wgpuRenderPassEncoderSetPipeline(pass, g.pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuQueueSubmit(g.queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);
        wgpuSurfacePresent(g.surface);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(st.texture);
    }

    float HalfToFloat(uint16_t h)
    {
        uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
        float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * float(1 << e) / 32768.0f;
        return s ? -v : v;
    }

    // RENDER_DUMP=<dir>: every 60th rendered frame, read the front buffer back and save it as PNG.
    void DumpRendered()
    {
        static const char* dir = getenv("RENDER_DUMP");
        static uint64_t lastDumped = 0;
        if (!dir)
            return;
        uint32_t w = 0, h = 0;
        bool isFloat = false;
        uint64_t frames = 0;
        WGPUTexture tex = render::BackendPresentTexture(w, h, isFloat, frames);
        if (!tex || frames < lastDumped + 60)
            return;
        lastDumped = frames;
        uint32_t bpp = isFloat ? 8 : 4, rowBytes = (w * bpp + 255) & ~255u;
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.size = uint64_t(rowBytes) * h;
        bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        WGPUBuffer buf = wgpuDeviceCreateBuffer(g.device, &bd);
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        src.texture = tex;
        WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        dst.buffer = buf;
        dst.layout.bytesPerRow = rowBytes;
        dst.layout.rowsPerImage = h;
        WGPUExtent3D size = { w, h, 1 };
        wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &size);
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuQueueSubmit(g.queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);
        bool done = false;
        WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        mci.mode = WGPUCallbackMode_AllowProcessEvents;
        mci.callback = [](WGPUMapAsyncStatus, WGPUStringView, void* ud, void*) { *(bool*)ud = true; };
        mci.userdata1 = &done;
        wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mci);
        while (!done)
        {
            wgpuDevicePoll(g.device, true, nullptr);
            wgpuInstanceProcessEvents(g.instance);
        }
        const uint8_t* p = (const uint8_t*)wgpuBufferGetConstMappedRange(buf, 0, bd.size);
        if (p)
        {
            std::vector<uint8_t> rgba(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                {
                    uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
                    if (isFloat)
                    {
                        const uint16_t* hp = (const uint16_t*)(p + size_t(y) * rowBytes + x * 8);
                        for (int c = 0; c < 4; c++)
                            o[c] = uint8_t(std::clamp(HalfToFloat(hp[c]), 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    else
                        memcpy(o, p + size_t(y) * rowBytes + x * 4, 4);
                    o[3] = 255;
                }
            char path[512];
            snprintf(path, sizeof(path), "%s/render_%06llu.png", dir, (unsigned long long)frames);
            WritePng(path, rgba.data(), w, h);
            Log("display: dumped %s (%ux%u%s)", path, w, h, isFloat ? ", RGBA16F" : "");
        }
        wgpuBufferUnmap(buf);
        wgpuBufferRelease(buf);
    }

    // FRAME_DUMP=<dir> (with FRAME_LOG): save each resolve output of the logged frames.
    void SaveResolveDumps()
    {
        for (render::ResolveDump& d : render::BackendTakeResolveDumps())
        {
            uint64_t bytes = uint64_t(d.rowBytes) * d.h;
            bool done = false;
            WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            mci.mode = WGPUCallbackMode_AllowProcessEvents;
            mci.callback = [](WGPUMapAsyncStatus, WGPUStringView, void* ud, void*) { *(bool*)ud = true; };
            mci.userdata1 = &done;
            wgpuBufferMapAsync(d.buffer, WGPUMapMode_Read, 0, bytes, mci);
            while (!done)
            {
                wgpuDevicePoll(g.device, true, nullptr);
                wgpuInstanceProcessEvents(g.instance);
            }
            const uint8_t* p = (const uint8_t*)wgpuBufferGetConstMappedRange(d.buffer, 0, bytes);
            if (p)
            {
                std::vector<uint8_t> rgba(size_t(d.w) * d.h * 4);
                for (uint32_t y = 0; y < d.h; y++)
                    for (uint32_t x = 0; x < d.w; x++)
                    {
                        uint8_t* o = &rgba[(size_t(y) * d.w + x) * 4];
                        if (d.isFloat)
                        {
                            const uint16_t* hp = (const uint16_t*)(p + size_t(y) * d.rowBytes + x * 8);
                            for (int c = 0; c < 4; c++)
                                o[c] = uint8_t(std::clamp(HalfToFloat(hp[c]), 0.0f, 1.0f) * 255.0f + 0.5f);
                        }
                        else
                            memcpy(o, p + size_t(y) * d.rowBytes + x * 4, 4);
                        o[3] = 255;
                    }
                WritePng(d.name, rgba.data(), d.w, d.h);
                if (!d.isFloat)
                {
                    // Alpha too, as a grey image: <name>.alpha.png.
                    std::vector<uint8_t> a(size_t(d.w) * d.h * 4);
                    for (uint32_t y = 0; y < d.h; y++)
                        for (uint32_t x = 0; x < d.w; x++)
                        {
                            uint8_t v = p[size_t(y) * d.rowBytes + x * 4 + 3];
                            uint8_t* o = &a[(size_t(y) * d.w + x) * 4];
                            o[0] = o[1] = o[2] = v;
                            o[3] = 255;
                        }
                    WritePng((std::string(d.name) + ".alpha.png").c_str(), a.data(), d.w, d.h);
                }
                if (d.isFloat)
                {
                    // Raw values too: <name>.f16 = u32 width, u32 height, then RGBA16F rows.
                    std::string raw = std::string(d.name) + ".f16";
                    if (FILE* fp = fopen(raw.c_str(), "wb"))
                    {
                        fwrite(&d.w, 4, 1, fp);
                        fwrite(&d.h, 4, 1, fp);
                        for (uint32_t y = 0; y < d.h; y++)
                            fwrite(p + size_t(y) * d.rowBytes, 8, d.w, fp);
                        fclose(fp);
                    }
                }
            }
            wgpuBufferUnmap(d.buffer);
            wgpuBufferRelease(d.buffer);
        }
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        {
            bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            const char* name = nullptr;
            char letter[2] = {};
            switch (wp)
            {
            case VK_RETURN: name = "enter"; break;
            case VK_TAB: name = "tab"; break;
            case VK_BACK: name = "backspace"; break;
            case VK_SPACE: name = "space"; break;
            case VK_ESCAPE: name = "escape"; break;
            case VK_UP: name = "arrowup"; break;
            case VK_DOWN: name = "arrowdown"; break;
            case VK_LEFT: name = "arrowleft"; break;
            case VK_RIGHT: name = "arrowright"; break;
            case VK_SHIFT: name = "shift"; break;
            case VK_CONTROL: name = "control"; break;
            default:
                if (wp >= 'A' && wp <= 'Z') { letter[0] = char(wp - 'A' + 'a'); name = letter; }
            }
            if (name) InputKey(name, down);
            return 0;
        }
        case WM_SIZE:
            if (g.device)
                ConfigureSurface(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_CLOSE:
            Log("display: window closed, exiting");
            fflush(nullptr);
            ::ExitProcess(0);
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    void DisplayThread()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Crysis2Recomp";
        ::RegisterClassW(&wc);
        RECT rc = { 0, 0, 1280, 720 };
        ::AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        g_hwnd = ::CreateWindowW(wc.lpszClassName, L"Crysis 2 (recompiled)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                 nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_hwnd || !InitGpu())
        {
            Log("display: initialization failed; continuing without a window");
            return;
        }
        Log("display: window ready (%ux%u, surface format %d)", g.surfW, g.surfH, int(g.surfaceFormat));

        std::string shownTitle;
        for (;;)
        {
            MSG m;
            while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            {
                ::TranslateMessage(&m);
                ::DispatchMessageW(&m);
            }
            {
                std::lock_guard lock(g_frameMutex);
                if (g_title != shownTitle)
                {
                    shownTitle = g_title;
                    std::wstring w(shownTitle.begin(), shownTitle.end());
                    ::SetWindowTextW(g_hwnd, w.c_str());
                }
            }
            RenderFrame(); // blocks on vsync (FIFO present)
            DumpRendered();
            SaveResolveDumps();
        }
    }
}

void DisplayStart()
{
    if (getenv("NO_WINDOW"))
        return;
    std::thread(DisplayThread).detach();
}

void DisplaySubmitFrame(const uint8_t* rgba, uint32_t w, uint32_t h, const char* title)
{
    static const char* dumpDir = getenv("DUMP_FRAMES");
    uint64_t n;
    {
        std::lock_guard lock(g_frameMutex);
        g_pending.assign(rgba, rgba + size_t(w) * h * 4);
        g_pendingW = w;
        g_pendingH = h;
        g_hasPending = true;
        g_title = title ? title : "";
        n = g_submitted++;
    }
    if (dumpDir && n % 60 == 0)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%06llu.png", dumpDir, (unsigned long long)n);
        WritePng(path, rgba, w, h);
    }
}

#endif // _WIN32
