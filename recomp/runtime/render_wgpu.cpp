// Executes recorded frames (render.h) with WebGPU (webgpu.h).
//
// EDRAM surfaces become render target textures keyed by (base, pitch,
// format); all surfaces with the same pitch share one height so they can be
// attached together. Consecutive draws to the same surfaces share a render
// pass. Resolves copy a surface into a sampled texture with a small blit
// pipeline (textureLoad, so any float format works), then apply clears.
#include "render.h"
#include "render_backend.h"
#include "guest.h"
#include "xenos_texture.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <unordered_map>

namespace render
{
    namespace
    {
        WGPUStringView Str(const char* s) { return { s, strlen(s) }; }

        // ---------------------------------------------------------------
        // Frame queue (any thread -> backend thread)
        // ---------------------------------------------------------------
        std::mutex g_queueMutex;
        std::condition_variable g_queueCv;
        std::deque<std::unique_ptr<Frame>> g_queue;
        std::atomic<bool> g_ready{ false };
        constexpr size_t kMaxQueued = 3;

        struct Backend
        {
            WGPUDevice device = nullptr;
            WGPUQueue queue = nullptr;
            WGPUBindGroupLayout bgl0 = nullptr;
            WGPUBindGroupLayout emptyBgl = nullptr;
            WGPUBindGroup emptyBg = nullptr;

            // Validity of created objects, reported by error scopes (asynchronously
            // in the browser): 0 pending, 1 valid, 2 invalid.
            using Status = std::shared_ptr<std::atomic<int>>;
            struct Shader { WGPUShaderModule module; bool pixel; uint32_t textureMask; uint8_t dims[32]; Status status; };
            std::unordered_map<uint64_t, Shader> shaders;

            // A pipeline being compiled asynchronously: the callback fills this in.
            struct PipeAsync { std::atomic<int> status{ 0 }; std::atomic<WGPURenderPipeline> pipeline{ nullptr }; uint64_t vs = 0, ps = 0; };
            struct Pipe { PipelineKey key; WGPURenderPipeline pipeline; WGPUBindGroupLayout bgl1; uint32_t mask; uint8_t dims[32]; Status status; std::shared_ptr<PipeAsync> async; };
            std::unordered_map<uint64_t, std::vector<Pipe>> pipelines;
            std::map<std::vector<uint8_t>, std::pair<WGPUBindGroupLayout, WGPUPipelineLayout>> textureLayouts;

            struct Tex { WGPUTexture texture = nullptr; WGPUTextureView view = nullptr, depthView = nullptr; uint32_t w = 0, h = 0, layers = 1, mips = 1; WGPUTextureFormat format{};
                         uint32_t depth = 1; bool is3d = false; // is3d: a 3D texture of `depth` slices
                         uint32_t nw = 0, nh = 0; };            // render targets: guest size in samples (w, h: host size, see TargetScale)
            std::unordered_map<uint32_t, Tex> textures;   // uploads and resolve destinations, by id
            struct RtKeyHash { size_t operator()(const RtKey& k) const { return k.base * 131 + k.pitch * 7 + k.format; } };
            std::unordered_map<RtKey, Tex, RtKeyHash> targets;
            std::unordered_map<uint32_t, uint32_t> pitchHeight;
            Tex placeholder2d, placeholder3d, placeholderCube;
            std::unordered_map<uint16_t, WGPUSampler> samplers;

            // Per-frame buffers (grown as needed, rewritten every frame).
            WGPUBuffer uniforms = nullptr, vertices = nullptr, indices = nullptr, resolveParams = nullptr;
            uint64_t uniformsSize = 0, verticesSize = 0, indicesSize = 0, resolveParamsSize = 0;
            WGPUBindGroup bg0 = nullptr;

            // Resolve blit
            WGPUBindGroupLayout blitBgl = nullptr;
            WGPUPipelineLayout blitLayout = nullptr;
            WGPURenderPipeline blitPipeline[3] = {}; // RGBA8, RGBA16F, R32F destination
            bool float32Filterable = false;

            uint32_t presentId = 0, presentW = 0, presentH = 0;
            uint64_t frames = 0, pipelineCount = 0, drawsExecuted = 0;
            std::atomic<uint32_t> pipelinesCompiling{ 0 }; // async pipeline creations not finished yet
            uint64_t drawsWaiting = 0;         // draws skipped because their pipeline was still compiling

            // Bind groups reused across frames: texture groups keyed by layout +
            // (view, sampler) per slot, resolve groups by source view. Creating
            // one per draw was the backend's biggest cost in the browser. Any
            // texture release clears both (a new view can reuse a freed pointer).
            struct KeyHash
            {
                size_t operator()(const std::vector<uintptr_t>& k) const
                {
                    uint64_t h = 0xCBF29CE484222325ull;
                    for (uintptr_t v : k) h = (h ^ uint64_t(v)) * 0x100000001B3ull;
                    return size_t(h ^ (h >> 32));
                }
            };
            std::unordered_map<std::vector<uintptr_t>, WGPUBindGroup, KeyHash> textureGroups;
            std::unordered_map<WGPUTextureView, WGPUBindGroup> resolveGroups;
            uint64_t groupsCreated = 0, groupsReused = 0;
        } g;

        void ClearBindGroupCaches()
        {
            for (auto& [k, bg] : g.textureGroups) wgpuBindGroupRelease(bg);
            for (auto& [k, bg] : g.resolveGroups) wgpuBindGroupRelease(bg);
            g.textureGroups.clear();
            g.resolveGroups.clear();
        }

        // Error scopes around object creation: invalid shaders and pipelines are
        // skipped instead of poisoning (and on wgpu-native, aborting) the submit.
        struct ScopeJob { Backend::Status status; std::string what; };
        void OnScope(WGPUPopErrorScopeStatus st, WGPUErrorType type, WGPUStringView msg, void* ud, void*)
        {
            auto* job = (ScopeJob*)ud;
            bool bad = st == WGPUPopErrorScopeStatus_Success && type != WGPUErrorType_NoError;
            job->status->store(bad ? 2 : 1);
            static std::atomic<int> logged{ 0 };
            if (bad && logged++ < 20)
                Log("render: %s failed: %.*s", job->what.c_str(), int(std::min<size_t>(msg.length, 1500)), msg.data ? msg.data : "");
            delete job;
        }
        Backend::Status BeginScope()
        {
            wgpuDevicePushErrorScope(g.device, WGPUErrorFilter_Validation);
            return std::make_shared<std::atomic<int>>(0);
        }
        void EndScope(const Backend::Status& status, std::string what)
        {
            WGPUPopErrorScopeCallbackInfo ci = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
            ci.mode = WGPUCallbackMode_AllowSpontaneous;
            ci.callback = OnScope;
            ci.userdata1 = new ScopeJob{ status, std::move(what) };
            wgpuDevicePopErrorScope(g.device, ci);
        }

        uint64_t HashBytes(const void* p, size_t n)
        {
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull;
            return h;
        }

        WGPUTextureFormat HostFormat(uint8_t f)
        {
            switch (f)
            {
            case RT_RGB10A2: return WGPUTextureFormat_RGB10A2Unorm;
            case RT_RGBA16F: return WGPUTextureFormat_RGBA16Float;
            case RT_RG16F: return WGPUTextureFormat_RG16Float;
            case RT_R32F: return WGPUTextureFormat_R32Float;
            case RT_RG32F: return WGPUTextureFormat_RG32Float;
            case RT_DEPTH24S8: return WGPUTextureFormat_Depth24PlusStencil8;
            default: return WGPUTextureFormat_RGBA8Unorm;
            }
        }
        bool Blendable(uint8_t f) { return f != RT_R32F && f != RT_RG32F; }

        WGPUCompareFunction Compare(uint32_t f)
        {
            static const WGPUCompareFunction m[8] = { WGPUCompareFunction_Never, WGPUCompareFunction_Less, WGPUCompareFunction_Equal,
                WGPUCompareFunction_LessEqual, WGPUCompareFunction_Greater, WGPUCompareFunction_NotEqual,
                WGPUCompareFunction_GreaterEqual, WGPUCompareFunction_Always };
            return m[f & 7];
        }
        WGPUStencilOperation StencilOp(uint32_t o)
        {
            static const WGPUStencilOperation m[8] = { WGPUStencilOperation_Keep, WGPUStencilOperation_Zero, WGPUStencilOperation_Replace,
                WGPUStencilOperation_IncrementClamp, WGPUStencilOperation_DecrementClamp, WGPUStencilOperation_Invert,
                WGPUStencilOperation_IncrementWrap, WGPUStencilOperation_DecrementWrap };
            return m[o & 7];
        }
        WGPUBlendFactor BlendFactor(uint32_t f)
        {
            switch (f)
            {
            case 0: return WGPUBlendFactor_Zero;
            case 1: return WGPUBlendFactor_One;
            case 4: return WGPUBlendFactor_Src;
            case 5: return WGPUBlendFactor_OneMinusSrc;
            case 6: return WGPUBlendFactor_SrcAlpha;
            case 7: return WGPUBlendFactor_OneMinusSrcAlpha;
            case 8: return WGPUBlendFactor_Dst;
            case 9: return WGPUBlendFactor_OneMinusDst;
            case 10: return WGPUBlendFactor_DstAlpha;
            case 11: return WGPUBlendFactor_OneMinusDstAlpha;
            case 12: case 14: return WGPUBlendFactor_Constant;
            case 13: case 15: return WGPUBlendFactor_OneMinusConstant;
            case 16: return WGPUBlendFactor_SrcAlphaSaturated;
            default: return WGPUBlendFactor_One;
            }
        }
        WGPUBlendComponent BlendComponent(uint32_t src, uint32_t op, uint32_t dst)
        {
            WGPUBlendComponent c = WGPU_BLEND_COMPONENT_INIT;
            static const WGPUBlendOperation ops[8] = { WGPUBlendOperation_Add, WGPUBlendOperation_Subtract, WGPUBlendOperation_Min,
                WGPUBlendOperation_Max, WGPUBlendOperation_ReverseSubtract, WGPUBlendOperation_Add, WGPUBlendOperation_Add, WGPUBlendOperation_Add };
            c.operation = ops[op & 7];
            bool minMax = c.operation == WGPUBlendOperation_Min || c.operation == WGPUBlendOperation_Max;
            c.srcFactor = minMax ? WGPUBlendFactor_One : BlendFactor(src);
            c.dstFactor = minMax ? WGPUBlendFactor_One : BlendFactor(dst);
            return c;
        }

        WGPUTexture MakeTexture(uint32_t w, uint32_t h, uint32_t layers, WGPUTextureFormat f, WGPUTextureUsage usage,
                                WGPUTextureDimension dim = WGPUTextureDimension_2D, uint32_t mips = 1)
        {
            WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
            td.size = { w, h, layers };
            td.mipLevelCount = mips;
            td.format = f;
            td.usage = usage;
            td.dimension = dim;
            // Large allocations are logged (memory jumps of several GB were seen).
            uint32_t bpp = f == WGPUTextureFormat_RGBA16Float ? 8 : f == WGPUTextureFormat_RGBA32Float ? 16 : 4;
            uint64_t bytes = uint64_t(w) * h * layers * bpp;
            if (bytes >= (32ull << 20))
            {
                static int logged = 0;
                if (logged++ < 40)
                    Log("render: large texture %ux%ux%u format %d (%llu MB)", w, h, layers, int(f), (unsigned long long)(bytes >> 20));
            }
            return wgpuDeviceCreateTexture(g.device, &td);
        }

        void ReleaseTex(Backend::Tex& t)
        {
            if (t.view || t.depthView)
                ClearBindGroupCaches();
            if (t.view) wgpuTextureViewRelease(t.view);
            if (t.depthView) wgpuTextureViewRelease(t.depthView);
            if (t.texture) wgpuTextureRelease(t.texture);
            t = {};
        }

        void MakePlaceholders()
        {
            static const uint8_t white[4 * 6] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                                  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 };
            auto fill = [&](Backend::Tex& t, uint32_t layers, WGPUTextureDimension dim, WGPUTextureViewDimension vdim, const uint8_t* data) {
                t.texture = MakeTexture(1, 1, layers, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, dim);
                WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                dst.texture = t.texture;
                WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
                layout.bytesPerRow = 4;
                layout.rowsPerImage = 1;
                WGPUExtent3D size = { 1, 1, layers };
                wgpuQueueWriteTexture(g.queue, &dst, data, 4 * layers, &layout, &size);
                WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                vd.dimension = vdim;
                t.view = wgpuTextureCreateView(t.texture, &vd);
                t.w = t.h = 1;
            };
            fill(g.placeholder2d, 1, WGPUTextureDimension_2D, WGPUTextureViewDimension_2D, white);
            fill(g.placeholder3d, 1, WGPUTextureDimension_3D, WGPUTextureViewDimension_3D, white);
            // Missing cube maps (environment light, reflections) add nothing rather than white.
            static const uint8_t black[4 * 6] = {};
            fill(g.placeholderCube, 6, WGPUTextureDimension_2D, WGPUTextureViewDimension_Cube, black);
        }

        WGPUSampler Sampler(uint16_t bits)
        {
            auto it = g.samplers.find(bits);
            if (it != g.samplers.end())
                return it->second;
            auto address = [](uint32_t c) {
                switch (c & 7)
                {
                case 0: return WGPUAddressMode_Repeat;
                case 1: case 3: case 5: case 7: return WGPUAddressMode_MirrorRepeat;
                default: return WGPUAddressMode_ClampToEdge;
                }
            };
            auto filter = [](uint32_t f) { return f == 0 ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear; };
            WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
            sd.addressModeU = address(bits);
            sd.addressModeV = address(bits >> 3);
            sd.addressModeW = address(bits >> 6);
            sd.magFilter = filter((bits >> 9) & 3);
            sd.minFilter = filter((bits >> 11) & 3);
            // Mip filter: 0 point, 1 linear, 2 base level only.
            uint32_t mipFilter = (bits >> 13) & 3;
            sd.mipmapFilter = mipFilter == 1 ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
            if (mipFilter == 2)
                sd.lodMaxClamp = 0.0f;
            WGPUSampler s = wgpuDeviceCreateSampler(g.device, &sd);
            g.samplers[bits] = s;
            return s;
        }

        void CreateFixedLayouts()
        {
            WGPUBindGroupLayoutEntry e[3] = { WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT };
            e[0].binding = 0;
            e[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            e[0].buffer.type = WGPUBufferBindingType_Uniform;
            e[0].buffer.hasDynamicOffset = true;
            e[0].buffer.minBindingSize = kConstBytes;
            e[1] = e[0];
            e[1].binding = 1;
            e[1].buffer.minBindingSize = kMiscBytes;
            e[2].binding = 2;
            e[2].visibility = WGPUShaderStage_Vertex;
            e[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
            WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            d.entryCount = 3;
            d.entries = e;
            g.bgl0 = wgpuDeviceCreateBindGroupLayout(g.device, &d);

            WGPUBindGroupLayoutDescriptor ed = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            g.emptyBgl = wgpuDeviceCreateBindGroupLayout(g.device, &ed);
            WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bd.layout = g.emptyBgl;
            g.emptyBg = wgpuDeviceCreateBindGroup(g.device, &bd);

            // Resolve blit: source surface (any float format, read with textureLoad)
            // and a uniform with the source offset.
            WGPUBindGroupLayoutEntry be[2] = { WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT };
            be[0].binding = 0;
            be[0].visibility = WGPUShaderStage_Fragment;
            be[0].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
            be[0].texture.viewDimension = WGPUTextureViewDimension_2D;
            be[1].binding = 1;
            be[1].visibility = WGPUShaderStage_Fragment;
            be[1].buffer.type = WGPUBufferBindingType_Uniform;
            be[1].buffer.hasDynamicOffset = true;
            be[1].buffer.minBindingSize = 64;
            WGPUBindGroupLayoutDescriptor bld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            bld.entryCount = 2;
            bld.entries = be;
            g.blitBgl = wgpuDeviceCreateBindGroupLayout(g.device, &bld);
            WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            pld.bindGroupLayoutCount = 1;
            pld.bindGroupLayouts = &g.blitBgl;
            g.blitLayout = wgpuDeviceCreatePipelineLayout(g.device, &pld);

            static const char* kBlit = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
struct P { offset: vec4i, mode: vec4i, mul: vec4f, maxv: vec4f, }
@group(0) @binding(1) var<uniform> params: P;
// 7e3 (3-bit exponent biased by 3, 7-bit mantissa) bit pattern of a value,
// as an integer 0..1023: exponent * 128 + mantissa (rounding carries over).
fn to7e3(v: f32) -> f32 {
    let x = clamp(v, 0.0, 31.875);
    if (x < 0.25) { return round(x * 512.0); }
    let e = floor(log2(x));
    let m = round((x / exp2(e) - 1.0) * 128.0);
    return min((e + 3.0) * 128.0 + m, 1023.0);
}
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    return vec4f(p[i], 0.0, 1.0);
}
fn load(p: vec2i, size: vec2i) -> vec4f {
    return textureLoad(src, clamp(p, vec2i(0), size - 1), 0);
}
@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let s = params.offset.zw;
    let c = (vec2i(pos.xy) + params.offset.xy) * s;
    let size = vec2i(textureDimensions(src));
    var v = vec4f(0.0);
    if (params.mode.z == 65536 && params.mode.w == 65536) {
        var sum = vec4f(0.0);
        for (var y = 0; y < s.y; y++) {
            for (var x = 0; x < s.x; x++) {
                sum += load(c + vec2i(x, y), size);
            }
        }
        v = sum / f32(s.x * s.y);
    } else {
        // A target rendered at a reduced size (render::TargetScale), or a
        // destination stored at one: the destination texel's area in source
        // samples (guest units), read at its centre in the source's texels.
        // Depth and integers take the nearest texel; colours are filtered
        // between the four nearest.
        let k = f32(params.mode.z) / 65536.0;
        let g = pos.xy / (f32(params.mode.w) / 65536.0); // guest destination position
        let centre = (g + vec2f(params.offset.xy)) * vec2f(s) * k;
        if (params.mode.x != 0) {
            v = load(vec2i(floor(centre)), size); // (filtering would invent depths and integers)
        } else {
            let p = centre - 0.5;
            let i = vec2i(floor(p));
            let f = p - floor(p);
            v = mix(mix(load(i, size), load(i + vec2i(1, 0), size), f.x),
                    mix(load(i + vec2i(0, 1), size), load(i + vec2i(1, 1), size), f.x), f.y);
        }
    }
    // No NaN or infinity reaches later passes; 7e3 surfaces hold [0, 31.875].
    v = select(clamp(v, vec4f(-65504.0), vec4f(65504.0)), vec4f(0.0), v != v);
    if (params.mode.y == 1) {
        v = vec4f(clamp(v.xyz, vec3f(0.0), vec3f(31.875)), clamp(v.w, 0.0, 1.0));
    }
    if (params.mode.y == 2) {
        // A 7e3 surface resolved while the target is 10:10:10:2: the hardware
        // copies the raw 7e3 bits, which shaders decode by hand (Crysis 2's
        // light buffers). Stored as those integers.
        return vec4f(to7e3(v.x), to7e3(v.y), to7e3(v.z), round(clamp(v.w, 0.0, 1.0) * 3.0));
    }
    if (params.mode.x == 2) {
        return vec4f(v.x); // depth, in every channel
    }
    if (params.mode.x == 1) {
        // Fixed-point destination (integer number format with an exponent
        // bias): the stored integers themselves, round(value * 2^bias),
        // clamped to each channel's range (exact in RGBA16F up to 2048).
        return clamp(round(v * params.mul), vec4f(0.0), params.maxv);
    }
    return v;
}
)";
            WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
            wgsl.code = Str(kBlit);
            WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            smd.nextInChain = &wgsl.chain;
            WGPUShaderModule m = wgpuDeviceCreateShaderModule(g.device, &smd);
            for (int i = 0; i < 3; i++)
            {
                WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
                target.format = i == 2 ? WGPUTextureFormat_R32Float : i ? WGPUTextureFormat_RGBA16Float : WGPUTextureFormat_RGBA8Unorm;
                WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
                fs.module = m;
                fs.entryPoint = Str("fs");
                fs.targetCount = 1;
                fs.targets = &target;
                WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
                pd.layout = g.blitLayout;
                pd.vertex.module = m;
                pd.vertex.entryPoint = Str("vs");
                pd.fragment = &fs;
                g.blitPipeline[i] = wgpuDeviceCreateRenderPipeline(g.device, &pd);
            }
            wgpuShaderModuleRelease(m);
        }

        // Translated shader sources by low 32 hash bits (GPU_PROFILE, crysis_shader_wgsl).
        std::unordered_map<uint32_t, std::string> g_shaderText;

        void AddShader(const ShaderSource& s)
        {
            if (g.shaders.count(s.hash))
                return;
            g_shaderText[uint32_t(s.hash)] = s.wgsl; // for GPU_PROFILE and crysis_shader_wgsl
            WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
            wgsl.code = { s.wgsl.data(), s.wgsl.size() };
            WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            smd.nextInChain = &wgsl.chain;
            Backend::Status status = BeginScope();
            Backend::Shader sh{ wgpuDeviceCreateShaderModule(g.device, &smd), s.pixel, s.textureMask, {}, status };
            char what[64];
            snprintf(what, sizeof(what), "%s shader %016llx", s.pixel ? "pixel" : "vertex", (unsigned long long)s.hash);
            EndScope(status, what);
            memcpy(sh.dims, s.textureDimension, 32);
            g.shaders[s.hash] = sh;
        }

        WGPUTextureViewDimension ViewDim(uint8_t d)
        {
            return d == 2 ? WGPUTextureViewDimension_3D : d == 3 ? WGPUTextureViewDimension_Cube : WGPUTextureViewDimension_2D;
        }

        // Group 1 layout for a texture set: bindings 2n (texture) / 2n+1 (sampler).
        std::pair<WGPUBindGroupLayout, WGPUPipelineLayout> TextureLayout(uint32_t mask, const uint8_t* dims)
        {
            std::vector<uint8_t> key(36);
            memcpy(key.data(), &mask, 4);
            for (int i = 0; i < 32; i++) key[4 + i] = ((mask >> i) & 1) ? dims[i] : 0;
            auto it = g.textureLayouts.find(key);
            if (it != g.textureLayouts.end())
                return it->second;
            std::vector<WGPUBindGroupLayoutEntry> e;
            for (uint32_t s = 0; s < 32; s++)
            {
                if (!((mask >> s) & 1)) continue;
                WGPUBindGroupLayoutEntry t = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                t.binding = s * 2;
                t.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
                t.texture.sampleType = WGPUTextureSampleType_Float;
                t.texture.viewDimension = ViewDim(dims[s]);
                e.push_back(t);
                WGPUBindGroupLayoutEntry sm = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                sm.binding = s * 2 + 1;
                sm.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
                sm.sampler.type = WGPUSamplerBindingType_Filtering;
                e.push_back(sm);
            }
            WGPUBindGroupLayout bgl = g.emptyBgl;
            if (!e.empty())
            {
                WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
                d.entryCount = e.size();
                d.entries = e.data();
                bgl = wgpuDeviceCreateBindGroupLayout(g.device, &d);
            }
            WGPUBindGroupLayout groups[2] = { g.bgl0, bgl };
            WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            pld.bindGroupLayoutCount = 2;
            pld.bindGroupLayouts = groups;
            auto result = std::make_pair(bgl, wgpuDeviceCreatePipelineLayout(g.device, &pld));
            g.textureLayouts[key] = result;
            return result;
        }

        const Backend::Pipe* Pipeline(const PipelineKey& k)
        {
            uint64_t h = HashBytes(&k, sizeof(k));
            auto& bucket = g.pipelines[h];
            for (auto& p : bucket)
                if (!memcmp(&p.key, &k, sizeof(k)))
                {
                    if (p.async)
                    {
                        int s = p.async->status.load();
                        if (s == 0)
                        {
                            g.drawsWaiting++;
                            return nullptr; // still compiling: skip the draw this frame
                        }
                        p.pipeline = p.async->pipeline.load();
                        p.status->store(s);
                        p.async.reset();
                    }
                    return p.pipeline && p.status->load() != 2 ? &p : nullptr;
                }
            auto vs = g.shaders.find(k.vs), ps = g.shaders.find(k.ps);
            Backend::Pipe pipe{};
            pipe.key = k;
            pipe.status = std::make_shared<std::atomic<int>>(2);
            if (vs == g.shaders.end() || ps == g.shaders.end() || vs->second.status->load() == 2 || ps->second.status->load() == 2)
            {
                bucket.push_back(pipe);
                return nullptr;
            }
            pipe.mask = vs->second.textureMask | ps->second.textureMask;
            for (int i = 0; i < 32; i++)
                pipe.dims[i] = ((ps->second.textureMask >> i) & 1) ? ps->second.dims[i] : vs->second.dims[i];
            auto layout = TextureLayout(pipe.mask, pipe.dims);
            pipe.bgl1 = layout.first;

            WGPUColorTargetState targets[4];
            WGPUBlendState blends[4];
            uint32_t targetCount = 0;
            for (int i = 0; i < 4; i++)
            {
                targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
                if (k.colorFormat[i] == kNone)
                {
                    targets[i].format = WGPUTextureFormat_Undefined;
                    targets[i].writeMask = WGPUColorWriteMask_None;
                    continue;
                }
                targetCount = i + 1;
                targets[i].format = HostFormat(k.colorFormat[i]);
                targets[i].writeMask = k.writeMask[i];
                uint32_t b = k.blend[i];
                bool enabled = !((b & 0x1F) == 1 && ((b >> 5) & 7) == 0 && ((b >> 8) & 0x1F) == 0 &&
                                 ((b >> 16) & 0x1F) == 1 && ((b >> 21) & 7) == 0 && ((b >> 24) & 0x1F) == 0);
                if (enabled && Blendable(k.colorFormat[i]))
                {
                    blends[i].color = BlendComponent(b & 0x1F, (b >> 5) & 7, (b >> 8) & 0x1F);
                    blends[i].alpha = BlendComponent((b >> 16) & 0x1F, (b >> 21) & 7, (b >> 24) & 0x1F);
                    targets[i].blend = &blends[i];
                }
            }

            WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
            fs.module = ps->second.module;
            fs.entryPoint = Str("main");
            fs.targetCount = targetCount;
            fs.targets = targets;
            // Fixed-function state the pixel shader folds in (xenos_wgsl.cpp overrides).
            WGPUConstantEntry constants[3] = {};
            const char* constantNames[3] = { "XE_ALPHA_FUNC", "XE_ALPHA_MASK", "XE_PGEN" };
            const double constantValues[3] = { double(k.alphaFunc), double(k.alphaToMask), k.pgenReg == 0xFF ? -1.0 : double(k.pgenReg) };
            for (int i = 0; i < 3; i++)
            {
                constants[i].key = Str(constantNames[i]);
                constants[i].value = constantValues[i];
            }
            fs.constantCount = 3;
            fs.constants = constants;

            WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
            ds.format = WGPUTextureFormat_Depth24PlusStencil8;
            ds.depthWriteEnabled = (k.depthTest && k.depthWrite) ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            ds.depthCompare = k.depthTest ? Compare(k.depthFunc) : WGPUCompareFunction_Always;
            if (k.stencilEnable)
            {
                auto face = [](uint32_t ops) {
                    WGPUStencilFaceState f = WGPU_STENCIL_FACE_STATE_INIT;
                    f.compare = Compare(ops & 7);
                    f.failOp = StencilOp(ops >> 3);
                    f.passOp = StencilOp(ops >> 6);
                    f.depthFailOp = StencilOp(ops >> 9);
                    return f;
                };
                ds.stencilFront = face(k.stencilOps & 0xFFF);
                ds.stencilBack = face(k.stencilOps >> 12);
                ds.stencilReadMask = k.stencilReadMask;
                ds.stencilWriteMask = k.stencilWriteMask;
            }
            else
            {
                ds.stencilReadMask = 0;
                ds.stencilWriteMask = 0;
            }

            WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            pd.layout = layout.second;
            pd.vertex.module = vs->second.module;
            pd.vertex.entryPoint = Str(k.rectList ? "main_rect" : "main");
            static const WGPUPrimitiveTopology topo[5] = { WGPUPrimitiveTopology_PointList, WGPUPrimitiveTopology_LineList,
                WGPUPrimitiveTopology_LineStrip, WGPUPrimitiveTopology_TriangleList, WGPUPrimitiveTopology_TriangleStrip };
            pd.primitive.topology = topo[k.topology];
            pd.primitive.stripIndexFormat = k.stripIndexed ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Undefined;
            pd.primitive.frontFace = k.frontCW ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
            pd.primitive.cullMode = k.cull == 1 ? WGPUCullMode_Front : k.cull == 2 ? WGPUCullMode_Back : WGPUCullMode_None;
            pd.depthStencil = k.noDepthAttachment ? nullptr : &ds;
            pd.fragment = &fs;
#ifdef __EMSCRIPTEN__
            // Compile asynchronously. A synchronous createRenderPipeline makes the
            // browser's GPU process compile the shaders on its main thread: at a
            // level start (~300 new pipelines) that froze the GPU process, and so
            // every window of the browser, for ~18 s. Async creation compiles on
            // the GPU process's worker threads; draws skip the pipeline until then.
            {
                pipe.status = std::make_shared<std::atomic<int>>(0);
                pipe.pipeline = nullptr;
                pipe.async = std::make_shared<Backend::PipeAsync>();
                pipe.async->vs = k.vs;
                pipe.async->ps = k.ps;
                WGPUCreateRenderPipelineAsyncCallbackInfo ci = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
                ci.mode = WGPUCallbackMode_AllowSpontaneous;
                ci.callback = [](WGPUCreatePipelineAsyncStatus st, WGPURenderPipeline pl, WGPUStringView msg, void* ud, void*) {
                    auto* holder = (std::shared_ptr<Backend::PipeAsync>*)ud;
                    Backend::PipeAsync& a = **holder;
                    if (st == WGPUCreatePipelineAsyncStatus_Success && pl)
                    {
                        a.pipeline = pl;
                        a.status = 1;
                    }
                    else
                    {
                        if (pl)
                            wgpuRenderPipelineRelease(pl);
                        a.status = 2;
                        static std::atomic<int> logged{ 0 };
                        if (logged++ < 20)
                            Log("render: pipeline vs %016llx ps %016llx failed: %.*s", (unsigned long long)a.vs, (unsigned long long)a.ps,
                                int(std::min<size_t>(msg.length, 1500)), msg.data ? msg.data : "");
                    }
                    g.pipelinesCompiling--;
                    delete holder;
                };
                ci.userdata1 = new std::shared_ptr<Backend::PipeAsync>(pipe.async);
                wgpuDeviceCreateRenderPipelineAsync(g.device, &pd, ci);
                g.pipelineCount++;
                g.pipelinesCompiling++;
                g.drawsWaiting++;
                bucket.push_back(pipe);
                return nullptr;
            }
#endif
            pipe.status = BeginScope();
            pipe.pipeline = wgpuDeviceCreateRenderPipeline(g.device, &pd);
            char what[96];
            snprintf(what, sizeof(what), "pipeline vs %016llx ps %016llx", (unsigned long long)k.vs, (unsigned long long)k.ps);
            EndScope(pipe.status, what);
            g.pipelineCount++;
            bucket.push_back(pipe);
            return bucket.back().pipeline && bucket.back().status->load() != 2 ? &bucket.back() : nullptr;
        }

        // Host size of `n` guest samples in a target scaled by s (never smaller
        // than the scaled area, so a scaled viewport always fits).
        uint32_t HostSize(uint32_t n, float s) { return s == 1.0f ? n : std::max(1u, uint32_t(std::ceil(double(n) * s))); }

        // Render target texture for a surface, at the shared height for its pitch
        // (both in samples), scaled by TargetScale.
        Backend::Tex& Target(const RtKey& k, uint32_t height)
        {
            uint32_t& ph = g.pitchHeight[k.pitch];
            ph = std::max(ph, height);
            Backend::Tex& t = g.targets[k];
            if (!t.texture || t.nh != ph || t.nw != k.pitch)
            {
                ReleaseTex(t);
                float s = TargetScale(k.pitch);
                t.format = HostFormat(k.format);
                t.texture = MakeTexture(HostSize(k.pitch, s), HostSize(ph, s), 1, t.format,
                                        WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc);
                t.view = wgpuTextureCreateView(t.texture, nullptr);
                if (k.format == RT_DEPTH24S8)
                {
                    // Depth aspect alone, for depth resolves (read with textureLoad).
                    WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                    vd.aspect = WGPUTextureAspect_DepthOnly;
                    t.depthView = wgpuTextureCreateView(t.texture, &vd);
                }
                t.w = HostSize(k.pitch, s);
                t.h = HostSize(ph, s);
                t.nw = k.pitch;
                t.nh = ph;
            }
            return t;
        }

        void EnsureBuffer(WGPUBuffer& buf, uint64_t& size, uint64_t need, WGPUBufferUsage usage)
        {
            need = std::max<uint64_t>((need + 255) & ~255ull, 256);
            if (buf && size >= need)
                return;
            if (buf) wgpuBufferRelease(buf);
            size = std::max<uint64_t>(need, size * 2);
            if (size >= (64ull << 20))
                Log("render: per-frame buffer grown to %llu MB (needed %llu MB)", (unsigned long long)(size >> 20), (unsigned long long)(need >> 20));
            WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
            bd.size = size;
            bd.usage = usage | WGPUBufferUsage_CopyDst;
            buf = wgpuDeviceCreateBuffer(g.device, &bd);
            if (g.bg0) { wgpuBindGroupRelease(g.bg0); g.bg0 = nullptr; }
            ClearBindGroupCaches(); // cached resolve groups reference resolveParams
        }

        void Write(WGPUBuffer buf, const void* data, size_t size)
        {
            if (size)
                wgpuQueueWriteBuffer(g.queue, buf, 0, data, (size + 3) & ~size_t(3));
        }

        // ---------------------------------------------------------------
        // Frame execution
        // ---------------------------------------------------------------
        struct PassState
        {
            WGPURenderPassEncoder pass = nullptr;
            RtKey color[4];
            RtKey depth;
            uint32_t height = 0;             // guest height of the targets (samples)
            float scale = 1.0f;              // TargetScale of the targets
            uint32_t hostW = 0, hostH = 0;   // their host size
            bool indexBound = false;
            // Last state sent in this pass (stateValid false: nothing sent yet).
            bool stateValid = false;
            float vw = 0, vh = 0;
            WGPURenderPipeline pipeline = nullptr;
            WGPUBindGroup bg1 = nullptr;
            uint32_t constOffset = 0, miscOffset = 0, stencilRef = 0;
            uint32_t scissor[4] = {};
            float blend[4] = {};
            bool usesDepth = false; // a draw in this pass tests or writes depth/stencil
            bool noDepth = false;   // the pass has no depth attachment
        };

        // Which draws can go in a pass without a depth buffer: runs of draws to
        // the same targets (between resolves) in which no draw tests or writes
        // depth or stencil (post-processing, mostly). Phone GPUs load and store
        // every attachment of every pass, so a full-size depth buffer each time
        // cost them memory traffic for nothing. Indexed like f.cmds.
        std::vector<uint8_t> DepthlessRuns(const Frame& f)
        {
            std::vector<uint8_t> out(f.cmds.size(), 0);
            size_t start = 0;
            bool open = false, uses = false;
            const DrawCmd* first = nullptr;
            auto close = [&](size_t end) {
                if (open && !uses)
                    for (size_t j = start; j < end; j++)
                        if (f.cmds[j].type == Cmd::Draw) out[j] = 1;
                open = false;
            };
            for (size_t i = 0; i < f.cmds.size(); i++)
            {
                if (f.cmds[i].type != Cmd::Draw) { close(i); continue; }
                const DrawCmd& d = f.draws[f.cmds[i].index];
                bool same = open && first->depth == d.depth;
                for (int k = 0; k < 4 && same; k++)
                    same = first->color[k] == d.color[k];
                if (!same)
                {
                    close(i);
                    open = true; uses = false; start = i; first = &d;
                }
                if (d.pipe.depthTest || d.pipe.depthWrite || d.pipe.stencilEnable)
                    uses = true;
            }
            close(f.cmds.size());
            return out;
        }

        // Render passes per frame (phone GPUs pay for each pass and for loading
        // and storing its targets): logged with the backend timing.
        struct PassCounts { uint64_t draw = 0, drawNoDepth = 0, resolve = 0, resolveFull = 0, clear = 0; } g_passCounts;

        void EndPass(PassState& ps)
        {
            if (ps.pass)
            {
                g_passCounts.draw++;
                if (!ps.usesDepth) g_passCounts.drawNoDepth++;
                wgpuRenderPassEncoderEnd(ps.pass);
                wgpuRenderPassEncoderRelease(ps.pass);
                ps.pass = nullptr;
            }
        }

        std::vector<ResolveDump> g_resolveDumps;

        // GPU_PROFILE=1 (browser): GPU time of every render pass, from timestamp
        // queries, on one frame in 30. Measured frames give each draw its own
        // pass so draws can be told apart (attachments load and store, so the
        // picture is unchanged). Every 10 measured frames, log the costliest
        // shader/target combinations and the frame's total GPU time.
        struct GpuProfiler
        {
            static constexpr uint32_t kMaxPasses = 2048;
            bool checked = false, enabled = false, active = false, inFlight = false;
            WGPUQuerySet querySet = nullptr;
            WGPUBuffer resolve = nullptr, readback = nullptr;
            std::vector<std::string> labels, pending;
            WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
            uint64_t frameCounter = 0;
            uint32_t measured = 0;
            double frameMs = 0;
            struct Total { double ms = 0; uint64_t passes = 0; };
            std::map<std::string, Total> totals;

            void BeginFrame()
            {
                if (!checked)
                {
                    checked = true;
                    enabled = getenv("GPU_PROFILE") && wgpuDeviceHasFeature(g.device, WGPUFeatureName_TimestampQuery);
                    if (getenv("GPU_PROFILE") && !enabled)
                        Log("render: GPU_PROFILE needs WebGPU timestamp queries, which this browser doesn't offer");
                    if (enabled)
                    {
                        WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
                        qd.type = WGPUQueryType_Timestamp;
                        qd.count = kMaxPasses * 2;
                        querySet = wgpuDeviceCreateQuerySet(g.device, &qd);
                        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
                        bd.size = kMaxPasses * 16;
                        bd.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
                        resolve = wgpuDeviceCreateBuffer(g.device, &bd);
                        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
                        readback = wgpuDeviceCreateBuffer(g.device, &bd);
                    }
                }
                active = enabled && !inFlight && (frameCounter++ % 30) == 0;
                labels.clear();
            }

            // Timestamp writes for the next pass, or null when not measuring.
            const WGPUPassTimestampWrites* Stamp(std::string label)
            {
                if (!active || labels.size() >= kMaxPasses)
                    return nullptr;
                uint32_t i = uint32_t(labels.size());
                labels.push_back(std::move(label));
                writes.querySet = querySet;
                writes.beginningOfPassWriteIndex = i * 2;
                writes.endOfPassWriteIndex = i * 2 + 1;
                return &writes;
            }

            void EndFrame(WGPUCommandEncoder enc)
            {
                if (!active || labels.empty())
                    return;
                uint32_t n = uint32_t(labels.size());
                wgpuCommandEncoderResolveQuerySet(enc, querySet, 0, n * 2, resolve, 0);
                wgpuCommandEncoderCopyBufferToBuffer(enc, resolve, 0, readback, 0, uint64_t(n) * 16);
            }

            void AfterSubmit()
            {
                if (!active || labels.empty())
                    return;
                inFlight = true;
                pending = std::move(labels);
                WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
                ci.mode = WGPUCallbackMode_AllowSpontaneous;
                ci.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* self, void*) { ((GpuProfiler*)self)->OnMapped(status); };
                ci.userdata1 = this;
                wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, pending.size() * 16, ci);
            }

            void OnMapped(WGPUMapAsyncStatus status)
            {
                if (status == WGPUMapAsyncStatus_Success)
                {
                    const uint64_t* ts = (const uint64_t*)wgpuBufferGetConstMappedRange(readback, 0, pending.size() * 16);
                    double frame = 0;
                    for (size_t i = 0; ts && i < pending.size(); i++)
                    {
                        double ms = ts[i * 2 + 1] > ts[i * 2] ? double(ts[i * 2 + 1] - ts[i * 2]) / 1e6 : 0.0;
                        frame += ms;
                        Total& t = totals[pending[i]];
                        t.ms += ms;
                        t.passes++;
                    }
                    wgpuBufferUnmap(readback);
                    frameMs += frame;
                    if (++measured % 10 == 0)
                    {
                        std::vector<std::pair<double, std::string>> top;
                        for (auto& [label, t] : totals) top.push_back({ t.ms, label + " x" + std::to_string(t.passes) });
                        std::sort(top.rbegin(), top.rend());
                        Log("render: GPU time %.2f ms/frame (sum of passes, %u frames); costliest:", frameMs / 10, 10u);
                        for (size_t i = 0; i < top.size() && i < 16; i++)
                            Log("render:   %6.2f ms/frame  %s", top[i].first / 10, top[i].second.c_str());
                        // The first time a pixel shader tops the list, print its source.
                        static std::set<uint32_t> printed;
                        for (size_t i = 0; i < top.size() && i < 3; i++)
                        {
                            uint32_t ps = 0;
                            if (sscanf(top[i].second.c_str(), "draw ps %x", &ps) != 1 || !printed.insert(ps).second)
                                continue;
                            auto it = g_shaderText.find(ps);
                            if (it == g_shaderText.end())
                                continue;
                            Log("render: ---- pixel shader %08X (%zu chars) ----", ps, it->second.size());
                            const std::string& w = it->second;
                            for (size_t at = 0; at < w.size();)
                            {
                                size_t nl = w.find('\n', at);
                                size_t end = nl == std::string::npos ? w.size() : nl;
                                Log("WGSL %s", w.substr(at, std::min<size_t>(end - at, 400)).c_str());
                                at = end + 1;
                            }
                        }
                        totals.clear();
                        frameMs = 0;
                    }
                }
                pending.clear();
                inFlight = false;
            }
        } g_gpuProf;

        // Backpressure. Each executed frame uploads its vertex, constant and
        // texture data (tens of MB); WebGPU accepts submissions without limit
        // and the browser keeps that data until the GPU has run the frame. When
        // the GPU fell behind, the browser's GPU process grew by ~330 MB a
        // second until the machine ran out of memory. Frames now wait while
        // kMaxInFlight are unfinished (the recorder drops frames meanwhile).
        constexpr int kMaxInFlight = 2;
        std::atomic<int> g_inFlight{ 0 };
        // Submission times of the unfinished frames (completions arrive in
        // order), and the submit-to-done latency of the frames finished since
        // the last backend timing report.
        std::mutex g_flightMutex;
        std::deque<std::chrono::steady_clock::time_point> g_submitTimes;
        double g_latencySum = 0, g_latencyMax = 0;
        uint32_t g_latencyCount = 0;

        void TrackSubmitted()
        {
            {
                std::lock_guard lock(g_flightMutex);
                g_submitTimes.push_back(std::chrono::steady_clock::now());
            }
            g_inFlight++;
            WGPUQueueWorkDoneCallbackInfo ci = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            ci.mode = WGPUCallbackMode_AllowSpontaneous;
            ci.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void*, void*) {
                {
                    std::lock_guard lock(g_flightMutex);
                    if (!g_submitTimes.empty())
                    {
                        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_submitTimes.front()).count();
                        g_submitTimes.pop_front();
                        g_latencySum += ms;
                        g_latencyMax = std::max(g_latencyMax, ms);
                        g_latencyCount++;
                    }
                }
                g_inFlight--;
            };
            wgpuQueueOnSubmittedWorkDone(g.queue, ci);
        }

        bool GpuBusy()
        {
            if (g_inFlight < kMaxInFlight)
                return false;
            static std::chrono::steady_clock::time_point lastWarn{};
            auto now = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point oldest = now;
            {
                std::lock_guard lock(g_flightMutex);
                if (!g_submitTimes.empty())
                    oldest = g_submitTimes.front();
            }
            if (now - oldest > std::chrono::seconds(3) && now - lastWarn > std::chrono::seconds(10))
            {
                lastWarn = now;
                Log("render: the GPU hasn't finished a frame submitted %.1f s ago (%d in flight)",
                    std::chrono::duration<double>(now - oldest).count(), g_inFlight.load());
            }
            return true;
        }

        // Resolve write-back (ResolveCmd::wbAddr): read the destination back
        // and store it in guest memory, tiled and swapped like the hardware's
        // resolve output, a frame or so later.
        struct Writeback
        {
            WGPUBuffer buffer = nullptr;
            uint32_t w = 0, h = 0, rowBytes = 0, addr = 0, pitch = 0;
            WGPUTextureFormat format{};
            uint8_t endian = 0, swap = 0, depth = 0, half2 = 0;
        };

        float HalfBitsToFloat(uint16_t h)
        {
            uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
            float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * float(1 << e) / 32768.0f;
            return s ? -v : v;
        }

        // Read-back buffers for write-back, reused by size (a dozen small
        // resolves a frame used to allocate and free a buffer each).
        std::unordered_map<uint64_t, std::vector<WGPUBuffer>> g_writebackPool;
        WGPUBuffer WritebackBuffer(uint64_t size)
        {
            auto& free = g_writebackPool[size];
            if (!free.empty())
            {
                WGPUBuffer b = free.back();
                free.pop_back();
                return b;
            }
            WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
            bd.size = size;
            bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
            return wgpuDeviceCreateBuffer(g.device, &bd);
        }

        void StartWriteback(const Writeback& wb)
        {
            auto* job = new Writeback(wb);
            WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            ci.mode = WGPUCallbackMode_AllowSpontaneous;
            ci.userdata1 = job;
            ci.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud, void*) {
                auto* wb = (Writeback*)ud;
                uint64_t bytes = uint64_t(wb->rowBytes) * wb->h;
                const uint8_t* p = status == WGPUMapAsyncStatus_Success ? (const uint8_t*)wgpuBufferGetConstMappedRange(wb->buffer, 0, bytes) : nullptr;
                if (p)
                {
                    std::vector<uint32_t> texels(size_t(wb->w) * wb->h);
                    for (uint32_t y = 0; y < wb->h; y++)
                        for (uint32_t x = 0; x < wb->w; x++)
                        {
                            const uint8_t* s = p + size_t(y) * wb->rowBytes;
                            uint32_t v;
                            if (wb->depth)
                            {
                                float d;
                                if (wb->format == WGPUTextureFormat_R32Float)
                                    memcpy(&d, s + x * 4, 4);
                                else
                                {
                                    uint16_t hbits;
                                    memcpy(&hbits, s + x * 8, 2);
                                    d = HalfBitsToFloat(hbits);
                                }
                                v = uint32_t(std::clamp(d, 0.0f, 1.0f) * 16777215.0f + 0.5f) << 8; // D24S8: depth above stencil
                            }
                            else if (wb->half2)
                            {
                                uint16_t rg[2];
                                memcpy(rg, s + x * 8, 4); // RGBA16F texel: red, green halves
                                v = rg[0] | uint32_t(rg[1]) << 16;
                            }
                            else
                            {
                                const uint8_t* c = s + x * 4; // RGBA8
                                v = wb->swap ? (c[2] | c[1] << 8 | c[0] << 16 | uint32_t(c[3]) << 24)
                                             : (c[0] | c[1] << 8 | c[2] << 16 | uint32_t(c[3]) << 24);
                            }
                            texels[size_t(y) * wb->w + x] = v;
                        }
                    WriteTiled32(wb->addr, wb->pitch, wb->w, wb->h, texels.data(), wb->endian);
                }
                wgpuBufferUnmap(wb->buffer);
                auto& free = g_writebackPool[uint64_t(wb->rowBytes) * wb->h];
                if (status == WGPUMapAsyncStatus_Success && free.size() < 8)
                    free.push_back(wb->buffer);
                else
                    wgpuBufferRelease(wb->buffer);
                delete wb;
            };
            wgpuBufferMapAsync(wb.buffer, WGPUMapMode_Read, 0, uint64_t(wb.rowBytes) * wb.h, ci);
        }

        // Backend cost per frame (ms), summed between the periodic log lines.
        struct ExecTiming { double upload = 0, encode = 0, submit = 0; uint64_t frames = 0, draws = 0, groupsCreated = 0, groupsReused = 0; } g_execTiming;

        void Execute(Frame& f)
        {
            using clk = std::chrono::steady_clock;
            auto ms = [](clk::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
            clk::time_point tStart = clk::now();
            for (const auto& s : f.shaders)
                AddShader(s);
            for (auto& up : f.uploads)
            {
                Backend::Tex& t = g.textures[up.id];
                uint32_t layers = std::max<uint32_t>(1, up.layers);
                uint32_t depth = up.is3d ? std::max<uint32_t>(1, up.depth) : 1;
                WGPUTextureFormat uf = up.half ? WGPUTextureFormat_RGBA16Float : WGPUTextureFormat_RGBA8Unorm;
                uint32_t mips = 1 + uint32_t(up.mips.size());
                if (!t.texture || t.w != up.width || t.h != up.height || t.layers != layers || t.mips != mips || t.format != uf ||
                    t.is3d != up.is3d || t.depth != depth)
                {
                    ReleaseTex(t);
                    t.format = uf;
                    t.texture = MakeTexture(up.width, up.height, up.is3d ? depth : layers, t.format,
                                            WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                            up.is3d ? WGPUTextureDimension_3D : WGPUTextureDimension_2D, mips);
                    t.mips = mips;
                    WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                    vd.dimension = up.is3d ? WGPUTextureViewDimension_3D : layers == 6 ? WGPUTextureViewDimension_Cube : WGPUTextureViewDimension_2D;
                    t.view = wgpuTextureCreateView(t.texture, &vd);
                    t.w = up.width;
                    t.h = up.height;
                    t.layers = layers;
                    t.depth = depth;
                    t.is3d = up.is3d;
                }
                WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                dst.texture = t.texture;
                WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
                layout.bytesPerRow = up.width * (up.half ? 8 : 4);
                layout.rowsPerImage = up.height;
                WGPUExtent3D size = { up.width, up.height, up.is3d ? depth : layers };
                wgpuQueueWriteTexture(g.queue, &dst, up.rgba.data(), up.rgba.size(), &layout, &size);
                for (uint32_t l = 1; l < mips; l++)
                {
                    uint32_t mw = std::max(1u, up.width >> l), mh = std::max(1u, up.height >> l);
                    dst.mipLevel = l;
                    layout.bytesPerRow = mw * 4;
                    layout.rowsPerImage = mh;
                    WGPUExtent3D msize = { mw, mh, up.is3d ? std::max(1u, depth >> l) : layers };
                    const std::vector<uint8_t>& m = up.mips[l - 1];
                    wgpuQueueWriteTexture(g.queue, &dst, m.data(), m.size(), &layout, &msize);
                }
            }

            EnsureBuffer(g.uniforms, g.uniformsSize, f.uniforms.size() + kConstBytes, WGPUBufferUsage_Uniform);
            EnsureBuffer(g.vertices, g.verticesSize, f.vertexData.size() * 4 + 16, WGPUBufferUsage_Storage);
            EnsureBuffer(g.indices, g.indicesSize, f.indices.size() * 4 + 16, WGPUBufferUsage_Index);
            EnsureBuffer(g.resolveParams, g.resolveParamsSize, f.resolves.size() * 256 + 256, WGPUBufferUsage_Uniform);
            Write(g.uniforms, f.uniforms.data(), f.uniforms.size());
            Write(g.vertices, f.vertexData.data(), f.vertexData.size() * 4);
            Write(g.indices, f.indices.data(), f.indices.size() * 4);
            if (!g.bg0)
            {
                WGPUBindGroupEntry e[3] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
                e[0].binding = 0; e[0].buffer = g.uniforms; e[0].size = kConstBytes;
                e[1].binding = 1; e[1].buffer = g.uniforms; e[1].size = kMiscBytes;
                e[2].binding = 2; e[2].buffer = g.vertices; e[2].size = std::min<uint64_t>(g.verticesSize, 128ull << 20);
                WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                bd.layout = g.bgl0;
                bd.entryCount = 3;
                bd.entries = e;
                g.bg0 = wgpuDeviceCreateBindGroup(g.device, &bd);
            }
            if (!f.resolves.empty())
            {
                std::vector<int32_t> params(f.resolves.size() * 64, 0);
                for (size_t i = 0; i < f.resolves.size(); i++)
                {
                    params[i * 64] = f.resolves[i].x;
                    params[i * 64 + 1] = f.resolves[i].y;
                    params[i * 64 + 2] = std::max<int32_t>(1, f.resolves[i].scaleX);
                    params[i * 64 + 3] = std::max<int32_t>(1, f.resolves[i].scaleY);
                    params[i * 64 + 4] = f.resolves[i].convert;
                    params[i * 64 + 5] = f.resolves[i].clamp7e3;
                    // The source's host texels per sample and the destination's per
                    // texel, in 1/65536 (targets rendered at a reduced size).
                    params[i * 64 + 6] = int32_t(std::lround(TargetScale(f.resolves[i].src.pitch) * 65536.0f));
                    params[i * 64 + 7] = int32_t(std::lround(f.resolves[i].destScale * 65536.0f));
                    memcpy(&params[i * 64 + 8], f.resolves[i].mul, 16);
                    memcpy(&params[i * 64 + 12], f.resolves[i].maxv, 16);
                }
                Write(g.resolveParams, params.data(), params.size() * 4);
            }

            clk::time_point tEncode = clk::now();
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
            PassState ps;
            std::vector<WGPUBindGroup> transient;
            std::vector<Writeback> writebacks;
            g_gpuProf.BeginFrame();

            const std::vector<uint8_t> depthless = DepthlessRuns(f);
            for (const Cmd& c : f.cmds)
            {
                if (c.type == Cmd::Draw)
                {
                    const DrawCmd& d = f.draws[c.index];
                    bool hasColor = false;
                    for (int i = 0; i < 4; i++)
                        hasColor |= d.color[i].Valid();
                    bool noDepth = hasColor && depthless[size_t(&c - f.cmds.data())] != 0; // (a pass needs some attachment)
                    PipelineKey key = d.pipe;
                    key.noDepthAttachment = noDepth;
                    const Backend::Pipe* pipe = Pipeline(key);
                    if (!pipe)
                        continue;
                    bool same = ps.pass && ps.depth == d.depth && ps.noDepth == noDepth && ps.height >= d.rtHeight && !g_gpuProf.active;
                    for (int i = 0; i < 4 && same; i++)
                        same = ps.color[i] == d.color[i];
                    if (!same)
                    {
                        EndPass(ps);
                        uint32_t height = d.rtHeight;
                        for (int i = 0; i < 4; i++)
                            if (d.color[i].Valid()) Target(d.color[i], height);
                        Backend::Tex& depth = Target(d.depth, height);
                        WGPURenderPassColorAttachment ca[4];
                        uint32_t count = 0;
                        for (int i = 0; i < 4; i++)
                        {
                            ca[i] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
                            if (d.color[i].Valid())
                            {
                                ca[i].view = Target(d.color[i], height).view;
                                ca[i].loadOp = WGPULoadOp_Load;
                                ca[i].storeOp = WGPUStoreOp_Store;
                                count = i + 1;
                            }
                        }
                        WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
                        da.depthClearValue = 1.0f; // the INIT value (NaN) is rejected by browsers even when not clearing
                        da.view = depth.view;
                        da.depthLoadOp = WGPULoadOp_Load;
                        da.depthStoreOp = WGPUStoreOp_Store;
                        da.stencilLoadOp = WGPULoadOp_Load;
                        da.stencilStoreOp = WGPUStoreOp_Store;
                        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
                        rp.colorAttachmentCount = count;
                        rp.colorAttachments = ca;
                        if (g_gpuProf.active)
                        {
                            char label[96];
                            snprintf(label, sizeof(label), "draw ps %08X vs %08X target %ux%u%s", uint32_t(d.pipe.ps), uint32_t(d.pipe.vs),
                                     depth.nw, std::min(d.rtHeight, depth.nh), d.pipe.rectList ? " rect" : "");
                            rp.timestampWrites = g_gpuProf.Stamp(label);
                        }
                        rp.depthStencilAttachment = noDepth ? nullptr : &da;
                        ps.pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
                        for (int i = 0; i < 4; i++) ps.color[i] = d.color[i];
                        ps.depth = d.depth;
                        ps.noDepth = noDepth;
                        ps.height = depth.nh;
                        ps.scale = TargetScale(d.depth.pitch);
                        ps.hostW = depth.w;
                        ps.hostH = depth.h;
                        ps.indexBound = false;
                        ps.stateValid = false; // a new pass starts with default state
                        ps.usesDepth = false;
                    }
                    if (d.pipe.depthTest || d.pipe.depthWrite || d.pipe.stencilEnable)
                        ps.usesDepth = true;

                    // Textures (group 1), from the cache when this exact set was bound before.
                    WGPUBindGroup bg1 = g.emptyBg;
                    if (pipe->mask)
                    {
                        static std::vector<WGPUBindGroupEntry> e;
                        static std::vector<uintptr_t> key;
                        e.clear();
                        key.clear();
                        key.push_back(uintptr_t(pipe->bgl1));
                        for (uint32_t s = 0; s < 32; s++)
                        {
                            if (!((pipe->mask >> s) & 1)) continue;
                            const TextureBinding* tb = nullptr;
                            for (uint32_t i = 0; i < d.textureCount; i++)
                                if (f.textures[d.textureFirst + i].slot == s) tb = &f.textures[d.textureFirst + i];
                            uint8_t dim = pipe->dims[s];
                            WGPUTextureView view = dim == 2 ? g.placeholder3d.view : dim == 3 ? g.placeholderCube.view : g.placeholder2d.view;
                            if (tb && tb->textureId)
                            {
                                auto t = g.textures.find(tb->textureId);
                                if (t != g.textures.end() && t->second.view && (t->second.layers == 6) == (dim == 3) &&
                                    t->second.is3d == (dim == 2))
                                    view = t->second.view;
                            }
                            WGPUBindGroupEntry te = WGPU_BIND_GROUP_ENTRY_INIT;
                            te.binding = s * 2;
                            te.textureView = view;
                            e.push_back(te);
                            WGPUBindGroupEntry se = WGPU_BIND_GROUP_ENTRY_INIT;
                            se.binding = s * 2 + 1;
                            se.sampler = Sampler(tb ? tb->sampler : 0);
                            e.push_back(se);
                            key.push_back(uintptr_t(view));
                            key.push_back(uintptr_t(se.sampler));
                        }
                        auto cached = g.textureGroups.find(key);
                        if (cached != g.textureGroups.end())
                        {
                            bg1 = cached->second;
                            g.groupsReused++;
                        }
                        else
                        {
                            WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                            bd.layout = pipe->bgl1;
                            bd.entryCount = e.size();
                            bd.entries = e.data();
                            bg1 = wgpuDeviceCreateBindGroup(g.device, &bd);
                            g.groupsCreated++;
                            if (g.textureGroups.size() >= 8192)
                                ClearBindGroupCaches();
                            g.textureGroups.emplace(key, bg1);
                        }
                    }

                    // Per-draw state, sent only when it changes within the pass.
                    uint32_t sx = std::min(d.scissor[0], d.depth.pitch);
                    uint32_t sy = std::min(d.scissor[1], ps.height);
                    uint32_t sw = std::min(d.scissor[2], d.depth.pitch - sx), sh = std::min(d.scissor[3], ps.height - sy);
                    if (!sw || !sh)
                        continue;
                    // Viewport per draw: the draw's transform spans d.rtHeight rows.
                    float vw = float(d.depth.pitch), vh = float(std::min(d.rtHeight, ps.height));
                    if (ps.scale != 1.0f)
                    {
                        // Scaled target: the same area in host texels. The scissor
                        // grows to whole texels (it never vanishes).
                        vw = std::min(vw * ps.scale, float(ps.hostW));
                        vh = std::min(vh * ps.scale, float(ps.hostH));
                        uint32_t x0 = uint32_t(std::floor(sx * ps.scale)), y0 = uint32_t(std::floor(sy * ps.scale));
                        uint32_t x1 = std::min(ps.hostW, uint32_t(std::ceil((sx + sw) * ps.scale)));
                        uint32_t y1 = std::min(ps.hostH, uint32_t(std::ceil((sy + sh) * ps.scale)));
                        if (x1 <= x0 || y1 <= y0)
                            continue;
                        sx = x0, sy = y0, sw = x1 - x0, sh = y1 - y0;
                    }
                    if (!ps.stateValid || ps.vw != vw || ps.vh != vh)
                    {
                        wgpuRenderPassEncoderSetViewport(ps.pass, 0, 0, vw, vh, 0.0f, 1.0f);
                        ps.vw = vw;
                        ps.vh = vh;
                    }
                    if (!ps.stateValid || ps.pipeline != pipe->pipeline)
                    {
                        wgpuRenderPassEncoderSetPipeline(ps.pass, pipe->pipeline);
                        ps.pipeline = pipe->pipeline;
                    }
                    if (!ps.stateValid || ps.constOffset != d.constOffset || ps.miscOffset != d.miscOffset)
                    {
                        uint32_t offsets[2] = { d.constOffset, d.miscOffset };
                        wgpuRenderPassEncoderSetBindGroup(ps.pass, 0, g.bg0, 2, offsets);
                        ps.constOffset = d.constOffset;
                        ps.miscOffset = d.miscOffset;
                    }
                    if (!ps.stateValid || ps.bg1 != bg1)
                    {
                        wgpuRenderPassEncoderSetBindGroup(ps.pass, 1, bg1, 0, nullptr);
                        ps.bg1 = bg1;
                    }
                    if (!ps.stateValid || ps.scissor[0] != sx || ps.scissor[1] != sy || ps.scissor[2] != sw || ps.scissor[3] != sh)
                    {
                        wgpuRenderPassEncoderSetScissorRect(ps.pass, sx, sy, sw, sh);
                        ps.scissor[0] = sx; ps.scissor[1] = sy; ps.scissor[2] = sw; ps.scissor[3] = sh;
                    }
                    if (!ps.stateValid || memcmp(ps.blend, d.blendConstant, sizeof(ps.blend)) != 0)
                    {
                        WGPUColor bc = { d.blendConstant[0], d.blendConstant[1], d.blendConstant[2], d.blendConstant[3] };
                        wgpuRenderPassEncoderSetBlendConstant(ps.pass, &bc);
                        memcpy(ps.blend, d.blendConstant, sizeof(ps.blend));
                    }
                    if (!ps.stateValid || ps.stencilRef != d.stencilRef)
                    {
                        wgpuRenderPassEncoderSetStencilReference(ps.pass, d.stencilRef);
                        ps.stencilRef = d.stencilRef;
                    }
                    ps.stateValid = true;
                    if (d.indexed)
                    {
                        if (!ps.indexBound)
                        {
                            wgpuRenderPassEncoderSetIndexBuffer(ps.pass, g.indices, WGPUIndexFormat_Uint32, 0, g.indicesSize);
                            ps.indexBound = true;
                        }
                        wgpuRenderPassEncoderDrawIndexed(ps.pass, d.count, 1, d.first, 0, 0);
                    }
                    else
                        wgpuRenderPassEncoderDraw(ps.pass, d.count, 1, d.first, 0);
                    g.drawsExecuted++;
                }
                else
                {
                    EndPass(ps);
                    const ResolveCmd& r = f.resolves[c.index];
                    if (r.copy && r.src.Valid())
                    {
                        Backend::Tex& src = Target(r.src, r.rtHeight);
                        Backend::Tex& dst = g.textures[r.destId];
                        bool depth = r.src.format == RT_DEPTH24S8;
                        int blit = depth && g.float32Filterable ? 2 : r.destFloat || depth || r.convert == 1 ? 1 : 0;
                        WGPUTextureFormat df = blit == 2 ? WGPUTextureFormat_R32Float : blit ? WGPUTextureFormat_RGBA16Float : WGPUTextureFormat_RGBA8Unorm;
                        // (A copy of a scaled target is stored scaled: destScale.)
                        uint32_t dw = HostSize(r.destW, r.destScale), dh = HostSize(r.destH, r.destScale);
                        if (!dst.texture || dst.w != dw || dst.h != dh || dst.nw != r.destW || dst.nh != r.destH || dst.format != df)
                        {
                            ReleaseTex(dst);
                            dst.format = df;
                            dst.texture = MakeTexture(dw, dh, 1, df, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc);
                            dst.view = wgpuTextureCreateView(dst.texture, nullptr);
                            dst.w = dw;
                            dst.h = dh;
                            dst.nw = r.destW;
                            dst.nh = r.destH;
                        }
                        // (ReleaseTex above may have cleared the cache; look up after it.)
                        WGPUTextureView srcView = src.depthView ? src.depthView : src.view;
                        WGPUBindGroup& bg = g.resolveGroups[srcView];
                        if (!bg)
                        {
                            WGPUBindGroupEntry e[2] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
                            e[0].binding = 0;
                            e[0].textureView = srcView;
                            e[1].binding = 1;
                            e[1].buffer = g.resolveParams;
                            e[1].size = 64;
                            WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                            bd.layout = g.blitBgl;
                            bd.entryCount = 2;
                            bd.entries = e;
                            bg = wgpuDeviceCreateBindGroup(g.device, &bd);
                            g.groupsCreated++;
                        }
                        else
                            g.groupsReused++;
                        uint32_t w = std::min<uint32_t>(uint32_t(r.w), r.destW), h = std::min<uint32_t>(uint32_t(r.h), r.destH);
                        // A copy over the whole texture starts from a clear, not a load:
                        // free on phone GPUs, where loading means reading it from memory.
                        bool whole = w == r.destW && h == r.destH;
                        // The area written, in the destination's texels.
                        uint32_t hw = std::min(dst.w, HostSize(w, r.destScale)), hh = std::min(dst.h, HostSize(h, r.destScale));
                        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
                        ca.view = dst.view;
                        ca.loadOp = whole ? WGPULoadOp_Clear : WGPULoadOp_Load;
                        ca.clearValue = { 0, 0, 0, 0 };
                        ca.storeOp = WGPUStoreOp_Store;
                        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
                        rp.colorAttachmentCount = 1;
                        rp.colorAttachments = &ca;
                        g_passCounts.resolve++;
                        if (whole) g_passCounts.resolveFull++;
                        if (g_gpuProf.active)
                        {
                            char label[96];
                            snprintf(label, sizeof(label), "resolve %ux%u (blit %d)", w, h, blit);
                            rp.timestampWrites = g_gpuProf.Stamp(label);
                        }
                        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
                        if (w && h)
                        {
                            wgpuRenderPassEncoderSetPipeline(pass, g.blitPipeline[blit]);
                            uint32_t off = c.index * 256;
                            wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 1, &off);
                            wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, hw, hh);
                            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
                        }
                        wgpuRenderPassEncoderEnd(pass);
                        wgpuRenderPassEncoderRelease(pass);
                        if (f.dumpDir)
                        {
                            ResolveDump rd{};
                            rd.w = dst.w;
                            rd.h = dst.h;
                            rd.isFloat = blit == 1;
                            rd.isR32F = blit == 2;
                            rd.rowBytes = (rd.w * (rd.isFloat ? 8 : 4) + 255) & ~255u;
                            WGPUBufferDescriptor bd2 = WGPU_BUFFER_DESCRIPTOR_INIT;
                            bd2.size = uint64_t(rd.rowBytes) * rd.h;
                            bd2.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
                            rd.buffer = wgpuDeviceCreateBuffer(g.device, &bd2);
                            WGPUTexelCopyTextureInfo ts = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                            ts.texture = dst.texture;
                            WGPUTexelCopyBufferInfo tb = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
                            tb.buffer = rd.buffer;
                            tb.layout.bytesPerRow = rd.rowBytes;
                            tb.layout.rowsPerImage = rd.h;
                            WGPUExtent3D ext = { rd.w, rd.h, 1 };
                            wgpuCommandEncoderCopyTextureToBuffer(enc, &ts, &tb, &ext);
                            snprintf(rd.name, sizeof(rd.name), "%s/f%llu_r%02u_%08X_src%u-%u-%u_tex%u.png", f.dumpDir,
                                     (unsigned long long)f.number, c.index, r.destAddr & 0x1FFFFFFF, r.src.base, r.src.pitch, r.src.format, r.destId);
                            g_resolveDumps.push_back(rd);
                        }
                        if (r.wbAddr && w && h)
                        {
                            Writeback wb{};
                            wb.w = w;
                            wb.h = h;
                            wb.format = df;
                            wb.rowBytes = (w * (df == WGPUTextureFormat_RGBA16Float ? 8 : 4) + 255) & ~255u;
                            wb.addr = r.wbAddr;
                            wb.pitch = r.wbPitch;
                            wb.endian = r.wbEndian;
                            wb.swap = r.wbSwap;
                            wb.depth = r.wbDepth;
                            wb.half2 = r.wbHalf2;
                            wb.buffer = WritebackBuffer(uint64_t(wb.rowBytes) * h);
                            WGPUTexelCopyTextureInfo ts = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                            ts.texture = dst.texture;
                            WGPUTexelCopyBufferInfo tb = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
                            tb.buffer = wb.buffer;
                            tb.layout.bytesPerRow = wb.rowBytes;
                            tb.layout.rowsPerImage = h;
                            WGPUExtent3D ext = { w, h, 1 };
                            wgpuCommandEncoderCopyTextureToBuffer(enc, &ts, &tb, &ext);
                            writebacks.push_back(wb);
                        }
                    }
                    if (r.clearColor.Valid() || r.clearDepth.Valid())
                    {
                        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
                        WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
                        da.depthClearValue = 1.0f; // the INIT value (NaN) is rejected by browsers even when not clearing
                        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
                        if (r.clearColor.Valid())
                        {
                            ca.view = Target(r.clearColor, r.rtHeight).view;
                            ca.loadOp = WGPULoadOp_Clear;
                            ca.storeOp = WGPUStoreOp_Store;
                            ca.clearValue = { r.clearValue[0], r.clearValue[1], r.clearValue[2], r.clearValue[3] };
                            rp.colorAttachmentCount = 1;
                            rp.colorAttachments = &ca;
                        }
                        if (r.clearDepth.Valid())
                        {
                            da.view = Target(r.clearDepth, r.rtHeight).view;
                            da.depthLoadOp = WGPULoadOp_Clear;
                            da.depthStoreOp = WGPUStoreOp_Store;
                            da.depthClearValue = r.depthValue;
                            da.stencilLoadOp = WGPULoadOp_Clear;
                            da.stencilStoreOp = WGPUStoreOp_Store;
                            da.stencilClearValue = r.stencilValue;
                            rp.depthStencilAttachment = &da;
                        }
                        // Attachments in one pass must match in size: clear separately if not.
                        bool sameSize = !(r.clearColor.Valid() && r.clearDepth.Valid()) ||
                                        Target(r.clearColor, r.rtHeight).w == Target(r.clearDepth, r.rtHeight).w;
                        if (g_gpuProf.active)
                            rp.timestampWrites = g_gpuProf.Stamp("clear");
                        g_passCounts.clear += sameSize ? 1 : 2;
                        if (sameSize)
                        {
                            WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
                            wgpuRenderPassEncoderEnd(pass);
                            wgpuRenderPassEncoderRelease(pass);
                        }
                        else
                        {
                            WGPURenderPassDescriptor c1 = rp, c2 = rp;
                            c1.depthStencilAttachment = nullptr;
                            c2.colorAttachmentCount = 0;
                            if (g_gpuProf.active)
                                c2.timestampWrites = g_gpuProf.Stamp("clear");
                            for (WGPURenderPassDescriptor* d : { &c1, &c2 })
                            {
                                WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, d);
                                wgpuRenderPassEncoderEnd(pass);
                                wgpuRenderPassEncoderRelease(pass);
                            }
                        }
                    }
                }
            }
            EndPass(ps);
            g_gpuProf.EndFrame(enc);
            clk::time_point tSubmit = clk::now();
            WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
            wgpuQueueSubmit(g.queue, 1, &cmd);
            TrackSubmitted();
            g_gpuProf.AfterSubmit();
            for (Writeback& wb : writebacks)
                StartWriteback(wb);
            // Textures the recorder evicted (none of them is used by this frame).
            for (uint32_t id : f.releases)
            {
                auto it = g.textures.find(id);
                if (it == g.textures.end())
                    continue;
                ReleaseTex(it->second);
                g.textures.erase(it);
            }
            wgpuCommandBufferRelease(cmd);
            wgpuCommandEncoderRelease(enc);
            for (WGPUBindGroup bg : transient)
                wgpuBindGroupRelease(bg);
            clk::time_point tEnd = clk::now();
            g_execTiming.upload += ms(tEncode - tStart);
            g_execTiming.encode += ms(tSubmit - tEncode);
            g_execTiming.submit += ms(tEnd - tSubmit);
            g_execTiming.frames++;
            g_execTiming.draws += f.draws.size();
            if (g_execTiming.frames == 120)
            {
                double n = double(g_execTiming.frames);
                double latAvg = 0, latMax = 0;
                {
                    std::lock_guard lock(g_flightMutex);
                    if (g_latencyCount)
                        latAvg = g_latencySum / g_latencyCount;
                    latMax = g_latencyMax;
                    g_latencySum = g_latencyMax = 0;
                    g_latencyCount = 0;
                }
                Log("render: backend %.1f ms/frame (uploads+writes %.1f, encode %.1f, submit %.1f), %.0f draws/frame; "
                    "bind groups: %.1f created, %.1f reused per frame; GPU latency %.0f ms avg, %.0f max",
                    (g_execTiming.upload + g_execTiming.encode + g_execTiming.submit) / n, g_execTiming.upload / n,
                    g_execTiming.encode / n, g_execTiming.submit / n, g_execTiming.draws / n,
                    (g.groupsCreated - g_execTiming.groupsCreated) / n, (g.groupsReused - g_execTiming.groupsReused) / n,
                    latAvg, latMax);
                Log("render: passes per frame: %.1f draw (%.1f without depth/stencil use), %.1f resolve (%.1f overwrite the whole texture), %.1f clear",
                    g_passCounts.draw / n, g_passCounts.drawNoDepth / n, g_passCounts.resolve / n, g_passCounts.resolveFull / n, g_passCounts.clear / n);
                g_passCounts = {};
                g_execTiming = {};
                g_execTiming.groupsCreated = g.groupsCreated;
                g_execTiming.groupsReused = g.groupsReused;
            }

            if (f.presentId)
            {
                g.presentId = f.presentId;
                g.presentW = f.presentW;
                g.presentH = f.presentH;
            }
            g.frames++;
            if (g.frames <= 3 || g.frames % 300 == 0)
            {
                auto texBytes = [](const Backend::Tex& t) {
                    uint64_t bpp = t.format == WGPUTextureFormat_RGBA16Float ? 8 : t.format == WGPUTextureFormat_RGBA32Float ? 16 : 4;
                    return uint64_t(t.w) * t.h * std::max(1u, t.layers) * std::max(1u, t.depth) * bpp * (t.mips > 1 ? 4 : 3) / 3;
                };
                uint64_t texMB = 0, rtMB = 0;
                for (auto& [id, t] : g.textures) texMB += texBytes(t);
                for (auto& [k, t] : g.targets) rtMB += texBytes(t);
                Log("render: executed frame %llu (%zu draws, %zu resolves); %llu pipelines (%u compiling, %llu draws waited), "
                    "%zu render targets (%llu MB), %zu textures (%llu MB), %llu draws total",
                    (unsigned long long)f.number, f.draws.size(), f.resolves.size(), (unsigned long long)g.pipelineCount,
                    g.pipelinesCompiling.load(), (unsigned long long)g.drawsWaiting,
                    g.targets.size(), (unsigned long long)(rtMB >> 20), g.textures.size(), (unsigned long long)(texMB >> 20),
                    (unsigned long long)g.drawsExecuted);
            }
        }
    }

    float TargetScale(uint32_t pitch)
    {
        // (In 1/16 steps: exact in float, so scaled sizes and viewports agree.)
        static const float scale = [] {
            const char* v = getenv("RES_SCALE");
            float s = v ? std::clamp(float(atof(v)), 0.25f, 1.0f) : 1.0f;
            s = std::round(s * 16.0f) / 16.0f;
            if (s != 1.0f)
                Log("render: internal resolution %.0f%% (render targets from %s samples wide)", s * 100.0f,
                    getenv("RES_SCALE_MIN_PITCH") ? getenv("RES_SCALE_MIN_PITCH") : "1024");
            return s;
        }();
        static const uint32_t minPitch = getenv("RES_SCALE_MIN_PITCH") ? uint32_t(atoi(getenv("RES_SCALE_MIN_PITCH"))) : 1024;
        return pitch >= minPitch ? scale : 1.0f;
    }

    void SubmitFrame(std::unique_ptr<Frame> frame)
    {
        if (!g_ready)
            return; // no backend (no window): drop
        std::unique_lock lock(g_queueMutex);
        // Wait briefly for the backend; if it isn't draining (a hidden browser tab
        // pauses animation frames), drop the oldest frame rather than stall the game.
        if (frame->dumpDir || (!g_queue.empty() && g_queue.front()->dumpDir))
            g_queueCv.wait(lock, [] { return g_queue.size() < kMaxQueued; });
        else if (!g_queueCv.wait_for(lock, std::chrono::milliseconds(100), [] { return g_queue.size() < kMaxQueued; }))
        {
            // The recorder sends shaders and texture data once: carry them over.
            std::unique_ptr<Frame> old = std::move(g_queue.front());
            g_queue.pop_front();
            Frame& next = g_queue.empty() ? *frame : *g_queue.front();
            next.shaders.insert(next.shaders.begin(), std::make_move_iterator(old->shaders.begin()), std::make_move_iterator(old->shaders.end()));
            next.uploads.insert(next.uploads.begin(), std::make_move_iterator(old->uploads.begin()), std::make_move_iterator(old->uploads.end()));
            next.releases.insert(next.releases.end(), old->releases.begin(), old->releases.end());
            static std::atomic<uint64_t> dropped{ 0 };
            if (dropped++ % 600 == 0)
                Log("render: backend not keeping up (hidden page?); dropped %llu frames", (unsigned long long)dropped.load());
        }
        g_queue.push_back(std::move(frame));
    }

    void BackendInit(WGPUDevice device, WGPUQueue queue)
    {
        g.device = device;
        g.queue = queue;
        g.float32Filterable = wgpuDeviceHasFeature(device, WGPUFeatureName_Float32Filterable);
        CreateFixedLayouts();
        MakePlaceholders();
        g_ready = true;
        Log("render: WebGPU backend ready");
    }

    // The front buffer's area in its texture's texels (a front buffer copied
    // from a target rendered at a reduced size is stored at that size).
    static void PresentSize(const Backend::Tex& t, uint32_t& w, uint32_t& h)
    {
        w = g.presentW ? g.presentW : t.w;
        h = g.presentH ? g.presentH : t.h;
        if (t.nw && t.nh && (t.nw != t.w || t.nh != t.h))
        {
            w = std::min(t.w, uint32_t(std::lround(double(w) * t.w / t.nw)));
            h = std::min(t.h, uint32_t(std::lround(double(h) * t.h / t.nh)));
        }
    }

    WGPUTexture BackendPresentTexture(uint32_t& w, uint32_t& h, bool& isFloat, uint64_t& frames)
    {
        frames = g.frames;
        auto it = g.textures.find(g.presentId);
        if (!g.presentId || it == g.textures.end())
            return nullptr;
        PresentSize(it->second, w, h);
        isFloat = it->second.format == WGPUTextureFormat_RGBA16Float;
        return it->second.texture;
    }

    std::vector<ResolveDump> BackendTakeResolveDumps()
    {
        return std::move(g_resolveDumps);
    }

    uint32_t BackendPendingFrames()
    {
        std::lock_guard lock(g_queueMutex);
        return uint32_t(g_queue.size());
    }

    WGPUTextureView BackendRunPending(uint32_t& showW, uint32_t& showH, uint32_t& texW, uint32_t& texH)
    {
        // A long gap between calls means the page's main thread was blocked
        // (or animation frames paused while the page was hidden).
        static std::chrono::steady_clock::time_point lastCall{};
        auto now = std::chrono::steady_clock::now();
        // (A hidden page gets one animation frame a second: report longer gaps.)
        if (lastCall != std::chrono::steady_clock::time_point{} && now - lastCall > std::chrono::milliseconds(1500))
            Log("render: the backend didn't run for %.1f s (main thread busy or page hidden)",
                std::chrono::duration<double>(now - lastCall).count());
        lastCall = now;
        for (;;)
        {
            if (GpuBusy())
                break; // frames stay queued; the recorder drops old ones if this lasts
            std::unique_ptr<Frame> f;
            {
                std::lock_guard lock(g_queueMutex);
                if (g_queue.empty())
                    break;
                f = std::move(g_queue.front());
                g_queue.pop_front();
            }
            g_queueCv.notify_all();
            Execute(*f);
        }
        if (!g.presentId)
            return nullptr;
        auto it = g.textures.find(g.presentId);
        if (it == g.textures.end() || !it->second.view)
            return nullptr;
        PresentSize(it->second, showW, showH);
        texW = it->second.w;
        texH = it->second.h;
        return it->second.view;
    }
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Browser console: Module._crysis_shader_wgsl(0x1234ABCD) logs that shader's
// translated WGSL (low 32 bits of its hash, as shown in frame logs).
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_shader_wgsl(uint32_t hash)
{
    auto it = render::g_shaderText.find(hash);
    if (it == render::g_shaderText.end())
    {
        Log("WGSL %08X: not loaded", hash);
        return;
    }
    const std::string& w = it->second;
    Log("WGSL ---- shader %08X (%zu chars) ----", hash, w.size());
    for (size_t at = 0; at < w.size();)
    {
        size_t nl = w.find('\n', at);
        size_t end = nl == std::string::npos ? w.size() : nl;
        Log("WGSL %s", w.substr(at, std::min<size_t>(end - at, 400)).c_str());
        at = end + 1;
    }
}
#endif
