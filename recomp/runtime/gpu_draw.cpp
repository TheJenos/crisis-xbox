// Draw recording: snapshots the GPU state of each draw into a render::Frame.
//
// Runs on the command processor thread, at the point in the command stream
// where the draw happens, so register and memory contents are exactly what the
// draw would see. EDRAM render targets are identified by (base, pitch, format);
// resolves copy them into backend textures that later draws (and the display)
// find by guest address.
#include "gpu_draw.h"
#include "guest.h"
#include "platform.h"
#include "png.h"
#include "render.h"
#include "xenos_texture.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif

namespace
{
    constexpr uint32_t REG_BASE = 0x7FC80000;
    uint32_t Reg(uint32_t index) { return Load<uint32_t>(REG_BASE + index * 4); }
    float RegF(uint32_t index) { uint32_t v = Reg(index); float f; memcpy(&f, &v, 4); return f; }
    uint32_t PhysToGuest(uint32_t p) { return 0xA0000000 | (p & 0x1FFFFFFF); }

    enum : uint32_t
    {
        RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002, RB_COLOR1_INFO = 0x2003,
        PA_SC_SCREEN_SCISSOR_TL = 0x200E, PA_SC_SCREEN_SCISSOR_BR = 0x200F,
        PA_SC_WINDOW_OFFSET = 0x2080, PA_SC_WINDOW_SCISSOR_TL = 0x2081, PA_SC_WINDOW_SCISSOR_BR = 0x2082,
        VGT_INDX_OFFSET = 0x2102, VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103,
        RB_COLOR_MASK = 0x2104, RB_BLEND_RED = 0x2105, RB_STENCILREFMASK_BF = 0x210C, RB_STENCILREFMASK = 0x210D,
        RB_ALPHA_REF = 0x210E, PA_CL_VPORT_XSCALE = 0x210F,
        RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202, PA_SU_SC_MODE_CNTL = 0x2205,
        PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208, RB_BLENDCONTROL1 = 0x2209,
        RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A, RB_COPY_DEST_INFO = 0x231B,
        RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E,
        ALU_CONSTANTS = 0x4000, FETCH_CONSTANTS = 0x4800, BOOL_CONSTANTS = 0x4900, LOOP_CONSTANTS = 0x4908,
    };

    uint32_t Swap(uint32_t v, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
        case 2: return __builtin_bswap32(v);
        case 3: return (v << 16) | (v >> 16);
        default: return v;
        }
    }
    // A dword as the GPU reads it: little-endian load, then the endian swap.
    uint32_t GpuDword(uint32_t guest, uint32_t endian)
    {
        uint32_t v;
        memcpy(&v, GuestPtr(guest), 4);
        return Swap(v, endian);
    }

    // Content hash (change detection of textures, keys). Four independent lanes:
    // one serial multiply chain made hashing the frame's textures (~200 MB/s
    // in the mission) latency-bound.
    uint64_t Hash64(const void* data, size_t n, uint64_t h = 1469598103934665603ull)
    {
        constexpr uint64_t P = 1099511628211ull;
        const uint8_t* p = (const uint8_t*)data;
        size_t i = 0;
        uint64_t l0 = h, l1 = h ^ 0x9E3779B97F4A7C15ull, l2 = h ^ 0xC2B2AE3D27D4EB4Full, l3 = h ^ 0x165667B19E3779F9ull;
        for (; i + 32 <= n; i += 32)
        {
            uint64_t w[4];
            memcpy(w, p + i, 32);
            l0 = (l0 ^ w[0]) * P; l0 ^= l0 >> 29;
            l1 = (l1 ^ w[1]) * P; l1 ^= l1 >> 29;
            l2 = (l2 ^ w[2]) * P; l2 ^= l2 >> 29;
            l3 = (l3 ^ w[3]) * P; l3 ^= l3 >> 29;
        }
        h = l0 ^ (l1 * 3) ^ (l2 * 5) ^ (l3 * 7);
        for (; i + 8 <= n; i += 8)
        {
            uint64_t w;
            memcpy(&w, p + i, 8);
            h = (h ^ w) * P;
            h ^= h >> 29;
        }
        for (; i < n; i++) h = (h ^ p[i]) * P;
        return h;
    }

    // Bulk endian conversion and comparison of guest data (vertex buffers,
    // indices, constants), 16 bytes at a time with WebAssembly SIMD. The
    // per-dword loops took most of the command processor's time.
#if defined(__wasm_simd128__)
    template <int E> inline v128_t SwapV(v128_t v)
    {
        if constexpr (E == 1) return wasm_i8x16_shuffle(v, v, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        else if constexpr (E == 2) return wasm_i8x16_shuffle(v, v, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        else if constexpr (E == 3) return wasm_i8x16_shuffle(v, v, 2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13);
        else return v;
    }
    template <int E> void CopySwapT(uint32_t* dst, const uint32_t* src, size_t n)
    {
        size_t i = 0;
        for (; i + 4 <= n; i += 4) wasm_v128_store(dst + i, SwapV<E>(wasm_v128_load(src + i)));
        for (; i < n; i++) dst[i] = Swap(src[i], E);
    }
    template <int E> bool EqualSwapT(const uint32_t* copy, const uint32_t* src, size_t n)
    {
        size_t i = 0;
        for (; i + 16 <= n; i += 16)
        {
            v128_t d = wasm_v128_xor(wasm_v128_load(copy + i), SwapV<E>(wasm_v128_load(src + i)));
            d = wasm_v128_or(d, wasm_v128_xor(wasm_v128_load(copy + i + 4), SwapV<E>(wasm_v128_load(src + i + 4))));
            d = wasm_v128_or(d, wasm_v128_xor(wasm_v128_load(copy + i + 8), SwapV<E>(wasm_v128_load(src + i + 8))));
            d = wasm_v128_or(d, wasm_v128_xor(wasm_v128_load(copy + i + 12), SwapV<E>(wasm_v128_load(src + i + 12))));
            if (wasm_v128_any_true(d)) return false;
        }
        for (; i < n; i++)
            if (copy[i] != Swap(src[i], E)) return false;
        return true;
    }
    // dst[i] = Swap(src[i], endian) for n dwords.
    void CopySwap32(uint32_t* dst, const uint32_t* src, size_t n, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 0: CopySwapT<0>(dst, src, n); break;
        case 1: CopySwapT<1>(dst, src, n); break;
        case 2: CopySwapT<2>(dst, src, n); break;
        default: CopySwapT<3>(dst, src, n); break;
        }
    }
    // copy[i] == Swap(src[i], endian) for all n dwords.
    bool EqualSwap32(const uint32_t* copy, const uint32_t* src, size_t n, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 0: return EqualSwapT<0>(copy, src, n);
        case 1: return EqualSwapT<1>(copy, src, n);
        case 2: return EqualSwapT<2>(copy, src, n);
        default: return EqualSwapT<3>(copy, src, n);
        }
    }
#else
    void CopySwap32(uint32_t* dst, const uint32_t* src, size_t n, uint32_t endian)
    {
        for (size_t i = 0; i < n; i++) dst[i] = Swap(src[i], endian);
    }
    bool EqualSwap32(const uint32_t* copy, const uint32_t* src, size_t n, uint32_t endian)
    {
        for (size_t i = 0; i < n; i++)
            if (copy[i] != Swap(src[i], endian)) return false;
        return true;
    }
#endif

    // An index buffer from memory, as the draw uses it: endian swap, then the
    // primitive reset index becomes 0xFFFFFFFF and the rest get the index
    // offset added. Returns the largest index other than 0xFFFFFFFF (0 if none).
    uint32_t ReadIndices(uint32_t* out, const uint8_t* src, uint32_t count, bool index32, uint32_t endian,
                         bool restart, uint32_t resetIndex, uint32_t offset)
    {
        uint32_t i = 0, maxIndex = 0;
        uint32_t reset = index32 ? resetIndex : (resetIndex & 0xFFFF);
#if defined(__wasm_simd128__)
        const v128_t vreset = wasm_i32x4_splat(int32_t(reset)), voff = wasm_i32x4_splat(int32_t(offset));
        const v128_t ones = wasm_i32x4_splat(-1);
        v128_t vmax = wasm_i32x4_splat(0);
        auto finish = [&](v128_t v) {
            v128_t isReset = restart ? wasm_i32x4_eq(v, vreset) : wasm_i32x4_splat(0);
            v128_t r = wasm_v128_or(wasm_i32x4_add(v, voff), isReset);
            vmax = wasm_u32x4_max(vmax, wasm_v128_andnot(r, wasm_i32x4_eq(r, ones)));
            return r;
        };
        if (!index32)
            for (; i + 8 <= count; i += 8)
            {
                v128_t h = wasm_v128_load(src + i * 2);
                if (endian)
                    h = wasm_i8x16_shuffle(h, h, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
                wasm_v128_store(out + i, finish(wasm_u32x4_extend_low_u16x8(h)));
                wasm_v128_store(out + i + 4, finish(wasm_u32x4_extend_high_u16x8(h)));
            }
        else
        {
            for (; i + 4 <= count; i += 4)
            {
                v128_t v = wasm_v128_load(src + i * 4);
                switch (endian & 3)
                {
                case 1: v = SwapV<1>(v); break;
                case 2: v = SwapV<2>(v); break;
                case 3: v = SwapV<3>(v); break;
                default: break;
                }
                wasm_v128_store(out + i, finish(v));
            }
        }
        maxIndex = std::max(std::max(wasm_u32x4_extract_lane(vmax, 0), wasm_u32x4_extract_lane(vmax, 1)),
                            std::max(wasm_u32x4_extract_lane(vmax, 2), wasm_u32x4_extract_lane(vmax, 3)));
#endif
        for (; i < count; i++)
        {
            uint32_t v;
            if (index32)
            {
                memcpy(&v, src + i * 4, 4);
                v = Swap(v, endian);
            }
            else
            {
                uint16_t h;
                memcpy(&h, src + i * 2, 2);
                v = endian ? uint16_t((h >> 8) | (h << 8)) : h;
            }
            v = restart && v == reset ? 0xFFFFFFFF : v + offset;
            out[i] = v;
            if (v != 0xFFFFFFFF) maxIndex = std::max(maxIndex, v);
        }
        return maxIndex;
    }

    // PROFILE=1: time from construction (or the last Next) into the current
    // phase's counter; free when profiling is off.
    struct PhaseTimer
    {
        int phase;
        uint64_t t0;
        explicit PhaseTimer(int p) : phase(p), t0(prof::Enabled() ? PlatformCounterNs() : 0) {}
        void Next(int p)
        {
            if (t0)
            {
                uint64_t now = PlatformCounterNs();
                prof::cpPhaseNs[phase] += now - t0;
                t0 = now;
            }
            phase = p;
        }
        ~PhaseTimer() { if (t0) prof::cpPhaseNs[phase] += PlatformCounterNs() - t0; }
    };
    void Count(int counter, uint64_t n) { if (prof::Enabled()) prof::cpCount[counter] += n; }

    struct Stats
    {
        uint64_t draws = 0, recorded = 0, resolves = 0, noShader = 0, badPrim = 0, rectLists = 0, noTarget = 0, frames = 0;
        uint64_t presented = 0;
    } g_stats;

    std::unique_ptr<render::Frame> g_frame;
    std::unordered_set<uint64_t> g_sentShaders;
    uint32_t g_nextTextureId = 1;

    struct CachedTexture { uint32_t id; uint64_t hash; uint64_t checkedFrame; uint64_t lastUsed; size_t bytes; };
    std::unordered_map<uint64_t, CachedTexture> g_textures; // by texture description
    size_t g_textureBytes = 0; // decoded texels held by the backend for g_textures

    // Last format each EDRAM surface (base, pitch) was drawn with: resolves
    // sometimes reinterpret a surface's bits in another format.
    struct Drawn { uint8_t host, xenos; };
    std::unordered_map<uint64_t, Drawn> g_lastDrawn;
    uint64_t SurfaceKey(uint32_t base, uint32_t pitch) { return uint64_t(base) | (uint64_t(pitch) << 16); }
    uint32_t XenosColorFormat(uint32_t index) { return (Reg(index == 0 ? RB_COLOR_INFO : RB_COLOR1_INFO + index - 1) >> 16) & 0xF; }

    struct Resolved { uint32_t id, w, h; bool isFloat; bool swapRB; float chanMax[4]; bool rawInt; bool raw7e3 = false; };
    std::unordered_map<uint32_t, Resolved> g_resolved; // by guest address

    // Shader constants (ALU constant registers 0x4000-0x47FF) as draws see
    // them, kept current by NoteConstantWrite as the command stream writes
    // them. Reading all 2048 registers and comparing them for every draw was
    // an eighth of the command processor's time. The title can also write
    // registers as memory, so each frame starts by re-reading them; if they
    // ever differ mid-game, every draw reads them all again (the safe old way).
    alignas(16) uint32_t g_constMirror[2048];
    alignas(16) uint32_t g_constUploaded[2048]; // the block last uploaded this frame
    bool g_constDirty = true;
    bool g_constMirrorTrusted = true;
    uint32_t g_lastConstantOffset = UINT32_MAX; // this frame's last constant block (UINT32_MAX: none yet)
    // Sizes of the last frame's buffers: the next frame reserves about as
    // much up front instead of regrowing (and re-copying) them as it fills.
    struct FrameSizes { size_t vertex = 0, indices = 0, uniforms = 0, draws = 0, textures = 0, cmds = 0; } g_lastFrameSizes;
    // Vertex buffers copied this frame, by (guest address, endian): where the
    // copy starts in the frame's vertex data and how many dwords it holds. A
    // draw that reaches no further than an earlier copy reuses it, so a large
    // shared buffer is copied once, not once per distinct reach.
    struct VertexRange { uint32_t at, words; };
    std::unordered_map<uint64_t, VertexRange> g_vertexRanges;

    render::Frame& Frame()
    {
        if (!g_frame)
        {
            g_frame = std::make_unique<render::Frame>();
            g_frame->number = g_stats.frames;
            g_lastConstantOffset = UINT32_MAX;
            g_vertexRanges.clear();
            const FrameSizes& s = g_lastFrameSizes;
            auto more = [](size_t n) { return n + n / 4; };
            g_frame->vertexData.reserve(more(s.vertex));
            g_frame->indices.reserve(more(s.indices));
            g_frame->uniforms.reserve(more(s.uniforms));
            g_frame->draws.reserve(more(s.draws));
            g_frame->textures.reserve(more(s.textures));
            g_frame->cmds.reserve(more(s.cmds));
        }
        return *g_frame;
    }

    // FRAME_LOG=<first frame>[,<count>]: log every draw and resolve of those frames.
    // A frame picked from the browser console (crysis_dump_frame): logged and dumped like FRAME_LOG/FRAME_DUMP.
    std::atomic<int64_t> g_dumpFrame{ -1 };
    std::atomic<uint32_t> g_constsPsLow{ 0 }; // crysis_log_consts
    std::atomic<uint32_t> g_skipPs[8]{};       // crysis_skip_ps: pixel shader hashes (low 32 bits) not drawn
    // crysis_watch_const: the latest value of one constant register for draws
    // with one pixel shader (cheap, every frame: e.g. a camera vector).
    std::atomic<uint32_t> g_watchPs{ 0 }, g_watchReg{ 0 };
    std::atomic<float> g_watchValue[4];
    std::atomic<uint64_t> g_watchFrame{ 0 };
    bool DumpRequested() { return g_dumpFrame >= 0 && int64_t(g_stats.frames) == g_dumpFrame; }

    bool FrameLog()
    {
        if (DumpRequested())
            return true;
        static int64_t first = -1, count = 1;
        static bool parsed = false;
        if (!parsed)
        {
            parsed = true;
            if (const char* e = getenv("FRAME_LOG"))
            {
                first = atoll(e);
                if (const char* c = strchr(e, ',')) count = atoll(c + 1);
            }
        }
        return first >= 0 && int64_t(g_stats.frames) >= first && int64_t(g_stats.frames) < first + count;
    }

    uint32_t AppendUniform(const void* data, size_t size)
    {
        auto& u = Frame().uniforms;
        size_t off = (u.size() + render::kUniformAlign - 1) & ~size_t(render::kUniformAlign - 1);
        u.resize(off + size);
        memcpy(u.data() + off, data, size);
        return uint32_t(off);
    }

    void SendShader(uint64_t hash, const xenos::Translation& t, bool pixel)
    {
        if (!g_sentShaders.insert(hash).second)
            return;
        render::ShaderSource s;
        s.hash = hash;
        s.pixel = pixel;
        s.wgsl = t.wgsl;
        s.textureMask = t.textureMask;
        memcpy(s.textureDimension, t.textureDimension, 32);
        Frame().shaders.push_back(std::move(s));
    }

    uint8_t HostColorFormat(uint32_t xenosFormat)
    {
        switch (xenosFormat)
        {
        case 0: case 1: return render::RT_RGBA8;
        case 2: case 10: return render::RT_RGB10A2;
        case 3: case 5: case 7: case 12: return render::RT_RGBA16F;
        case 4: case 6: return render::RT_RG16F;
        case 14: return render::RT_R32F;
        case 15: return render::RT_RG32F;
        default: return render::RT_RGBA8;
        }
    }

    // MSAA: EDRAM holds samples, and surfaces with different sample counts
    // alias (Crysis 2 draws stencil at quarter size with 4x MSAA over the
    // full-size depth buffer). Targets live in sample space: 2x doubles the
    // height, 4x doubles both dimensions.
    uint32_t MsaaX() { return ((Reg(RB_SURFACE_INFO) >> 16) & 3) == 2 ? 2 : 1; }
    uint32_t MsaaY() { return ((Reg(RB_SURFACE_INFO) >> 16) & 3) != 0 ? 2 : 1; }

    render::RtKey ColorTarget(uint32_t index)
    {
        uint32_t info = Reg(index == 0 ? RB_COLOR_INFO : RB_COLOR1_INFO + index - 1);
        render::RtKey k;
        k.base = info & 0xFFF;
        k.pitch = (Reg(RB_SURFACE_INFO) & 0x3FFF) * MsaaX();
        k.format = HostColorFormat((info >> 16) & 0xF);
        return k;
    }

    render::RtKey DepthTarget()
    {
        render::RtKey k;
        k.base = Reg(RB_DEPTH_INFO) & 0xFFF;
        k.pitch = (Reg(RB_SURFACE_INFO) & 0x3FFF) * MsaaX();
        k.format = render::RT_DEPTH24S8;
        return k;
    }

    // Render target height needed by the current scissor (EDRAM surfaces have
    // no explicit height).
    uint32_t TargetHeight()
    {
        uint32_t br = Reg(PA_SC_WINDOW_SCISSOR_BR), sbr = Reg(PA_SC_SCREEN_SCISSOR_BR);
        uint32_t h = std::min<uint32_t>((br >> 16) & 0x3FFF, (sbr >> 16) & 0x7FFF);
        if (h == 0 || h > 4096) h = 720;
        return (h + 15) & ~15u;
    }

    // ---------------------------------------------------------------------
    // Textures
    // ---------------------------------------------------------------------
    // DecodeTexture returns display-order RGBA; recover the hardware's raw
    // X/Y/Z/W order for the format, then apply the fetch constant's swizzle
    // (3 bits per output channel: 0-3 = X/Y/Z/W, 4 = 0, 5 = 1).
    void ApplySwizzle(uint32_t format, uint32_t swizzle, std::vector<uint8_t>& rgba)
    {
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        {
            uint8_t* p = &rgba[i];
            uint8_t raw[4];
            switch (format)
            {
            case 2: case 8: case 9: case 59: raw[0] = p[0]; raw[1] = 0; raw[2] = 0; raw[3] = 255; break; // one channel
            case 10: case 49: case 60: raw[0] = p[0]; raw[1] = p[1]; raw[2] = 0; raw[3] = 255; break; // two channels
            case 3: case 4: case 6: case 7: case 14: case 15: case 50:                               // packed BGRA-style
                raw[0] = p[2]; raw[1] = p[1]; raw[2] = p[0]; raw[3] = p[3]; break;
            default: raw[0] = p[0]; raw[1] = p[1]; raw[2] = p[2]; raw[3] = p[3]; break;            // block-compressed RGBA
            }
            for (int c = 0; c < 4; c++)
            {
                uint32_t sel = (swizzle >> (c * 3)) & 7;
                p[c] = sel < 4 ? raw[sel] : sel == 5 ? 255 : 0;
            }
        }
    }

    // Frame dumps (crysis_dump_frame): every decoded texture the frame binds,
    // as it reaches the GPU (after the fetch swizzle), shrunk to at most 128
    // texels wide; cube maps as their six faces stacked. Float formats show
    // their 0-1 range. Named tex_<address>_f<format>_sw<swizzle>.png.
    void DumpTexture(const XenosTexture& t, uint32_t swizzle)
    {
        static std::unordered_set<uint64_t> done;
        static uint64_t doneFrame = ~0ull;
        if (doneFrame != g_stats.frames) { done.clear(); doneFrame = g_stats.frames; }
        if (g_resolved.count(t.address) || !CanDecodeTexture(t.format) || !done.insert((uint64_t(t.address) << 16) ^ swizzle ^ (uint64_t(t.format) << 12)).second)
            return;
        std::vector<uint8_t> rgba;
        if (DecodesToHalf(t.format))
        {
            std::vector<uint8_t> half;
            if (!DecodeTextureHalf(t, half))
                return;
            rgba.resize(half.size() / 2);
            for (size_t i = 0; i < rgba.size(); i++)
            {
                uint16_t h;
                memcpy(&h, &half[i * 2], 2);
                uint32_t e = (h >> 10) & 31, m = h & 1023;
                float v = (h & 0x8000) ? 0.0f : e == 0 ? m / 1024.0f / 16384.0f : (1.0f + m / 1024.0f) * float(1 << e) / 32768.0f;
                rgba[i] = uint8_t(std::min(v, 1.0f) * 255.0f + 0.5f);
            }
        }
        else
        {
            if (!DecodeTexture(t, rgba))
                return;
            ApplySwizzle(t.format, swizzle, rgba);
        }
        uint32_t layers = std::max<uint32_t>(1, t.dimension == 2 ? t.depth : t.layers); // 3D: slices stacked
        uint32_t step = (t.width + 127) / 128;
        uint32_t ow = (t.width + step - 1) / step, ohFace = (t.height + step - 1) / step;
        std::vector<uint8_t> out(size_t(ow) * ohFace * layers * 4);
        for (uint32_t l = 0; l < layers; l++)
            for (uint32_t y = 0; y < ohFace; y++)
                for (uint32_t x = 0; x < ow; x++)
                {
                    const uint8_t* s = &rgba[((size_t(l) * t.height + y * step) * t.width + x * step) * 4];
                    uint8_t* o = &out[((size_t(l) * ohFace + y) * ow + x) * 4];
                    memcpy(o, s, 3);
                    o[3] = 255;
                }
        std::vector<uint8_t> png = EncodePng(out.data(), ow, ohFace * layers);
        char name[96];
        snprintf(name, sizeof(name), "tex_%08X_f%u_sw%03X_%ux%u%s.png", t.address, t.format, swizzle, t.width, t.height,
                 t.dimension == 2 ? "_3d" : layers == 6 ? "_cube" : "");
        PlatformUploadDump(name, png.data(), png.size());
    }

    // Resolved textures hold the render target's RGBA; the fetch swizzle is
    // applied in the shader (after the resolve's red/blue swap). Decoded
    // textures have it applied on upload, so their shader swizzle is identity.
    // signsApplied: the decode already applied the fetch constant's sign modes
    // (normalized 16-bit formats), so the shader must not apply them again.
    constexpr uint32_t kIdentitySwizzle = 0 | 1 << 3 | 2 << 6 | 3 << 9;
    uint32_t TextureFor(const XenosTexture& t, uint32_t swizzle, uint32_t& shaderSwizzle, float rawMax[4], bool& rawInt,
                        bool& signsApplied)
    {
        rawInt = false;
        signsApplied = false;
        shaderSwizzle = kIdentitySwizzle;
        bool norm16 = IsNorm16TextureFormat(t.format);
        float defMax = IsFloatTextureFormat(t.format) ? 1.0f : norm16 ? 65535.0f : 255.0f;
        for (int c = 0; c < 4; c++) rawMax[c] = defMax;
        // Resolves write 2D textures; a 3D fetch never reads one.
        auto r = t.dimension == 2 ? g_resolved.end() : g_resolved.find(t.address);
        if (r != g_resolved.end())
        {
            static const uint32_t perm[2][4] = { { 0, 1, 2, 3 }, { 2, 1, 0, 3 } };
            rawInt = r->second.rawInt;
            for (int c = 0; c < 4; c++)
            {
                float m = r->second.chanMax[perm[r->second.swapRB][c]];
                rawMax[c] = m ? m : r->second.isFloat ? 1.0f : 255.0f; // fixed point, float data, or 8-bit unorm
            }
            uint32_t s = 0;
            for (int c = 0; c < 4; c++)
            {
                uint32_t sel = (swizzle >> (c * 3)) & 7;
                s |= (sel < 4 ? perm[r->second.swapRB][sel] : sel) << (c * 3);
            }
            shaderSwizzle = s;
            return r->second.id;
        }
        if (!CanDecodeTexture(t.format))
            return 0;
        if (DecodesToHalf(t.format))
            shaderSwizzle = swizzle;
        if (norm16)
        {
            // Decoded to RGBA16F with the sign modes applied (the shader's sign
            // conversion handles 8-bit data only); integer fetches scale by the
            // component's own integer range.
            signsApplied = true;
            for (int c = 0; c < 4; c++)
                rawMax[c] = ((t.signs >> (c * 2)) & 3) == 1 ? 32767.0f : 65535.0f;
        }
        // Key from the fields (not the struct bytes: padding is uninitialized).
        // The sign modes only change the decode of normalized 16-bit formats.
        const uint32_t keyFields[] = { t.address, t.width, t.height, t.pitch, t.format, t.endian, t.dimension, t.layers,
                                       t.mipAddress, t.mipLevels, t.packedMips, t.tiled, swizzle, t.depth, norm16 ? t.signs : 0 };
        uint64_t key = Hash64(keyFields, sizeof(keyFields));
        CachedTexture& c = g_textures[key];
        c.lastUsed = g_stats.frames;
        if (c.id && c.checkedFrame == g_stats.frames)
            return c.id;
        size_t bytes = TextureByteSize(t);
        uint64_t h = bytes ? Hash64(GuestPtr(t.address), bytes) : 0;
        Count(prof::CPC_TEX_HASH_BYTES, bytes);
        c.checkedFrame = g_stats.frames;
        if (c.id && c.hash == h)
            return c.id;
        render::TextureUpload up;
        if (DecodesToHalf(t.format))
        {
            // Raw X Y Z W in RGBA16F; the fetch swizzle is applied in the shader.
            if (!DecodeTextureHalf(t, up.rgba))
                return 0;
            up.half = true;
        }
        else
        {
            if (!DecodeTexture(t, up.rgba))
                return 0;
            ApplySwizzle(t.format, swizzle, up.rgba);
            // Mip levels, while they can be located (a gap ends the chain).
            for (uint32_t level = 1; level < t.mipLevels; level++)
            {
                std::vector<uint8_t> mip;
                if (!DecodeTextureMip(t, level, mip))
                    break;
                ApplySwizzle(t.format, swizzle, mip);
                up.mips.push_back(std::move(mip));
            }
            // MIP_CHECK=1: compare level 1 with a box-filtered base level (layout check, debugging).
            static const bool mipCheck = getenv("MIP_CHECK") != nullptr;
            if (mipCheck && !up.mips.empty() && t.width >= 8 && t.height >= 8)
            {
                uint32_t mw = t.width / 2, mh = t.height / 2;
                double diff = 0;
                for (uint32_t y = 0; y < mh; y++)
                    for (uint32_t x = 0; x < mw; x++)
                        for (int c = 0; c < 3; c++)
                        {
                            auto at = [&](uint32_t xx, uint32_t yy) { return double(up.rgba[(size_t(yy) * t.width + xx) * 4 + c]); };
                            double box = (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1)) / 4;
                            diff += std::abs(box - double(up.mips[0][(size_t(y) * mw + x) * 4 + c]));
                        }
                diff /= double(mw) * mh * 3;
                static int good = 0, bad = 0;
                (diff < 12 ? good : bad)++;
                if (diff >= 12 && bad <= 12)
                    Log("MIP_CHECK: mismatch %.1f: %ux%u fmt %u tiled %u packed %u levels %u layers %u mipAddr %08X", diff,
                        t.width, t.height, t.format, t.tiled, t.packedMips, t.mipLevels, t.layers, t.mipAddress);
                if ((good + bad) % 200 == 0)
                    Log("MIP_CHECK: %d textures match their level 1, %d don't", good, bad);
            }
        }
        // TEX_DUMP=<guest address>: save that texture's decoded texels (debugging).
        static const uint32_t dumpAddr = getenv("TEX_DUMP") ? uint32_t(strtoul(getenv("TEX_DUMP"), nullptr, 16)) : 0;
        if (dumpAddr && (t.address & 0x1FFFFFFF) == (dumpAddr & 0x1FFFFFFF))
        {
            char path[64];
            snprintf(path, sizeof(path), "tex_%08X_%ux%u_f%u_%s.raw", t.address, t.width, t.height, t.format, up.half ? "rgba16f" : "rgba8");
            if (FILE* fp = fopen(path, "wb")) { fwrite(up.rgba.data(), 1, up.rgba.size(), fp); fclose(fp); }
            Log("TEX_DUMP: %s (pitch %u, tiled %d, endian %u, swizzle %03X)", path, t.pitch, int(t.tiled), t.endian, swizzle);
        }
        if (!c.id)
            c.id = g_nextTextureId++;
        c.hash = h;
        size_t uploadBytes = up.rgba.size();
        for (const auto& m : up.mips) uploadBytes += m.size();
        g_textureBytes += uploadBytes;
        g_textureBytes -= std::min(g_textureBytes, c.bytes);
        c.bytes = uploadBytes;
        Count(prof::CPC_TEX_DECODE_BYTES, uploadBytes);
        up.id = c.id;
        up.width = t.width;
        up.height = t.height;
        up.layers = t.layers;
        up.is3d = t.dimension == 2;
        up.depth = up.is3d ? t.depth : 1;
        Frame().uploads.push_back(std::move(up));
        return c.id;
    }

    // Texture eviction. Decoded textures used to stay on the GPU forever: the
    // title streams gigabytes of them, so the browser's GPU process grew until
    // memory ran out (the app hung). Textures unused for kMaxIdleFrames are
    // released, and above kBudgetBytes the least recently used go first. The
    // backend frees them when it runs this frame (Frame::releases).
    void EvictTextures(render::Frame& f)
    {
        constexpr uint64_t kMaxIdleFrames = 900;      // about 30 s
        constexpr size_t kBudgetBytes = 1536ull << 20; // decoded texels
        uint64_t now = g_stats.frames;
        bool overBudget = g_textureBytes > kBudgetBytes;
        if (now % 60 != 0 && !overBudget)
            return;
        auto release = [&](std::unordered_map<uint64_t, CachedTexture>::iterator it) {
            if (it->second.id)
                f.releases.push_back(it->second.id);
            g_textureBytes -= std::min(g_textureBytes, it->second.bytes);
            return g_textures.erase(it);
        };
        size_t before = g_textures.size();
        for (auto it = g_textures.begin(); it != g_textures.end();)
            it = now - it->second.lastUsed > kMaxIdleFrames ? release(it) : std::next(it);
        if (g_textureBytes > kBudgetBytes)
        {
            // Oldest first, down to three quarters of the budget; never this frame's.
            std::vector<std::pair<uint64_t, uint64_t>> byAge; // (lastUsed, key)
            for (auto& [key, c] : g_textures)
                if (c.id && c.lastUsed < now) byAge.push_back({ c.lastUsed, key });
            std::sort(byAge.begin(), byAge.end());
            for (auto& [used, key] : byAge)
            {
                if (g_textureBytes <= kBudgetBytes / 4 * 3)
                    break;
                release(g_textures.find(key));
            }
        }
        static uint64_t lastLog = 0;
        if (before != g_textures.size() && now - lastLog >= 600)
        {
            lastLog = now;
            Log("render: texture cache %zu textures, %zu MB (released %zu)", g_textures.size(), g_textureBytes >> 20, before - g_textures.size());
        }
    }

    // Runaway-loop check. Shaders with jumps or loops run as a state machine
    // capped at 65536 steps per pixel (vertex); a loop that doesn't end in our
    // translation then runs the whole cap, and a full-screen draw takes the
    // GPU minutes (the display froze and the browser's memory ballooned).
    // Simulate the control flow on the CPU with the draw's bool and loop
    // constants, exactly as the generated code runs it, assuming per-pixel
    // predicates never break out (the worst case), and log shaders that would
    // run away. Once per shader and constant set.
    uint32_t SimulateControlFlow(const std::vector<xenos::ControlFlow>& cf, const uint32_t* bools, const uint32_t* loops, std::string& trace)
    {
        using xenos::CfOp;
        auto boolConst = [&](uint32_t i) { return ((bools[i / 32] >> (i % 32)) & 1) != 0; };
        int32_t lcount[4] = {}, liter[4] = {}, ld = -1;
        uint32_t cstack[4] = {}, cd = 0, pc = 0, steps = 0;
        while (pc < cf.size() && steps < 70000)
        {
            const xenos::ControlFlow& c = cf[pc];
            steps++;
            if (steps <= 64)
                trace += std::to_string(pc) + (c.op == CfOp::LoopStart ? "L" : c.op == CfOp::LoopEnd ? "E" : c.op == CfOp::CondJmp ? "J" : "") + " ";
            uint32_t next = pc + 1;
            switch (c.op)
            {
            case CfOp::LoopStart:
            {
                uint32_t lc = loops[c.loopId & 31];
                ld = std::min(ld + 1, 3);
                lcount[ld] = int32_t(lc & 0xFF);
                liter[ld] = 0;
                if (lcount[ld] <= 0) { ld--; pc = c.address; } else pc = next;
                continue;
            }
            case CfOp::LoopEnd:
                liter[std::max(ld, 0)] += 1;
                if (ld >= 0 && liter[ld] < lcount[ld]) pc = c.address;
                else { ld = std::max(ld - 1, -1); pc = next; }
                continue;
            case CfOp::CondJmp:
            case CfOp::CondCall:
            {
                bool taken = c.unconditional ? true : c.predicated ? false : boolConst(c.boolIndex) == c.condition;
                if (taken && c.op == CfOp::CondCall) { cstack[std::min(cd, 3u)] = next; cd = std::min(cd + 1, 4u); }
                pc = taken ? c.address : next;
                continue;
            }
            case CfOp::Return:
                if (cd > 0) { cd--; pc = cstack[cd]; } else pc = next;
                continue;
            default:
                if (c.IsExec() && c.IsEnd())
                {
                    bool runs = c.UsesBool() ? boolConst(c.boolIndex) == c.condition : !c.UsesPredicate();
                    if (runs)
                        return steps;
                }
                pc = next;
            }
        }
        return steps;
    }

    void CheckRunawayLoops(uint64_t hash, const xenos::Translation* t, const uint32_t* bools, const uint32_t* loops)
    {
        if (!t || !t->usesStateMachine || t->controlFlow.empty())
            return;
        uint64_t parts[1 + 8 + 32];
        parts[0] = hash;
        for (int i = 0; i < 8; i++) parts[1 + i] = bools[i];
        for (int i = 0; i < 32; i++) parts[9 + i] = loops[i];
        static std::unordered_set<uint64_t> checked;
        if (!checked.insert(Hash64(parts, sizeof(parts))).second)
            return;
        std::string trace;
        uint32_t steps = SimulateControlFlow(t->controlFlow, bools, loops, trace);
        static uint32_t worst = 0;
        if (steps > 4096 || steps > worst * 2)
        {
            worst = std::max(worst, steps);
            std::string loopInfo;
            for (const auto& c : t->controlFlow)
                if (c.op == xenos::CfOp::LoopStart)
                {
                    char b[64];
                    snprintf(b, sizeof(b), " L%u=%08X", c.loopId, loops[c.loopId & 31]);
                    loopInfo += b;
                }
            Log("render: shader %016llX control flow runs %u steps%s (%zu CFs;%s) path: %s", (unsigned long long)hash, steps,
                steps >= 16384 ? " - RUNAWAY, would hit the cap" : "", t->controlFlow.size(), loopInfo.c_str(), trace.c_str());
        }
    }

    uint16_t SamplerBits(const uint32_t f[6])
    {
        uint32_t clamp = (f[0] >> 10) & 0x1FF;     // x, y, z: 3 bits each
        uint32_t filters = (f[3] >> 19) & 0x3F;    // mag, min, mip: 2 bits each
        return uint16_t(clamp | (filters << 9));
    }

    // ---------------------------------------------------------------------
    // Indices
    // ---------------------------------------------------------------------
    // Converts the primitive type to a WebGPU topology, rewriting the index
    // list for types WebGPU lacks (fans, quads, line loops). Returns false for
    // unsupported types.
    bool ConvertPrimitive(uint32_t prim, render::PodVector<uint32_t>& idx, uint8_t& topology)
    {
        switch (prim)
        {
        case 1: return false; // point lists need sprite expansion (WebGPU points are 1 pixel)
        case 2: topology = render::TOP_LINES; return true;
        case 3: topology = render::TOP_LINE_STRIP; return true;
        case 4: topology = render::TOP_TRIANGLES; return true;
        case 6: topology = render::TOP_TRIANGLE_STRIP; return true;
        case 5: // triangle fan
        {
            render::PodVector<uint32_t> out;
            for (size_t i = 2; i < idx.size(); i++)
                out.insert(out.end(), { idx[0], idx[i - 1], idx[i] });
            idx.swap(out);
            topology = render::TOP_TRIANGLES;
            return true;
        }
        case 13: // quad list
        {
            render::PodVector<uint32_t> out;
            for (size_t i = 0; i + 3 < idx.size(); i += 4)
                out.insert(out.end(), { idx[i], idx[i + 1], idx[i + 2], idx[i], idx[i + 2], idx[i + 3] });
            idx.swap(out);
            topology = render::TOP_TRIANGLES;
            return true;
        }
        case 12: // line loop
            if (!idx.empty()) idx.push_back(idx[0]);
            topology = render::TOP_LINE_STRIP;
            return true;
        default:
            return false;
        }
    }

    // ---------------------------------------------------------------------
    // Resolves
    // ---------------------------------------------------------------------
    void RecordResolve()
    {
        PhaseTimer phase(prof::CP_RESOLVE);
        g_stats.resolves++;
        uint32_t copyControl = Reg(RB_COPY_CONTROL);
        uint32_t srcSelect = copyControl & 7;
        bool clearColor = (copyControl >> 8) & 1, clearDepth = (copyControl >> 9) & 1;
        uint32_t command = (copyControl >> 20) & 3; // 0 raw, 1 convert, 2 constant one, 3 null (clear only)

        // The rectangle comes from the draw's vertices: three float2 corners in
        // the buffer of vertex fetch constant 0.
        uint32_t vf0 = Reg(FETCH_CONSTANTS), vf1 = Reg(FETCH_CONSTANTS + 1);
        float xs[3], ys[3];
        uint32_t vaddr = PhysToGuest(vf0 & ~3u);
        for (int i = 0; i < 3; i++)
        {
            uint32_t x = GpuDword(vaddr + i * 8, vf1 & 3), y = GpuDword(vaddr + i * 8 + 4, vf1 & 3);
            memcpy(&xs[i], &x, 4);
            memcpy(&ys[i], &y, 4);
        }
        // D3D-style pixel centers (PA_SU_VTX_CNTL.pix_center = 0) put the
        // corners at -0.5: shift by half a pixel, then take whole pixels.
        float half = (Reg(0x2302) & 1) ? 0.0f : 0.5f;
        float x0 = std::floor(std::min({ xs[0], xs[1], xs[2] }) + half), x1 = std::floor(std::max({ xs[0], xs[1], xs[2] }) + half);
        float y0 = std::floor(std::min({ ys[0], ys[1], ys[2] }) + half), y1 = std::floor(std::max({ ys[0], ys[1], ys[2] }) + half);
        if ((Reg(PA_SU_SC_MODE_CNTL) >> 16) & 1)
        {
            uint32_t wo = Reg(PA_SC_WINDOW_OFFSET);
            int32_t ox = int32_t(wo << 17) >> 17, oy = int32_t(wo << 1) >> 17;
            x0 += ox; x1 += ox; y0 += oy; y1 += oy;
        }

        render::ResolveCmd rc{};
        rc.src = srcSelect == 4 ? DepthTarget() : ColorTarget(srcSelect & 3);
        uint32_t srcXenos = srcSelect == 4 ? 0xFF : XenosColorFormat(srcSelect & 3), drawnXenos = srcXenos;
        auto is7e3 = [](uint32_t x) { return x == 3 || x == 12; };
        if (srcSelect == 4)
            rc.convert = 2; // depth: the value in every channel
        if (srcSelect != 4)
        {
            auto it = g_lastDrawn.find(SurfaceKey(rc.src.base, rc.src.pitch));
            if (it != g_lastDrawn.end() && it->second.host != rc.src.format)
            {
                // Drawn as one format, resolved as another: read the drawn
                // surface; 7e3 float read as 10:10:10:2 gives its raw bits.
                drawnXenos = it->second.xenos;
                rc.src.format = it->second.host;
            }
        }
        rc.rtHeight = std::max<uint32_t>(TargetHeight(), uint32_t(std::max(0.0f, y1))) * MsaaY();
        rc.scaleX = uint8_t(MsaaX());
        rc.scaleY = uint8_t(MsaaY());
        rc.x = int32_t(std::max(0.0f, x0));
        rc.y = int32_t(std::max(0.0f, y0));
        rc.w = std::max(0, int32_t(x1) - rc.x);
        rc.h = std::max(0, int32_t(y1) - rc.y);
        rc.copy = command != 3 && rc.w > 0 && rc.h > 0;

        rc.clamp7e3 = is7e3(drawnXenos);
        // Drawn as 7e3, resolved as 10:10:10:2 unorm: the EDRAM bits are taken
        // as they are, so the copy holds 7e3 bit patterns (2: encode them).
        bool asUnorm1010102 = srcXenos == 2 || srcXenos == 10;
        if (rc.clamp7e3 && asUnorm1010102 && !is7e3(srcXenos))
            rc.clamp7e3 = 2;

        // Integer destination number formats with an exponent bias store
        // fixed point: value * 2^bias, as an integer of the channel's width.
        uint32_t destInfo = Reg(RB_COPY_DEST_INFO);
        uint32_t destFormat = (destInfo >> 7) & 0x3F, destNumber = (destInfo >> 13) & 7;
        int32_t expBias = int32_t(destInfo << 10) >> 26; // bits 16-21, signed
        static const bool frameRaw = getenv("FRAME_RAW") != nullptr; // debugging: logged frames keep raw values
        if (rc.copy && srcSelect != 4 && destNumber != 7 && expBias != 0 && !(frameRaw && FrameLog()))
        {
            float maxv[4] = { 255, 255, 255, 255 };
            switch (destFormat)
            {
            case 7: maxv[0] = maxv[1] = maxv[2] = 1023; maxv[3] = 3; break;      // 2_10_10_10
            case 16: maxv[0] = maxv[1] = 2047; maxv[2] = 1023; maxv[3] = 1; break; // 10_11_11
            case 17: maxv[0] = 1023; maxv[1] = maxv[2] = 2047; maxv[3] = 1; break; // 11_11_10
            case 24: case 25: case 26: maxv[0] = maxv[1] = maxv[2] = maxv[3] = 65535; break; // 16-bit
            }
            rc.convert = 1;
            memcpy(rc.chanMaxStore, maxv, sizeof(maxv));
            for (int c = 0; c < 4; c++)
            {
                rc.mul[c] = std::ldexp(1.0f, expBias);
                rc.maxv[c] = maxv[c];
            }
        }

        if (rc.copy)
        {
            uint32_t dest = PhysToGuest(Reg(RB_COPY_DEST_BASE));
            uint32_t pitch = Reg(RB_COPY_DEST_PITCH);
            uint32_t dw = std::max<uint32_t>(pitch & 0x3FFF, uint32_t(rc.w)), dh = std::max<uint32_t>((pitch >> 16) & 0x3FFF, uint32_t(rc.h));
            bool isFloat = rc.src.format == render::RT_RGBA16F || rc.src.format == render::RT_RG16F ||
                           rc.src.format == render::RT_R32F || rc.src.format == render::RT_RG32F;
            if (!g_resolved.count(dest))
            {
                static int logged = 0;
                if (logged++ < 40)
                    Log("render: first resolve to %08X: %ux%u from base %u pitch %u fmt %u, dest info %08X, frame %llu",
                        dest, dw, dh, rc.src.base, rc.src.pitch, rc.src.format, Reg(RB_COPY_DEST_INFO), (unsigned long long)g_stats.frames);
            }
            // One texture id per destination address. A new id per size change
            // leaked the old GPU texture: some passes reuse an address at other
            // sizes every frame, and the browser's GPU process grew by hundreds
            // of MB a second until the machine ran out of memory. The backend
            // resizes the texture in place when the size or format changes.
            Resolved& r = g_resolved[dest];
            if (!r.id)
                r = { g_nextTextureId++, dw, dh, isFloat, false };
            r.w = dw;
            r.h = dh;
            r.isFloat = isFloat;
            r.swapRB = (Reg(RB_COPY_DEST_INFO) >> 24) & 1; // copy_dest_swap: red and blue exchanged in memory
            memcpy(r.chanMax, rc.chanMaxStore, sizeof(r.chanMax));
            r.rawInt = rc.convert == 1; // stored as integers, not normalized
            r.raw7e3 = rc.clamp7e3 == 2; // 7e3 bit patterns: never filter between them
            rc.destId = r.id;
            rc.destAddr = dest;
            rc.destW = dw;
            rc.destH = dh;
            rc.destFloat = isFloat;

            // Small 8:8:8:8 and 16:16 float resolves go back to guest memory as
            // well: the CPU reads some, and saw zeros (or its own initial values)
            // because results only lived on the host GPU. CryEngine's occlusion
            // depth is 8:8:8:8; its adapted scene luminance (16:16 float, two
            // 8x8 buffers in turn) sets the exposure, which stayed at its
            // starting value and left every scene far too bright.
            static const bool writeback = !getenv("RESOLVE_WRITEBACK") || atoi(getenv("RESOLVE_WRITEBACK")) != 0;
            uint32_t endian = destInfo & 7;
            bool half2 = destFormat == 31 && isFloat && srcSelect != 4;
            if (writeback && (destFormat == 6 || half2) && uint32_t(rc.w) * uint32_t(rc.h) <= 16384 && rc.convert != 1 && endian <= 3)
            {
                rc.wbAddr = dest;
                rc.wbPitch = pitch & 0x3FFF;
                rc.wbEndian = uint8_t(endian);
                rc.wbSwap = uint8_t((destInfo >> 24) & 1);
                rc.wbDepth = srcSelect == 4;
                rc.wbHalf2 = half2;
            }
        }
        if (clearColor)
        {
            rc.clearColor = ColorTarget(srcSelect & 3);
            uint32_t v = Reg(RB_COLOR_CLEAR); // A8R8G8B8 for 32bpp targets
            rc.clearValue[0] = ((v >> 16) & 0xFF) / 255.0f;
            rc.clearValue[1] = ((v >> 8) & 0xFF) / 255.0f;
            rc.clearValue[2] = (v & 0xFF) / 255.0f;
            rc.clearValue[3] = (v >> 24) / 255.0f;
        }
        if (clearDepth)
        {
            rc.clearDepth = DepthTarget();
            uint32_t v = Reg(RB_DEPTH_CLEAR);
            rc.depthValue = (v >> 8) / 16777215.0f;
            rc.stencilValue = v & 0xFF;
        }
        if (FrameLog())
            Log("framelog %llu: RESOLVE xenos fmt %u (drawn %u, convert %u) src=%u base %u pitch %u fmt %u rect %d,%d %dx%d rtH %u copy %d cmd %u dest %08X pitch %08X info %08X ctl %08X clear c%d d%d colorclear %08X winofs %08X",
                (unsigned long long)g_stats.frames, srcXenos, drawnXenos, unsigned(rc.convert), srcSelect, rc.src.base, rc.src.pitch, rc.src.format, rc.x, rc.y, rc.w, rc.h, rc.rtHeight,
                int(rc.copy), command, Reg(RB_COPY_DEST_BASE), Reg(RB_COPY_DEST_PITCH), Reg(RB_COPY_DEST_INFO), copyControl,
                int(clearColor), int(clearDepth), Reg(RB_COLOR_CLEAR), Reg(PA_SC_WINDOW_OFFSET));
        if (!rc.copy && !clearColor && !clearDepth)
            return;
        auto& f = Frame();
        f.cmds.push_back({ render::Cmd::Resolve, uint32_t(f.resolves.size()) });
        f.resolves.push_back(rc);
    }
}

namespace
{
    // EDRAM aliasing, one pattern of it. Depth and color surfaces share EDRAM,
    // but the host keeps each (base, pitch, format) in its own texture. Crysis 2
    // clears its 8:8:8:8 velocity buffer by drawing a depth/stencil-only
    // rectangle (depth ALWAYS + write, stencil ALWAYS/REPLACE) into a depth
    // surface placed over the color surface's EDRAM: each 24:8 depth/stencil
    // dword then reads back as the color (stencil, depth bits 0-7, 8-15, 16-23)
    // = (0x7F, 0x7F, 0, 0), "no motion". Without this the buffer stayed zero,
    // decoded as maximum motion everywhere, and the whole frame was blurred.
    // The underwater light buffer (288x180 at EDRAM 675) is cleared the same
    // way: strips to the right and below by a color rectangle, the remaining
    // 240x176 by a 4xMSAA depth clear of 120x88 pixels at z = -2^-28. Without
    // the mirror the ten additive passes drawn into it each frame piled up
    // until the screen turned white.
    // Mirror such a clear onto the aliased color surface (whole surface: the
    // game covers the rest with a color rectangle of the same value).
    void AliasedDepthClear(const render::DrawCmd& d, bool rect)
    {
        const render::PipelineKey& p = d.pipe;
        if (!rect || !d.depth.Valid())
            return;
        for (int i = 0; i < 4; i++)
            if (p.colorFormat[i] != render::kNone)
                return;
        uint32_t dc = Reg(RB_DEPTHCONTROL), srm = Reg(RB_STENCILREFMASK);
        bool depthAlways = (dc & 6) == 6 && ((dc >> 4) & 7) == 7;
        bool stencilReplace = (dc & 1) && ((dc >> 8) & 7) == 7 && ((dc >> 14) & 7) == 2 && ((srm >> 16) & 0xFF) == 0xFF;
        if (!depthAlways || !stencilReplace)
            return;
        auto drawn = g_lastDrawn.find(SurfaceKey(d.depth.base, d.depth.pitch));
        if (drawn == g_lastDrawn.end() || drawn->second.host != render::RT_RGBA8 || drawn->second.xenos != 0)
            return;
        // The rectangle's depth: z of its three corners, laid out as Crysis 2's
        // pass-through clear vertices are (x y z, then a float4 color: 7
        // dwords). Accept only if all three agree and lie in [0, 1], give or
        // take rounding (the hardware clamps depth to that range).
        uint32_t vf0 = Reg(FETCH_CONSTANTS), vf1 = Reg(FETCH_CONSTANTS + 1);
        uint32_t vaddr = PhysToGuest(vf0 & ~3u);
        float x[3], y[3], z[3];
        for (int i = 0; i < 3; i++)
        {
            float* out[3] = { &x[i], &y[i], &z[i] };
            for (int c = 0; c < 3; c++)
            {
                uint32_t bits = GpuDword(vaddr + (i * 7 + c) * 4, vf1 & 3);
                memcpy(out[c], &bits, 4);
            }
        }
        if (!(z[0] == z[1] && z[1] == z[2]) || !(z[0] >= -1e-4f && z[0] <= 1.0001f))
            return;
        // Only clears: from the surface's corner across most of its width, as
        // the mirrored clear covers the entire host surface.
        float x0 = std::min({ x[0], x[1], x[2] }), x1 = std::max({ x[0], x[1], x[2] });
        float y0 = std::min({ y[0], y[1], y[2] });
        if (x0 > 0.5f || y0 > 0.5f || (x1 - x0) * MsaaX() < 0.5f * d.depth.pitch)
            return;
        float depth = std::clamp(z[0], 0.0f, 1.0f);
        uint32_t d24 = uint32_t(std::lround(double(depth) * 16777215.0)), stencil = srm & 0xFF;
        uint32_t dword = (d24 << 8) | stencil; // as 8:8:8:8: R bits 0-7, G 8-15, B 16-23, A 24-31
        render::ResolveCmd rc{};
        rc.rtHeight = d.rtHeight / MsaaY();
        rc.copy = false;
        rc.clearColor = d.depth;
        rc.clearColor.format = render::RT_RGBA8;
        for (int c = 0; c < 4; c++)
            rc.clearValue[c] = ((dword >> (c * 8)) & 0xFF) / 255.0f;
        render::Frame& f = Frame();
        f.cmds.push_back({ render::Cmd::Resolve, uint32_t(f.resolves.size()) });
        f.resolves.push_back(rc);
        static int logged = 0;
        if (logged++ < 3 || FrameLog())
            Log("render: depth clear at EDRAM %u (pitch %u) mirrored onto the 8:8:8:8 surface there as %08X",
                d.depth.base, d.depth.pitch, dword);
    }
}

void NoteConstantWrite(uint32_t index, uint32_t value)
{
    g_constMirror[index & 2047] = value;
    g_constDirty = true;
}

bool RenderingEnabled()
{
    static const bool enabled = !getenv("RENDER") || atoi(getenv("RENDER")) != 0;
    return enabled;
}

void RecordDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexSize,
                const uint32_t* immediate, uint32_t immediateDwords, const DrawShaders& shaders)
{
    g_stats.draws++;
    uint32_t edramMode = Reg(RB_MODECONTROL) & 7;
    if (edramMode == 6)
    {
        RecordResolve();
        return;
    }
    if (edramMode != 4 && edramMode != 5)
    {
        static std::atomic<uint64_t> skipped[8];
        uint64_t n = skipped[edramMode]++;
        if ((n & (n - 1)) == 0 && n >= 1) // log at powers of two
            Log("render: draw with EDRAM mode %u skipped (%llu so far; RB_MODECONTROL=%08X, initiator %08X, VS %016llX PS %016llX)",
                edramMode, (unsigned long long)n + 1, Reg(RB_MODECONTROL), initiator,
                (unsigned long long)shaders.vsHash, (unsigned long long)shaders.psHash);
        return;
    }
    if (!shaders.vs || !shaders.ps)
    {
        g_stats.noShader++;
        return;
    }
    PhaseTimer phase(prof::CP_INDICES);

    // --- Indices ---------------------------------------------------------
    uint32_t prim = initiator & 0x3F, source = (initiator >> 6) & 3;
    bool index32 = (initiator >> 11) & 1;
    uint32_t count = initiator >> 16;
    bool rect = prim == 8; // rectangle list: expanded by the vertex shader's main_rect entry point
    if (rect)
        g_stats.rectLists++;
    // (Reused between draws: one allocation instead of one per draw.)
    static render::PodVector<uint32_t> idx;
    idx.clear();
    bool indexed = source != 2;
    uint32_t offset = Reg(VGT_INDX_OFFSET);
    bool restart = (Reg(PA_SU_SC_MODE_CNTL) >> 21) & 1;
    uint32_t resetIndex = Reg(VGT_MULTI_PRIM_IB_RESET_INDX);
    // The largest vertex index the draw uses (for how much vertex data to copy).
    uint32_t maxIndex = 0;
    bool maxKnown = false;
    if (source == 0)
    {
        uint32_t base = PhysToGuest(indexBase), endian = indexSize >> 30;
        idx.resize(count);
        maxIndex = ReadIndices(idx.data(), (const uint8_t*)GuestPtr(base), count, index32, endian, restart, resetIndex, offset);
        maxKnown = true;
    }
    else if (source == 1)
    {
        idx.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t w = index32 ? i : i / 2;
            uint32_t v = w < immediateDwords ? immediate[w] : 0;
            idx[i] = index32 ? v : ((i & 1) ? v >> 16 : v & 0xFFFF);
        }
        for (auto& v : idx)
        {
            if (restart && v == (index32 ? resetIndex : (resetIndex & 0xFFFF)))
                v = 0xFFFFFFFF;
            else
                v += offset;
        }
    }

    uint8_t topology = render::TOP_TRIANGLES;
    bool needsList = prim == 5 || prim == 13 || prim == 12 || rect;
    if (!indexed && needsList)
    {
        idx.resize(count);
        for (uint32_t i = 0; i < count; i++) idx[i] = i + offset;
        indexed = true;
    }
    // (Point lists stay skipped: Crysis 2's ~300 per frame have no color
    // target, and WebGPU point pipelines allow only 15 vertex outputs.)
    if (rect)
        idx.resize(idx.size() / 3 * 3);
    else if (!ConvertPrimitive(prim, idx, topology))
    {
        static std::atomic<uint64_t> warned{ 0 };
        uint64_t w = warned++;
        if (w < 10 || ((w & (w - 1)) == 0))
            Log("render: skipping primitive type %u (%u indices, source %u, mode %u) VS %016llX PS %016llX pts %d pitch %u scissor %08X-%08X point size %08X",
                prim, count, source, edramMode, (unsigned long long)shaders.vsHash, (unsigned long long)shaders.psHash,
                int(shaders.vs->writesPointSize), Reg(RB_SURFACE_INFO) & 0x3FFF, Reg(PA_SC_WINDOW_SCISSOR_TL), Reg(PA_SC_WINDOW_SCISSOR_BR),
                Reg(0x2280));
        g_stats.badPrim++;
        return;
    }
    if (indexed && idx.empty())
        return;
    Count(prof::CPC_INDICES, idx.size());
    phase.Next(prof::CP_STATE);

    render::Frame& f = Frame();
    SendShader(shaders.vsHash, *shaders.vs, false);
    SendShader(shaders.psHash, *shaders.ps, true);

    render::DrawCmd d{};
    render::PipelineKey& p = d.pipe;
    memset(&p, 0, sizeof(p));
    p.vs = shaders.vsHash;
    p.ps = shaders.psHash;
    p.topology = topology;
    p.stripIndexed = indexed && (topology == render::TOP_TRIANGLE_STRIP || topology == render::TOP_LINE_STRIP);
    p.rectList = rect;

    // --- Render targets and output state ---------------------------------
    uint32_t colorMask = Reg(RB_COLOR_MASK);
    for (int i = 0; i < 4; i++)
    {
        p.colorFormat[i] = render::kNone;
        bool written = edramMode == 4 && (shaders.ps->colorTargetMask >> i) & 1 && ((colorMask >> (i * 4)) & 0xF);
        if (!written)
            continue;
        d.color[i] = ColorTarget(i);
        p.colorFormat[i] = d.color[i].format;
        g_lastDrawn[SurfaceKey(d.color[i].base, d.color[i].pitch)] = { d.color[i].format, uint8_t(XenosColorFormat(i)) };
        p.writeMask[i] = uint8_t((colorMask >> (i * 4)) & 0xF);
        p.blend[i] = Reg(i == 0 ? RB_BLENDCONTROL0 : RB_BLENDCONTROL1 + i - 1);
    }
    d.depth = DepthTarget();
    p.depthFormat = render::RT_DEPTH24S8;
    if (!d.depth.pitch)
    {
        g_stats.noTarget++;
        return;
    }
    uint32_t dc = Reg(RB_DEPTHCONTROL);
    p.depthTest = (dc >> 1) & 1;
    p.depthWrite = (dc >> 2) & 1;
    p.depthFunc = (dc >> 4) & 7;
    p.stencilEnable = dc & 1;
    uint32_t srm = Reg(RB_STENCILREFMASK);
    p.stencilReadMask = uint8_t(srm >> 8);
    p.stencilWriteMask = uint8_t(srm >> 16);
    uint32_t back = ((dc >> 7) & 1) ? (dc >> 20) & 0xFFF : (dc >> 8) & 0xFFF;
    p.stencilOps = ((dc >> 8) & 0xFFF) | (back << 12);
    d.stencilRef = srm & 0xFF;
    uint32_t mode = Reg(PA_SU_SC_MODE_CNTL);
    p.cull = rect ? 0 : (mode & 3) == 1 ? 1 : (mode & 3) == 2 ? 2 : 0;
    p.frontCW = (mode >> 2) & 1;
    for (int i = 0; i < 4; i++)
        d.blendConstant[i] = RegF(RB_BLEND_RED + i);

    // --- Viewport: fold the Xenos viewport transform into the shader's
    // position transform so the host viewport can cover the whole target.
    // The transform maps guest pixels to NDC over (pitch x height) pixels; the
    // backend's viewport covers the same area in samples (d.rtHeight).
    uint32_t sx = MsaaX(), sy = MsaaY();
    uint32_t pitch = d.depth.pitch / sx, height = TargetHeight();
    d.rtHeight = height * sy;
    float W = float(pitch), H = float(height);
    uint32_t vte = Reg(PA_CL_VTE_CNTL);
    float xs = (vte & 1) ? RegF(PA_CL_VPORT_XSCALE) : 1.0f, xo = (vte & 2) ? RegF(PA_CL_VPORT_XSCALE + 1) : 0.0f;
    float ys = (vte & 4) ? RegF(PA_CL_VPORT_XSCALE + 2) : 1.0f, yo = (vte & 8) ? RegF(PA_CL_VPORT_XSCALE + 3) : 0.0f;
    float zs = (vte & 16) ? RegF(PA_CL_VPORT_XSCALE + 4) : 1.0f, zo = (vte & 32) ? RegF(PA_CL_VPORT_XSCALE + 5) : 0.0f;
    int32_t winX = 0, winY = 0;
    uint32_t wo = Reg(PA_SC_WINDOW_OFFSET);
    if ((mode >> 16) & 1)
    {
        winX = int32_t(wo << 17) >> 17;
        winY = int32_t(wo << 1) >> 17;
        xo += float(winX);
        yo += float(winY);
    }
    // D3D9 pixel centers (PA_SU_VTX_CNTL.pix_center = 0): guest pixel centers are
    // at integer coordinates, host ones at +0.5, so move everything by half a pixel.
    if (!(Reg(0x2302) & 1))
    {
        xo += 0.5f;
        yo += 0.5f;
    }
    xenos::MiscConstants misc{};
    for (uint32_t& s : misc.textureSwizzle) s = kIdentitySwizzle;
    for (auto& sc : misc.textureScale) sc[0] = sc[1] = sc[2] = sc[3] = 1.0f;
    for (auto& ts : misc.texcoordScale) ts[0] = ts[1] = 1.0f;
    for (int i = 0; i < 8; i++) misc.bools[i] = Reg(BOOL_CONSTANTS + i);
    for (int i = 0; i < 32; i++) misc.loops[i] = Reg(LOOP_CONSTANTS + i);
    CheckRunawayLoops(shaders.vsHash, shaders.vs, misc.bools, misc.loops);
    CheckRunawayLoops(shaders.psHash, shaders.ps, misc.bools, misc.loops);
    misc.positionScale[0] = 2.0f * xs / W;
    misc.positionOffset[0] = 2.0f * xo / W - 1.0f;
    misc.positionScale[1] = -2.0f * ys / H;
    misc.positionOffset[1] = 1.0f - 2.0f * yo / H;
    misc.positionScale[2] = zs;
    misc.positionOffset[2] = zo;
    uint32_t colorControl = Reg(RB_COLORCONTROL);
    misc.alphaRef[0] = RegF(RB_ALPHA_REF);
    misc.alphaRef[1] = (colorControl & 8) ? float(colorControl & 7) : 7.0f;
    misc.alphaRef[3] = (colorControl & 16) ? 1.0f : 0.0f; // alpha to mask (coverage from alpha)
    // SQ_PROGRAM_CNTL.param_gen (bit 18): the pixel position goes to the
    // interpolator register SQ_CONTEXT_MISC.param_gen_pos (bits 8-15).
    misc.paramGen[0] = (Reg(0x2180) >> 18) & 1 ? float((Reg(0x2181) >> 8) & 0xFF) : -1.0f;
    misc.paramGen[1] = (Reg(0x2302) & 1) ? 0.0f : 0.5f;
    misc.paramGen[2] = 16384.0f; // state-machine shaders' step cap (see xenos_wgsl.cpp)

    // Scissor: window scissor (offset by the window offset unless disabled)
    // intersected with the screen scissor.
    uint32_t tl = Reg(PA_SC_WINDOW_SCISSOR_TL), br = Reg(PA_SC_WINDOW_SCISSOR_BR);
    int32_t sx0 = tl & 0x3FFF, sy0 = (tl >> 16) & 0x3FFF, sx1 = br & 0x3FFF, sy1 = (br >> 16) & 0x3FFF;
    if (!((tl >> 31) & 1))
    {
        int32_t ox = int32_t(wo << 17) >> 17, oy = int32_t(wo << 1) >> 17;
        sx0 += ox; sx1 += ox; sy0 += oy; sy1 += oy;
    }
    uint32_t stl = Reg(PA_SC_SCREEN_SCISSOR_TL), sbr = Reg(PA_SC_SCREEN_SCISSOR_BR);
    sx0 = std::max<int32_t>(sx0, int32_t(stl << 17) >> 17);
    sy0 = std::max<int32_t>(sy0, int32_t(stl << 1) >> 17);
    sx1 = std::min<int32_t>(sx1, int32_t(sbr << 17) >> 17);
    sy1 = std::min<int32_t>(sy1, int32_t(sbr << 1) >> 17);
    sx0 = std::clamp<int32_t>(sx0, 0, int32_t(pitch));
    sx1 = std::clamp<int32_t>(sx1, sx0, int32_t(pitch));
    sy0 = std::clamp<int32_t>(sy0, 0, int32_t(height));
    sy1 = std::clamp<int32_t>(sy1, sy0, int32_t(height));
    d.scissor[0] = uint32_t(sx0) * sx;
    d.scissor[1] = uint32_t(sy0) * sy;
    d.scissor[2] = uint32_t(sx1 - sx0) * sx;
    d.scissor[3] = uint32_t(sy1 - sy0) * sy;
    if (!d.scissor[2] || !d.scissor[3])
        return;

    // --- Vertex data -------------------------------------------------------
    phase.Next(prof::CP_VERTEX);
    // Copy only what this draw can reach: up to the highest vertex index times
    // the fetch stride (plus offset and one element), within the buffer size.
    // (Primitive conversion only repeats indices, so a maximum read with the
    // indices still holds.)
    if (!maxKnown && indexed)
    {
        for (uint32_t v : idx)
            if (v != 0xFFFFFFFF) maxIndex = std::max(maxIndex, v);
    }
    else if (!indexed)
        maxIndex = offset + (count ? count - 1 : 0);
    for (uint32_t k = 0; k < 96; k++)
    {
        if (!((shaders.vs->vertexFetchMask[k / 32] >> (k % 32)) & 1))
            continue;
        uint32_t d0 = Reg(FETCH_CONSTANTS + k * 2), d1 = Reg(FETCH_CONSTANTS + k * 2 + 1);
        uint32_t addr = PhysToGuest(d0 & ~3u), endian = d1 & 3, words = (d1 >> 2) & 0xFFFFFF;
        uint64_t reach = 0;
        for (const auto& use : shaders.vs->vertexFetches)
            if (use.constIndex == k)
                reach = std::max<uint64_t>(reach, uint64_t(maxIndex) * use.stride + uint64_t(std::max(use.offset, 0)) + 4);
        uint32_t full = words;
        words = uint32_t(std::min<uint64_t>(words, reach));
        uint64_t key = (uint64_t(addr) << 2) | endian;
        auto it = g_vertexRanges.find(key);
        const uint32_t* src = (const uint32_t*)GuestPtr(addr);
        // An earlier copy is reused only if it covers this draw's reach and
        // still matches guest memory: dynamic buffers (full-screen quads, UI)
        // are rewritten at the same address within a frame. Small buffers are
        // compared in full; large ones (static meshes in practice) at the ends
        // and at sampled positions.
        auto stillValid = [&](const VertexRange& r) {
            if (r.words < words)
                return false;
            Count(prof::CPC_VTX_CHECK_BYTES, words <= 16384 ? words * 4 : 3072 * 4);
            const uint32_t* copy = f.vertexData.data() + r.at;
            auto same = [&](uint32_t i) { return copy[i] == Swap(src[i], endian); };
            if (words <= 16384)
                return EqualSwap32(copy, src, words, endian);
            if (!EqualSwap32(copy, src, 256, endian) || !EqualSwap32(copy + words - 256, src + words - 256, 256, endian))
                return false;
            for (uint32_t s = 0; s < 1024; s++)
                if (!same(uint32_t(uint64_t(words - 1) * s / 1023))) return false;
            return true;
        };
        if (it == g_vertexRanges.end() || !stillValid(it->second))
        {
            // Growing an earlier copy: at least double it (within the buffer)
            // so a reach that creeps up draw by draw costs O(n), not O(n^2).
            if (it != g_vertexRanges.end() && it->second.words < words)
                words = std::min(full, std::max(words, it->second.words * 2));
            if (f.vertexData.size() + words > (100u << 20) / 4)
            {
                static int warned = 0;
                if (warned++ < 5)
                    Log("render: frame vertex data over 100 MB, dropping a draw (%u dwords from %08X)", words, addr);
                return;
            }
            uint32_t at = uint32_t(f.vertexData.size());
            f.vertexData.resize(at + words);
            CopySwap32(f.vertexData.data() + at, src, words, endian);
            Count(prof::CPC_VTX_COPY_BYTES, uint64_t(words) * 4);
            it = g_vertexRanges.insert_or_assign(key, VertexRange{ at, words }).first;
        }
        misc.vertexBase[k] = it->second.at;
    }

    // Rectangle corners go to the storage buffer for main_rect.
    if (rect)
    {
        uint32_t at = uint32_t(f.vertexData.size());
        f.vertexData.insert(f.vertexData.end(), idx.begin(), idx.end());
        memcpy(&misc.alphaRef[2], &at, 4);
    }

    // --- Constants ---------------------------------------------------------
    phase.Next(prof::CP_CONSTANTS);
    static_assert(sizeof(g_constMirror) == render::kConstBytes);
    bool firstOfFrame = g_lastConstantOffset == UINT32_MAX;
    if (firstOfFrame || !g_constMirrorTrusted)
    {
        uint32_t differ = 0;
        for (uint32_t i = 0; i < 2048; i++)
        {
            uint32_t v = Reg(ALU_CONSTANTS + i);
            if (v != g_constMirror[i]) { g_constMirror[i] = v; differ++; }
        }
        if (differ)
            g_constDirty = true;
        // (Early frames: the mirror starts empty and fills in.)
        if (differ && g_constMirrorTrusted && g_stats.frames > 120)
        {
            g_constMirrorTrusted = false;
            Log("render: %u shader constants changed outside the command stream; reading all constants for every draw from now on", differ);
        }
    }
    if (firstOfFrame || (g_constDirty && !EqualSwap32(g_constUploaded, g_constMirror, 2048, 0)))
    {
        g_lastConstantOffset = AppendUniform(g_constMirror, sizeof(g_constMirror));
        memcpy(g_constUploaded, g_constMirror, sizeof(g_constMirror));
        Count(prof::CPC_CONST_UPLOADS, 1);
    }
    g_constDirty = false;
    d.constOffset = g_lastConstantOffset;

    // --- Textures ----------------------------------------------------------
    phase.Next(prof::CP_TEXTURES);
    d.textureFirst = uint32_t(f.textures.size());
    std::string texInfo;
    uint32_t texMask = shaders.vs->textureMask | shaders.ps->textureMask;
    for (uint32_t slot = 0; slot < 32; slot++)
    {
        if (!((texMask >> slot) & 1))
            continue;
        uint32_t dim = ((shaders.ps->textureMask >> slot) & 1) ? shaders.ps->textureDimension[slot] : shaders.vs->textureDimension[slot];
        uint32_t fc[6];
        for (int i = 0; i < 6; i++) fc[i] = Reg(FETCH_CONSTANTS + slot * 6 + i);
        render::TextureBinding b{};
        b.slot = uint8_t(slot);
        b.dimension = uint8_t(dim);
        b.sampler = SamplerBits(fc);
        {
            // Fetch instructions may set the filters themselves (e.g. point
            // sampling of raw 7e3 light-buffer bits, which blend to garbage).
            const auto& fo = ((shaders.ps->textureMask >> slot) & 1) ? shaders.ps->filterOverride[slot] : shaders.vs->filterOverride[slot];
            for (int i = 0; i < 3; i++)
                if (fo[i] != 3)
                    b.sampler = uint16_t((b.sampler & ~(3u << (9 + i * 2))) | (uint32_t(fo[i]) << (9 + i * 2)));
        }
        XenosTexture t;
        // The fetch instruction's dimension must match the texture's: cube
        // with cube, 3D with 3D (stacked 2D textures, also fetched as 3D,
        // aren't handled), 1D/2D with 2D.
        if (ParseTextureFetch(fc, t) && (t.dimension == 3) == (dim == 3) && (t.dimension == 2) == (dim == 2))
        {
            uint32_t swizzle = (fc[3] >> 1) & 0xFFF;
            float rawMax[4];
            bool rawInt, signsApplied;
            b.textureId = TextureFor(t, swizzle, misc.textureSwizzle[slot], rawMax, rawInt, signsApplied);
            // Resolved textures are as large as the copy's destination pitch;
            // map the fetch's normalized coordinates onto its own size.
            if (dim <= 1)
            {
                auto rs = g_resolved.find(t.address);
                if (rs != g_resolved.end() && rs->second.id == b.textureId && rs->second.w && rs->second.h)
                {
                    misc.texcoordScale[slot][0] = float(t.width) / float(rs->second.w);
                    misc.texcoordScale[slot][1] = float(t.height) / float(rs->second.h);
                    // Raw 7e3 bit patterns (light buffers): blending neighbours'
                    // bits gives nonsense where their exponents differ (banded
                    // blotches on skin); sample the nearest texel.
                    if (rs->second.raw7e3)
                        b.sampler = uint16_t(b.sampler & ~(0x3Fu << 9)); // mag, min, mip: point
                }
            }
            if (DumpRequested())
                DumpTexture(t, swizzle);
            // Number format (bit 0: integer) and exponent adjust (bits 13-18, signed):
            // integer fetches return the stored integer; the result is scaled by 2^adjust.
            bool integerFetch = fc[3] & 1;
            float adjust = std::ldexp(1.0f, int32_t(fc[3] << 13) >> 26);
            for (int c = 0; c < 4; c++)
            {
                uint32_t sel = (swizzle >> (c * 3)) & 7;
                // Normalized storage: integer = value * max. Integer storage: fraction = value / max.
                float numberScale = integerFetch ? (rawInt ? 1.0f : rawMax[sel]) : (rawInt ? 1.0f / rawMax[sel] : 1.0f);
                misc.textureScale[slot][c] = sel < 4 ? adjust * numberScale : 1.0f;
            }
            // TEX_DUMP_PS=<pixel shader hash>:<slot>: save that binding's decoded texels once (debugging).
            static const char* dumpPs = getenv("TEX_DUMP_PS");
            static std::unordered_set<uint32_t> dumpedPs;
            if (dumpPs && dumpedPs.size() < 24 && strtoull(dumpPs, nullptr, 16) == shaders.psHash && strchr(dumpPs, ':') &&
                uint32_t(atoi(strchr(dumpPs, ':') + 1)) == slot && dumpedPs.insert(t.address).second)
            {
                std::vector<uint8_t> px;
                if (DecodeTexture(t, px))
                {
                    ApplySwizzle(t.format, swizzle, px);
                    char path[80];
                    snprintf(path, sizeof(path), "texps_%08X_%ux%u_f%u.rgba8", t.address, t.width, t.height, t.format);
                    if (FILE* fp = fopen(path, "wb")) { fwrite(px.data(), 1, px.size(), fp); fclose(fp); }
                    Log("TEX_DUMP_PS: %s (resolved id %u, pitch %u, tiled %d, endian %u, swizzle %03X)", path,
                        g_resolved.count(t.address) ? g_resolved[t.address].id : 0, t.pitch, int(t.tiled), t.endian, swizzle);
                }
            }
            // Texture sign of the component each output channel comes from,
            // converted in the shader (float formats have no sign modes).
            uint32_t signs = (fc[0] >> 2) & 0xFF, signMask = 0;
            // (27-29 are the 16-bit float _EXPAND formats: Crysis 2 reads its
            // luminance chain through them with sign mode "signed", which on
            // float data must be ignored, not applied as 8-bit signed.
            // Decoded normalized 16-bit textures have theirs applied already;
            // resolved ones keep the shader's conversion, as before.)
            bool floatFormat = (t.format >= 27 && t.format <= 32) || (t.format >= 36 && t.format <= 38) ||
                               t.format == 22 || t.format == 23 || signsApplied;
            for (int c = 0; c < 4 && !floatFormat; c++)
            {
                uint32_t sel = (swizzle >> (c * 3)) & 7;
                if (sel < 4)
                    signMask |= ((signs >> (sel * 2)) & 3) << (c * 2);
            }
            misc.textureSwizzle[slot] |= signMask << 12;
        }
        if (FrameLog())
        {
            char buf[96];
            snprintf(buf, sizeof(buf), " [s%u d%u f%u %08X id%u sg%02X sw%03X ea%d nf%u]", slot, dim, fc[1] & 0x3F, fc[1] & 0xFFFFF000, b.textureId,
                     (fc[0] >> 2) & 0xFF, (fc[3] >> 1) & 0xFFF, int32_t(fc[3] << 13) >> 26, fc[3] & 1);
            texInfo += buf;
            if (!b.textureId) // nothing bound: the raw fetch constant, to see why
            {
                char raw[80];
                snprintf(raw, sizeof(raw), "{fc %08X %08X %08X %08X %08X %08X}", fc[0], fc[1], fc[2], fc[3], fc[4], fc[5]);
                texInfo += raw;
            }
        }
        f.textures.push_back(b);
    }
    d.textureCount = uint32_t(f.textures.size()) - d.textureFirst;
    phase.Next(prof::CP_RECORD);
    d.miscOffset = AppendUniform(&misc, sizeof(misc));

    // --- Draw ----------------------------------------------------------------
    d.indexed = indexed && !rect;
    if (rect)
    {
        d.first = 0;
        d.count = uint32_t(idx.size() / 3 * 6);
    }
    else if (indexed)
    {
        d.first = uint32_t(f.indices.size());
        d.count = uint32_t(idx.size());
        f.indices.insert(f.indices.end(), idx.begin(), idx.end());
    }
    else
    {
        d.first = offset;
        d.count = count;
    }
    if (FrameLog())
    {
        char tex[256] = "";
        size_t n = 0;
        for (uint32_t i = 0; i < d.textureCount && n < sizeof(tex) - 24; i++)
        {
            const auto& b = f.textures[d.textureFirst + i];
            n += snprintf(tex + n, sizeof(tex) - n, " t%u=%u", b.slot, b.textureId);
        }
        Log("framelog %llu: TEX%s", (unsigned long long)g_stats.frames, texInfo.c_str());
        if (rect)
        {
            // Rectangle lists: the first dwords of vertex fetch constant 0's buffer.
            uint32_t vf0 = Reg(FETCH_CONSTANTS), vf1 = Reg(FETCH_CONSTANTS + 1);
            uint32_t vaddr = PhysToGuest(vf0 & ~3u);
            char vb[480];
            size_t vn = 0;
            for (uint32_t i = 0; i < 42 && vn < sizeof(vb) - 16; i++)
            {
                uint32_t v = GpuDword(vaddr + i * 4, vf1 & 3);
                float fv;
                memcpy(&fv, &v, 4);
                vn += snprintf(vb + vn, sizeof(vb) - vn, " %g", fv);
            }
            Log("framelog %llu: RECTVB %08X:%s", (unsigned long long)g_stats.frames, vaddr, vb);
            Log("framelog %llu: RECTREGS surf %08X color %08X depth %08X vte %08X vport %g %g %g %g %g %g mode %08X winofs %08X "
                "winsc %08X-%08X screensc %08X-%08X vtxcntl %08X",
                (unsigned long long)g_stats.frames, Reg(RB_SURFACE_INFO), Reg(RB_COLOR_INFO), Reg(RB_DEPTH_INFO), Reg(PA_CL_VTE_CNTL),
                RegF(PA_CL_VPORT_XSCALE), RegF(PA_CL_VPORT_XSCALE + 1), RegF(PA_CL_VPORT_XSCALE + 2), RegF(PA_CL_VPORT_XSCALE + 3),
                RegF(PA_CL_VPORT_XSCALE + 4), RegF(PA_CL_VPORT_XSCALE + 5), Reg(PA_SU_SC_MODE_CNTL), Reg(PA_SC_WINDOW_OFFSET),
                Reg(PA_SC_WINDOW_SCISSOR_TL), Reg(PA_SC_WINDOW_SCISSOR_BR), Reg(0x200E), Reg(0x200F), Reg(0x2302));
        }
        Log("framelog %llu: DRAW %zu prim %u n %u c0 %u/%u/%u c1 %u/%u/%u z %u/%u h %u sc %u,%u %ux%u ztest %d zw %d blend %08X mask %04X VS %016llX PS %016llX%s",
            (unsigned long long)g_stats.frames, f.draws.size(), prim, d.count, d.color[0].base, d.color[0].pitch, d.color[0].format,
            d.color[1].base, d.color[1].pitch, d.color[1].format, d.depth.base, d.depth.pitch, d.rtHeight,
            d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3], int(p.depthTest), int(p.depthWrite), p.blend[0], colorMask & 0xFFFF,
            (unsigned long long)p.vs, (unsigned long long)p.ps, tex);
        // FRAME_CONSTS=<pixel shader hash>: also log that shader's nonzero pixel constants.
        // (Or from the browser console: Module._crysis_log_consts(0x<low 32 bits>), which also logs c0-c255.)
        static const uint64_t constsPs = getenv("FRAME_CONSTS") ? strtoull(getenv("FRAME_CONSTS"), nullptr, 16) : 0;
        bool consoleMatch = g_constsPsLow && uint32_t(p.ps) == g_constsPsLow;
        if ((constsPs && constsPs == p.ps) || consoleMatch)
            for (uint32_t c = consoleMatch ? 0 : 256; c < 512; c++)
            {
                float v[4];
                for (int k = 0; k < 4; k++) v[k] = RegF(ALU_CONSTANTS + c * 4 + k);
                if (v[0] || v[1] || v[2] || v[3])
                    Log("framelog %llu: CONST c%u = %g %g %g %g", (unsigned long long)g_stats.frames, c, v[0], v[1], v[2], v[3]);
            }
        Log("framelog %llu: DS dc %08X srm %08X srmbf %08X r2203 %08X r2204 %08X modecntl %08X surf %08X polyofs %g %g %g %g",
            (unsigned long long)g_stats.frames, Reg(RB_DEPTHCONTROL), Reg(RB_STENCILREFMASK), Reg(RB_STENCILREFMASK_BF),
            Reg(0x2203), Reg(0x2204), Reg(PA_SU_SC_MODE_CNTL), Reg(RB_SURFACE_INFO),
            RegF(0x2380), RegF(0x2381), RegF(0x2382), RegF(0x2383));
    }
    if (uint32_t w = g_watchPs.load(std::memory_order_relaxed); w && w == uint32_t(p.ps))
    {
        uint32_t reg = g_watchReg.load(std::memory_order_relaxed);
        for (int k = 0; k < 4; k++)
            g_watchValue[k].store(RegF(ALU_CONSTANTS + reg * 4 + k), std::memory_order_relaxed);
        g_watchFrame.store(g_stats.frames, std::memory_order_relaxed);
    }
    for (auto& s : g_skipPs)
        if (uint32_t v = s.load(std::memory_order_relaxed); v && v == uint32_t(p.ps))
            return; // crysis_skip_ps: leave this draw out
    f.cmds.push_back({ render::Cmd::Draw, uint32_t(f.draws.size()) });
    f.draws.push_back(d);
    g_stats.recorded++;
    AliasedDepthClear(d, rect);
}

void RecordFrameEnd(uint32_t frontAddress, uint32_t width, uint32_t height)
{
    PhaseTimer phase(prof::CP_FRAME_END);
    render::Frame& f = Frame();
    auto r = g_resolved.find(frontAddress);
    if (r != g_resolved.end())
    {
        f.presentId = r->second.id;
        f.presentW = std::min(width ? width : r->second.w, r->second.w);
        f.presentH = std::min(height ? height : r->second.h, r->second.h);
        g_stats.presented++;
    }
    EvictTextures(f);
    if (FrameLog())
        f.dumpDir = getenv("FRAME_DUMP") ? getenv("FRAME_DUMP") : DumpRequested() ? "dump" : nullptr;
    if (FrameLog())
        Log("framelog %llu: END front %08X %ux%u present id %u (%ux%u); RB_SURFACE_INFO %08X RB_DEPTH_INFO %08X RB_DEPTHCONTROL %08X",
            (unsigned long long)g_stats.frames, frontAddress, width, height, f.presentId, f.presentW, f.presentH, Reg(RB_SURFACE_INFO),
            Reg(RB_DEPTH_INFO), Reg(RB_DEPTHCONTROL));
    g_stats.frames++;
    if (g_stats.frames <= 3 || g_stats.frames % 300 == 0)
        Log("render: frame %llu: %zu draws, %zu resolves, %zu uploads, %zu KB vertex data, front buffer %08X %ux%u %s; "
            "totals: %llu draws seen, %llu recorded, %llu resolves, %llu without shaders, %llu rect lists, %llu bad prims, %llu presented",
            (unsigned long long)f.number, f.draws.size(), f.resolves.size(), f.uploads.size(), f.vertexData.size() / 256,
            frontAddress, width, height, f.presentId ? "(resolved)" : "(not resolved)",
            (unsigned long long)g_stats.draws, (unsigned long long)g_stats.recorded, (unsigned long long)g_stats.resolves,
            (unsigned long long)g_stats.noShader, (unsigned long long)g_stats.rectLists, (unsigned long long)g_stats.badPrim,
            (unsigned long long)g_stats.presented);
    g_lastFrameSizes = { f.vertexData.size(), f.indices.size(), f.uniforms.size(), f.draws.size(), f.textures.size(), f.cmds.size() };
    uint64_t submitStart = prof::Enabled() ? PlatformCounterNs() : 0;
    render::SubmitFrame(std::move(g_frame));
    if (submitStart) prof::submitWaitNs += PlatformCounterNs() - submitStart;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Browser console: Module._crysis_dump_frame() logs every draw and resolve of a
// frame shortly ahead and uploads its resolve outputs (see display_web.cpp).
// Module._crysis_log_consts(0x1234ABCD): in dumped frames, log every constant
// (c0-c511) of draws whose pixel shader hash ends in those 32 bits (0: off).
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_log_consts(uint32_t psLow) { g_constsPsLow = psLow; }
// Module._crysis_watch_const(0x1234ABCD, 483): record constant c483 of draws
// with that pixel shader every frame; _crysis_watch_value(0..3) reads it,
// _crysis_watch_value(4) the frame it came from.
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_watch_const(uint32_t psLow, uint32_t reg)
{
    g_watchReg = reg & 511;
    g_watchPs = psLow;
}
extern "C" EMSCRIPTEN_KEEPALIVE float crysis_watch_value(int i)
{
    return i >= 0 && i < 4 ? g_watchValue[i].load() : float(g_watchFrame.load());
}
// Module._crysis_skip_ps(0x1234ABCD): stop drawing with the pixel shader whose
// hash ends in those 32 bits; the same call again draws it again; 0 clears all.
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_skip_ps(uint32_t psLow)
{
    if (!psLow)
    {
        for (auto& s : g_skipPs) s = 0;
        Log("render: drawing every pixel shader again");
        return;
    }
    for (auto& s : g_skipPs)
        if (s == psLow)
        {
            s = 0;
            Log("render: drawing pixel shader ..%08X again", psLow);
            return;
        }
    for (auto& s : g_skipPs)
        if (!s)
        {
            s = psLow;
            Log("render: skipping pixel shader ..%08X", psLow);
            return;
        }
    Log("render: can't skip more than %zu pixel shaders", std::size(g_skipPs));
}
extern "C" EMSCRIPTEN_KEEPALIVE void crysis_dump_frame()
{
    g_dumpFrame = int64_t(g_stats.frames) + 2;
    Log("render: dumping frame %lld", (long long)g_dumpFrame.load());
}
#endif
