// Browser display: WebGPU (emdawnwebgpu) on the page's main thread.
//
// The game's threads are workers without a JS event loop, so they can't own a
// WebGPU device. The main thread owns it: every animation frame it executes
// the frames the command processor queued (render_wgpu.cpp) and draws the
// presented front buffer onto <canvas id="screen">, letterboxed. Until a front
// buffer exists, debug frames from DisplaySubmitFrame are shown instead.
#ifdef __EMSCRIPTEN__

#include "guest.h"
#include "display.h"
#include "render_backend.h"
#include "input.h"
#include "png.h"
#include "platform.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <mutex>
#include <string>
#include <vector>
#include <webgpu/webgpu.h>

namespace
{
    WGPUStringView Str(const char* s) { return { s, strlen(s) }; }

    float HalfToFloat(uint16_t h)
    {
        uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
        float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * float(1 << e) / 32768.0f;
        return s ? -v : v;
    }

    // Resolve outputs captured for a dump (see crysis_dump_frame): read each
    // back asynchronously and upload a PNG (float targets clamped to 0-1, plus
    // the raw RGBA16F as <name>.f16; 8-bit targets plus their alpha as
    // <name>.alpha.png).
    void UploadResolveDumps()
    {
        for (render::ResolveDump& d : render::BackendTakeResolveDumps())
        {
            auto* job = new render::ResolveDump(d);
            WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            ci.mode = WGPUCallbackMode_AllowSpontaneous;
            ci.userdata1 = job;
            ci.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud, void*) {
                auto* d = (render::ResolveDump*)ud;
                uint64_t bytes = uint64_t(d->rowBytes) * d->h;
                const uint8_t* p = status == WGPUMapAsyncStatus_Success ? (const uint8_t*)wgpuBufferGetConstMappedRange(d->buffer, 0, bytes) : nullptr;
                if (p && d->isR32F)
                {
                    // Depth: raw floats (<name>.r32: u32 w, u32 h, rows of f32) and
                    // a grey preview stretched over the frame's depth range.
                    const char* slash = strrchr(d->name, '/');
                    std::string name = slash ? slash + 1 : d->name;
                    std::vector<uint8_t> raw(8 + size_t(d->w) * d->h * 4);
                    memcpy(raw.data(), &d->w, 4);
                    memcpy(raw.data() + 4, &d->h, 4);
                    float lo = 1e30f, hi = -1e30f;
                    for (uint32_t y = 0; y < d->h; y++)
                    {
                        memcpy(raw.data() + 8 + size_t(y) * d->w * 4, p + size_t(y) * d->rowBytes, size_t(d->w) * 4);
                        for (uint32_t x = 0; x < d->w; x++)
                        {
                            float v;
                            memcpy(&v, p + size_t(y) * d->rowBytes + x * 4, 4);
                            lo = std::min(lo, v);
                            hi = std::max(hi, v);
                        }
                    }
                    PlatformUploadDump((name + ".r32").c_str(), raw.data(), raw.size());
                    std::vector<uint8_t> grey(size_t(d->w) * d->h * 4);
                    for (uint32_t y = 0; y < d->h; y++)
                        for (uint32_t x = 0; x < d->w; x++)
                        {
                            float v;
                            memcpy(&v, p + size_t(y) * d->rowBytes + x * 4, 4);
                            uint8_t g = uint8_t(hi > lo ? (v - lo) / (hi - lo) * 255.0f : 0.0f);
                            uint8_t* o = &grey[(size_t(y) * d->w + x) * 4];
                            o[0] = o[1] = o[2] = g;
                            o[3] = 255;
                        }
                    std::vector<uint8_t> png = EncodePng(grey.data(), d->w, d->h);
                    PlatformUploadDump(name.c_str(), png.data(), png.size());
                }
                else if (p)
                {
                    const char* slash = strrchr(d->name, '/');
                    std::string name = slash ? slash + 1 : d->name;
                    std::vector<uint8_t> rgba(size_t(d->w) * d->h * 4), alpha;
                    if (!d->isFloat) alpha.resize(rgba.size());
                    for (uint32_t y = 0; y < d->h; y++)
                        for (uint32_t x = 0; x < d->w; x++)
                        {
                            uint8_t* o = &rgba[(size_t(y) * d->w + x) * 4];
                            if (d->isFloat)
                            {
                                const uint16_t* hp = (const uint16_t*)(p + size_t(y) * d->rowBytes + x * 8);
                                for (int c = 0; c < 4; c++)
                                    o[c] = uint8_t(std::clamp(HalfToFloat(hp[c]), 0.0f, 1.0f) * 255.0f + 0.5f);
                            }
                            else
                            {
                                memcpy(o, p + size_t(y) * d->rowBytes + x * 4, 4);
                                uint8_t* a = &alpha[(size_t(y) * d->w + x) * 4];
                                a[0] = a[1] = a[2] = o[3];
                                a[3] = 255;
                            }
                            o[3] = 255;
                        }
                    std::vector<uint8_t> png = EncodePng(rgba.data(), d->w, d->h);
                    PlatformUploadDump(name.c_str(), png.data(), int(png.size()));
                    if (!alpha.empty())
                    {
                        png = EncodePng(alpha.data(), d->w, d->h);
                        PlatformUploadDump((name + ".alpha.png").c_str(), png.data(), int(png.size()));
                    }
                    if (d->isFloat)
                    {
                        std::vector<uint8_t> raw(8 + size_t(d->w) * d->h * 8);
                        memcpy(raw.data(), &d->w, 4);
                        memcpy(raw.data() + 4, &d->h, 4);
                        for (uint32_t y = 0; y < d->h; y++)
                            memcpy(raw.data() + 8 + size_t(y) * d->w * 8, p + size_t(y) * d->rowBytes, size_t(d->w) * 8);
                        PlatformUploadDump((name + ".f16").c_str(), raw.data(), int(raw.size()));
                    }
                }
                wgpuBufferUnmap(d->buffer);
                wgpuBufferRelease(d->buffer);
                delete d;
            };
            wgpuBufferMapAsync(d.buffer, WGPUMapMode_Read, 0, uint64_t(d.rowBytes) * d.h, ci);
        }
    }

    const char* kShader = R"(
@group(0) @binding(0) var frameTex: texture_2d<f32>;
@group(0) @binding(1) var frameSampler: sampler;
@group(0) @binding(2) var<uniform> uvScale: vec4f;
struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f, };
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

    struct State
    {
        WGPUInstance instance = nullptr;
        WGPUAdapter adapter = nullptr;
        WGPUDevice device = nullptr;
        WGPUQueue queue = nullptr;
        WGPUSurface surface = nullptr;
        WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;
        WGPURenderPipeline pipeline = nullptr;
        WGPUSampler sampler = nullptr;
        WGPUBuffer uv = nullptr;
        WGPUTexture debugTex = nullptr;
        WGPUTextureView debugView = nullptr;
        uint32_t debugW = 0, debugH = 0;
        WGPUBindGroup bg = nullptr;
        WGPUTextureView bgView = nullptr;
        float bgU = 0, bgV = 0;
        int canvasW = 0, canvasH = 0;
    } g;

    // Debug frames from other threads.
    std::mutex g_mutex;
    std::vector<uint8_t> g_pixels;
    uint32_t g_w = 0, g_h = 0;
    bool g_new = false;
    std::string g_title, g_shownTitle;

    void ConfigureSurface(int w, int h)
    {
        WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
        cfg.device = g.device;
        cfg.format = g.format;
        cfg.usage = WGPUTextureUsage_RenderAttachment;
        cfg.width = uint32_t(w);
        cfg.height = uint32_t(h);
        cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
        cfg.presentMode = WGPUPresentMode_Fifo;
        wgpuSurfaceConfigure(g.surface, &cfg);
        g.canvasW = w;
        g.canvasH = h;
    }

    void BindView(WGPUTextureView view, float u, float v)
    {
        if (view == g.bgView && u == g.bgU && v == g.bgV)
            return;
        if (g.bg) wgpuBindGroupRelease(g.bg);
        float uv[4] = { u, v, 0, 0 };
        wgpuQueueWriteBuffer(g.queue, g.uv, 0, uv, 16);
        WGPUBindGroupEntry e[3] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
        e[0].binding = 0; e[0].textureView = view;
        e[1].binding = 1; e[1].sampler = g.sampler;
        e[2].binding = 2; e[2].buffer = g.uv; e[2].size = 16;
        WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bd.layout = wgpuRenderPipelineGetBindGroupLayout(g.pipeline, 0);
        bd.entryCount = 3;
        bd.entries = e;
        g.bg = wgpuDeviceCreateBindGroup(g.device, &bd);
        g.bgView = view;
        g.bgU = u;
        g.bgV = v;
    }

    bool OnAnimationFrame(double, void*)
    {
        // Redraw the canvas only when there's something new: the display
        // refreshes at 60+ Hz but the game makes at most 30 frames a second,
        // and an untouched WebGPU canvas keeps showing its last picture.
        static uint64_t presentedFrames = UINT64_MAX;
        bool dirty = false;
        int cw = 0, ch = 0;
        emscripten_get_canvas_element_size("#screen", &cw, &ch);
        if (cw > 0 && ch > 0 && (cw != g.canvasW || ch != g.canvasH))
        {
            ConfigureSurface(cw, ch);
            dirty = true;
        }

        // Rendered front buffer, else the latest debug frame.
        uint32_t showW = 0, showH = 0, texW = 0, texH = 0;
        WGPUTextureView view = render::BackendRunPending(showW, showH, texW, texH);
        UploadResolveDumps();
        {
            uint32_t pw = 0, ph = 0;
            bool pf = false;
            uint64_t frames = 0;
            render::BackendPresentTexture(pw, ph, pf, frames);
            if (frames != presentedFrames)
            {
                presentedFrames = frames;
                dirty = true;
            }
        }
        float contentW = 0, contentH = 0;
        if (view)
        {
            BindView(view, float(showW) / float(texW), float(showH) / float(texH));
            contentW = float(showW);
            contentH = float(showH);
        }
        else
        {
            std::lock_guard lock(g_mutex);
            if (g_new)
            {
                if (!g.debugTex || g.debugW != g_w || g.debugH != g_h)
                {
                    if (g.debugView) wgpuTextureViewRelease(g.debugView);
                    if (g.debugTex) wgpuTextureRelease(g.debugTex);
                    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
                    td.size = { g_w, g_h, 1 };
                    td.format = WGPUTextureFormat_RGBA8Unorm;
                    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
                    g.debugTex = wgpuDeviceCreateTexture(g.device, &td);
                    g.debugView = wgpuTextureCreateView(g.debugTex, nullptr);
                    g.debugW = g_w;
                    g.debugH = g_h;
                }
                WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                dst.texture = g.debugTex;
                WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
                layout.bytesPerRow = g_w * 4;
                layout.rowsPerImage = g_h;
                WGPUExtent3D size = { g_w, g_h, 1 };
                wgpuQueueWriteTexture(g.queue, &dst, g_pixels.data(), g_pixels.size(), &layout, &size);
                g_new = false;
                dirty = true;
            }
            if (g.debugView)
            {
                BindView(g.debugView, 1, 1);
                contentW = float(g.debugW);
                contentH = float(g.debugH);
            }
            if (g_title != g_shownTitle)
            {
                g_shownTitle = g_title;
                emscripten_set_window_title(g_shownTitle.c_str());
            }
        }

        if (!dirty)
            return true;
        WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(g.surface, &st);
        if (!st.texture)
            return true;
        WGPUTextureView target = wgpuTextureCreateView(st.texture, nullptr);
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = target;
        ca.loadOp = WGPULoadOp_Clear;
        ca.storeOp = WGPUStoreOp_Store;
        ca.clearValue = { 0, 0, 0, 1 };
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        if (g.bg && contentW > 0 && contentH > 0)
        {
            float sw = float(g.canvasW), sh = float(g.canvasH);
            float scale = std::min(sw / contentW, sh / contentH);
            float vw = contentW * scale, vh = contentH * scale;
            wgpuRenderPassEncoderSetViewport(pass, (sw - vw) / 2, (sh - vh) / 2, vw, vh, 0, 1);
            wgpuRenderPassEncoderSetPipeline(pass, g.pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, g.bg, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuQueueSubmit(g.queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(enc);
        wgpuTextureViewRelease(target);
        wgpuTextureRelease(st.texture);
        return true; // keep the loop going; the browser presents the canvas
    }

    void OnDeviceError(WGPUDevice const*, WGPUErrorType type, WGPUStringView msg, void*, void*)
    {
        static int logged = 0;
        if (logged++ < 30)
            Log("display: WebGPU error %d: %.*s", int(type), int(std::min<size_t>(msg.length, 1500)), msg.data ? msg.data : "");
    }

    void SetUp()
    {
        g.queue = wgpuDeviceGetQueue(g.device);

        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector src = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
        src.selector = Str("#screen");
        WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
        sd.nextInChain = &src.chain;
        g.surface = wgpuInstanceCreateSurface(g.instance, &sd);
        WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
        if (wgpuSurfaceGetCapabilities(g.surface, g.adapter, &caps) == WGPUStatus_Success && caps.formatCount)
            g.format = caps.formats[0];

        WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
        wgsl.code = Str(kShader);
        WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        smd.nextInChain = &wgsl.chain;
        WGPUShaderModule module = wgpuDeviceCreateShaderModule(g.device, &smd);
        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = g.format;
        WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
        fs.module = module;
        fs.entryPoint = Str("fs");
        fs.targetCount = 1;
        fs.targets = &target;
        WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        pd.vertex.module = module;
        pd.vertex.entryPoint = Str("vs");
        pd.fragment = &fs;
        g.pipeline = wgpuDeviceCreateRenderPipeline(g.device, &pd);
        WGPUSamplerDescriptor samp = WGPU_SAMPLER_DESCRIPTOR_INIT;
        samp.magFilter = WGPUFilterMode_Linear;
        samp.minFilter = WGPUFilterMode_Linear;
        g.sampler = wgpuDeviceCreateSampler(g.device, &samp);
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.size = 16;
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        g.uv = wgpuDeviceCreateBuffer(g.device, &bd);

        int cw = 1280, ch = 720;
        emscripten_get_canvas_element_size("#screen", &cw, &ch);
        ConfigureSurface(cw, ch);
        if (!getenv("RENDER") || atoi(getenv("RENDER")) != 0)
            render::BackendInit(g.device, g.queue);
        emscripten_request_animation_frame_loop(OnAnimationFrame, nullptr);
        // Keys and mouse buttons are mapped by index.html (rebindable in its
        // settings) and arrive through crysis_control.
        Log("display: WebGPU drawing to <canvas id=\"screen\"> (surface format %d)", int(g.format));
    }

    // The GPU device can be lost (the browser's GPU process crashed or ran out
    // of memory, a driver reset). Say so on the page instead of leaving a
    // frozen picture; only a reload gets a new device.
    EM_JS(void, js_show_device_lost, (const char* why), {
        var d = document.createElement('div');
        d.textContent = 'The graphics device was lost (' + UTF8ToString(why >>> 0) + '). Reload the page to continue.';
        d.style.cssText = 'position:fixed;left:50%;top:40%;transform:translateX(-50%);padding:16px 22px;background:#300;' +
                          'color:#fff;font:15px system-ui,sans-serif;border:1px solid #a33;border-radius:8px;z-index:9999';
        document.body.appendChild(d);
    });

    void OnDeviceLost(WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView msg, void*, void*)
    {
        std::string why(msg.data ? msg.data : "", msg.data ? msg.length : 0);
        Log("display: WebGPU device lost (reason %d): %s", int(reason), why.c_str());
        if (reason != WGPUDeviceLostReason_Destroyed && reason != WGPUDeviceLostReason_CallbackCancelled)
            js_show_device_lost(why.empty() ? "unknown reason" : why.c_str());
    }

    void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView msg, void*, void*)
    {
        if (status != WGPURequestDeviceStatus_Success)
        {
            Log("display: WebGPU device request failed: %.*s", int(msg.length), msg.data ? msg.data : "");
            return;
        }
        g.device = device;
        SetUp();
    }

    void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView msg, void*, void*)
    {
        if (status != WGPURequestAdapterStatus_Success)
        {
            Log("display: WebGPU adapter request failed: %.*s", int(msg.length), msg.data ? msg.data : "");
            return;
        }
        g.adapter = adapter;
        WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
        dd.uncapturedErrorCallbackInfo.callback = OnDeviceError;
        dd.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        dd.deviceLostCallbackInfo.callback = OnDeviceLost;
        // Filterable 32-bit float textures hold resolved depth at full precision.
        // Timestamp queries are only used by GPU_PROFILE=1 (no cost otherwise).
        static WGPUFeatureName features[2];
        size_t featureCount = 0;
        if (wgpuAdapterHasFeature(adapter, WGPUFeatureName_Float32Filterable))
            features[featureCount++] = WGPUFeatureName_Float32Filterable;
        if (wgpuAdapterHasFeature(adapter, WGPUFeatureName_TimestampQuery))
            features[featureCount++] = WGPUFeatureName_TimestampQuery;
        dd.requiredFeatureCount = featureCount;
        dd.requiredFeatures = features;
        WGPURequestDeviceCallbackInfo ci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
        ci.mode = WGPUCallbackMode_AllowSpontaneous;
        ci.callback = OnDevice;
        wgpuAdapterRequestDevice(adapter, &dd, ci);
    }

    // Runs on the page's main thread.
    void StartOnMainThread()
    {
        g.instance = wgpuCreateInstance(nullptr);
        if (!g.instance)
        {
            Log("display: WebGPU is not available in this browser");
            return;
        }
        WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
        ao.powerPreference = WGPUPowerPreference_HighPerformance;
        WGPURequestAdapterCallbackInfo ci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
        ci.mode = WGPUCallbackMode_AllowSpontaneous;
        ci.callback = OnAdapter;
        wgpuInstanceRequestAdapter(g.instance, &ao, ci);
    }
}

// On-screen (touch) controls in index.html call these on the main thread.
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_touch_buttons(int mask, int down) { InputTouchButtons(uint16_t(mask), down != 0); }
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_touch_axis(int axis, float value) { InputTouchAxis(axis, value); }
// Key and mouse-button bindings from index.html: press/release a pad control (input.h).
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_control(int id, int down) { InputControl(id, down != 0); }
// Mouse look from index.html (pointer lock): movement in pixels, and settings.
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_mouse_move(float dx, float dy) { InputMouseMove(dx, dy); }
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_mouse_config(float sensitivity, int invertY) { InputMouseConfig(sensitivity, invertY != 0); }

void DisplayStart()
{
    emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, StartOnMainThread);
}

void DisplaySubmitFrame(const uint8_t* rgba, uint32_t w, uint32_t h, const char* title)
{
    std::lock_guard lock(g_mutex);
    g_pixels.assign(rgba, rgba + size_t(w) * h * 4);
    g_w = w;
    g_h = h;
    g_new = true;
    g_title = title ? title : "";
}

#endif // __EMSCRIPTEN__
