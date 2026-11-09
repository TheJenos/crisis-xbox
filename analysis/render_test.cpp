// End-to-end test of the draw backend (render_wgpu.cpp) with real game shaders.
//
// Builds a recorded frame by hand: clear (blue) -> draw a red triangle with the
// game's vs_3ab766e338a19a76 (float3 position + float4 color vertex fetch) and
// ps_cd002852f2b66aa6 (color passthrough) -> resolve to a texture. Then reads
// the resolved texture back and checks pixels inside and outside the triangle.
//
// usage: render_test <shader dir>
#include "render.h"
#include "render_backend.h"
#include "xenos_shader.h"

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static WGPUInstance g_instance;
static WGPUAdapter g_adapter;
static WGPUDevice g_device;

static std::vector<uint32_t> LoadWords(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> w(b.size() / 4);
    memcpy(w.data(), b.data(), w.size() * 4);
    return w;
}

static render::ShaderSource Translate(const std::string& path, uint64_t hash, bool pixel)
{
    auto words = LoadWords(path);
    xenos::Translation t = xenos::TranslateToWgsl(xenos::Decode(words.data(), words.size(), pixel));
    if (!t.error.empty()) { printf("translate %s: %s\n", path.c_str(), t.error.c_str()); exit(1); }
    render::ShaderSource s;
    s.hash = hash;
    s.pixel = pixel;
    s.wgsl = t.wgsl;
    s.textureMask = t.textureMask;
    memcpy(s.textureDimension, t.textureDimension, 32);
    return s;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: render_test <shader dir>\n"); return 1; }
    std::string dir = argv[1];

    g_instance = wgpuCreateInstance(nullptr);
    bool done = false;
    WGPURequestAdapterCallbackInfo aci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    aci.mode = WGPUCallbackMode_AllowProcessEvents;
    aci.callback = [](WGPURequestAdapterStatus s, WGPUAdapter a, WGPUStringView, void* d, void*) { if (s == WGPURequestAdapterStatus_Success) g_adapter = a; *(bool*)d = true; };
    aci.userdata1 = &done;
    WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    wgpuInstanceRequestAdapter(g_instance, &ao, aci);
    while (!done) wgpuInstanceProcessEvents(g_instance);
    done = false;
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType, WGPUStringView m, void*, void*) { printf("WebGPU error: %.*s\n", int(m.length), m.data); };
    WGPURequestDeviceCallbackInfo dci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dci.mode = WGPUCallbackMode_AllowProcessEvents;
    dci.callback = [](WGPURequestDeviceStatus s, WGPUDevice d, WGPUStringView, void* p, void*) { if (s == WGPURequestDeviceStatus_Success) g_device = d; *(bool*)p = true; };
    dci.userdata1 = &done;
    wgpuAdapterRequestDevice(g_adapter, &dd, dci);
    while (!done) wgpuInstanceProcessEvents(g_instance);
    WGPUQueue queue = wgpuDeviceGetQueue(g_device);
    render::BackendInit(g_device, queue);

    const uint32_t W = 1152, H = 720;
    auto f = std::make_unique<render::Frame>();
    f->shaders.push_back(Translate(dir + "/vs_3ab766e338a19a76.bin", 0x1111, false));
    f->shaders.push_back(Translate(dir + "/ps_cd002852f2b66aa6.bin", 0x2222, true));

    // Vertices: float3 position (pixels; w = 1 from the fetch swizzle) + float4 color, stride 7 dwords.
    float verts[3][7] = { { 100, 100, 0.5f, 1, 0, 0, 1 }, { 700, 100, 0.5f, 1, 0, 0, 1 }, { 100, 600, 0.5f, 1, 0, 0, 1 } };
    f->vertexData.resize(21);
    memcpy(f->vertexData.data(), verts, sizeof(verts));

    // Uniforms: zero float constants, then the misc block (viewport transform for pixel coordinates).
    f->uniforms.assign(render::kConstBytes, 0);
    xenos::MiscConstants misc{};
    misc.positionScale[0] = 2.0f / W; misc.positionOffset[0] = -1.0f;
    misc.positionScale[1] = -2.0f / H; misc.positionOffset[1] = 1.0f;
    misc.positionScale[2] = 1.0f;
    misc.alphaRef[1] = 7.0f;
    misc.vertexBase[0] = 0;
    size_t miscOffset = f->uniforms.size();
    f->uniforms.resize(miscOffset + 768);
    memcpy(f->uniforms.data() + miscOffset, &misc, sizeof(misc));

    render::RtKey color; color.base = 0; color.pitch = W; color.format = render::RT_RGBA8;
    render::RtKey depth; depth.base = 400; depth.pitch = W; depth.format = render::RT_DEPTH24S8;

    // 1. clear color (blue) and depth
    render::ResolveCmd clear{};
    clear.src = color; clear.rtHeight = H; clear.copy = false;
    clear.clearColor = color; clear.clearValue[0] = 0; clear.clearValue[1] = 0; clear.clearValue[2] = 1; clear.clearValue[3] = 1;
    clear.clearDepth = depth; clear.depthValue = 1.0f;
    f->cmds.push_back({ render::Cmd::Resolve, 0 });
    f->resolves.push_back(clear);

    // 2. draw
    render::DrawCmd d{};
    memset(&d.pipe, 0, sizeof(d.pipe));
    d.pipe.vs = 0x1111; d.pipe.ps = 0x2222;
    d.pipe.topology = render::TOP_TRIANGLES;
    for (int i = 0; i < 4; i++) d.pipe.colorFormat[i] = render::kNone;
    d.pipe.colorFormat[0] = render::RT_RGBA8;
    d.pipe.writeMask[0] = 0xF;
    d.pipe.blend[0] = 0x00010001; // src one, dst zero, add (color and alpha)
    d.pipe.depthFormat = render::RT_DEPTH24S8;
    d.pipe.depthTest = 1; d.pipe.depthWrite = 1; d.pipe.depthFunc = 3; // less-equal
    d.color[0] = color; d.depth = depth; d.rtHeight = H;
    d.scissor[0] = 0; d.scissor[1] = 0; d.scissor[2] = W; d.scissor[3] = H;
    d.constOffset = 0; d.miscOffset = uint32_t(miscOffset);
    d.indexed = false; d.first = 0; d.count = 3;
    f->cmds.push_back({ render::Cmd::Draw, 0 });
    f->draws.push_back(d);

    // 3. resolve into texture 1 and present it
    render::ResolveCmd rs{};
    rs.src = color; rs.rtHeight = H; rs.x = 0; rs.y = 0; rs.w = W; rs.h = H;
    rs.destId = 1; rs.destW = W; rs.destH = H; rs.copy = true;
    f->cmds.push_back({ render::Cmd::Resolve, 1 });
    f->resolves.push_back(rs);
    f->presentId = 1; f->presentW = W; f->presentH = H;

    render::SubmitFrame(std::move(f));
    uint32_t sw, sh, tw, th;
    render::BackendRunPending(sw, sh, tw, th);
    uint32_t w, h; bool isFloat; uint64_t frames;
    WGPUTexture tex = render::BackendPresentTexture(w, h, isFloat, frames);
    if (!tex) { printf("FAIL: nothing presented\n"); return 2; }

    uint32_t row = (w * 4 + 255) & ~255u;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = uint64_t(row) * h;
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(g_device, &bd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = tex;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buf;
    dst.layout.bytesPerRow = row;
    dst.layout.rowsPerImage = h;
    WGPUExtent3D size = { w, h, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &size);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue, 1, &cmd);
    done = false;
    WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mci.mode = WGPUCallbackMode_AllowProcessEvents;
    mci.callback = [](WGPUMapAsyncStatus, WGPUStringView, void* d, void*) { *(bool*)d = true; };
    mci.userdata1 = &done;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mci);
    while (!done) { wgpuDevicePoll(g_device, true, nullptr); wgpuInstanceProcessEvents(g_instance); }
    const uint8_t* p = (const uint8_t*)wgpuBufferGetConstMappedRange(buf, 0, bd.size);
    auto px = [&](uint32_t x, uint32_t y) { const uint8_t* q = p + size_t(y) * row + x * 4; return std::vector<int>{ q[0], q[1], q[2], q[3] }; };
    struct Check { uint32_t x, y; std::vector<int> want; const char* what; } checks[] = {
        { 150, 150, { 255, 0, 0, 255 }, "inside the triangle (red)" },
        { 650, 120, { 255, 0, 0, 255 }, "inside, near the right corner (red)" },
        { 600, 500, { 0, 0, 255, 255 }, "outside, past the hypotenuse (blue clear)" },
        { 1100, 700, { 0, 0, 255, 255 }, "corner (blue clear)" },
        { 50, 50, { 0, 0, 255, 255 }, "above-left of the triangle (blue clear)" },
    };
    int failures = 0;
    for (auto& c : checks)
    {
        auto got = px(c.x, c.y);
        bool ok = got == c.want;
        failures += !ok;
        printf("%s (%u,%u) %s: got %d,%d,%d,%d\n", ok ? "ok  " : "FAIL", c.x, c.y, c.what, got[0], got[1], got[2], got[3]);
    }
    printf("%s: %d of %zu checks failed\n", failures ? "FAILED" : "PASSED", failures, std::size(checks));
    return failures ? 2 : 0;
}
