// Recorded GPU frames: the command processor (gpu_draw.cpp) turns the guest's
// draws, resolves and swaps into a self-contained Frame; the WebGPU backend
// (render_wgpu.cpp) executes it. The two run on different threads (and, in the
// browser, the backend runs on the page's main thread), so a Frame owns copies
// of everything it needs: shader sources, constants, vertex and index data,
// and decoded textures.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace render
{
    // A vector that leaves new elements uninitialized on resize: the frame's
    // big buffers are always written right after they grow, and zero-filling
    // them first was a sixth of the command processor's time.
    template <class T> struct NoInitAllocator
    {
        using value_type = T;
        NoInitAllocator() = default;
        template <class U> NoInitAllocator(const NoInitAllocator<U>&) noexcept {}
        T* allocate(size_t n) { return std::allocator<T>().allocate(n); }
        void deallocate(T* p, size_t n) noexcept { std::allocator<T>().deallocate(p, n); }
        template <class U> void construct(U* p) noexcept { ::new ((void*)p) U; }
        template <class U, class... A> void construct(U* p, A&&... a) { ::new ((void*)p) U(static_cast<A&&>(a)...); }
        template <class U> bool operator==(const NoInitAllocator<U>&) const noexcept { return true; }
    };
    template <class T> using PodVector = std::vector<T, NoInitAllocator<T>>;

    constexpr uint32_t kConstBytes = 8192;   // 512 float4 constants (VS 0-255, PS 256-511)
    constexpr uint32_t kMiscBytes = 1504;     // xenos::MiscConstants as laid out in the WGSL
    constexpr uint32_t kUniformAlign = 256;  // dynamic uniform offset alignment
    constexpr uint8_t kNone = 0xFF;

    // Host render target formats.
    enum RtFormat : uint8_t { RT_RGBA8, RT_RGB10A2, RT_RGBA16F, RT_RG16F, RT_R32F, RT_RG32F, RT_DEPTH24S8 };

    enum Topology : uint8_t { TOP_POINTS, TOP_LINES, TOP_LINE_STRIP, TOP_TRIANGLES, TOP_TRIANGLE_STRIP };

    // Identifies an EDRAM surface; the backend keeps one texture per key.
    struct RtKey
    {
        uint32_t base = 0;   // EDRAM base, in tiles
        uint32_t pitch = 0;  // pixels
        uint8_t format = kNone;
        bool operator==(const RtKey& o) const { return base == o.base && pitch == o.pitch && format == o.format; }
        bool Valid() const { return format != kNone; }
    };

    // Everything that selects a WebGPU render pipeline. Plain bytes: hashed and
    // compared with memcmp, so construct zeroed.
    struct PipelineKey
    {
        uint64_t vs, ps;
        uint8_t topology;
        uint8_t stripIndexed;    // indexed strip (primitive restart index 0xFFFFFFFF)
        uint8_t cull;            // 0 none, 1 front, 2 back
        uint8_t frontCW;
        uint8_t colorFormat[4];  // RtFormat or kNone
        uint8_t writeMask[4];
        uint8_t depthFormat;     // RT_DEPTH24S8 or kNone
        uint8_t depthTest, depthWrite, depthFunc;    // Xenos compare function
        uint8_t stencilEnable, stencilReadMask, stencilWriteMask;
        uint8_t rectList;        // vertex entry point main_rect: 6 vertices per Xenos rectangle
        uint32_t stencilOps;     // front func/fail/zpass/zfail, back func/fail/zpass/zfail (3 bits each)
        uint32_t blend[4];       // raw RB_BLENDCONTROL
    };

    struct TextureBinding
    {
        uint8_t slot, dimension; // fetch constant slot, 0 1D / 1 2D / 2 3D / 3 cube
        uint16_t sampler;        // clamp x/y/z (3 bits each), mag/min/mip filter (2 bits each)
        uint32_t textureId;      // 0 = placeholder
    };

    struct DrawCmd
    {
        PipelineKey pipe;
        RtKey color[4];
        RtKey depth;
        uint32_t rtHeight;              // required render target height
        uint32_t scissor[4];            // x, y, w, h
        uint32_t constOffset, miscOffset; // into Frame::uniforms
        bool indexed;
        uint32_t first, count;          // index (or vertex) range
        uint32_t textureFirst, textureCount;
        float blendConstant[4];
        uint32_t stencilRef;
    };

    // EDRAM -> texture copy (with optional clears of the source surfaces afterwards).
    struct ResolveCmd
    {
        RtKey src;                      // color or depth surface
        uint32_t rtHeight;
        int32_t x, y, w, h;             // source rectangle
        uint32_t destId, destW, destH;  // destination texture (backend-side, sampled by later draws)
        bool destFloat;                 // RGBA16F destination (else RGBA8)
        uint8_t scaleX, scaleY;         // MSAA source: samples per pixel in x and y (averaged)
        uint8_t convert;                // 0 copy, 1 fixed point (integers: round(value * mul), clamped to maxv), 2 depth
        float mul[4];                   // convert 1: 2^exp_bias
        float maxv[4];                  // convert 1: channel integer maximum
        uint8_t clamp7e3;               // source drawn as 7e3 float: 1 clamp to its range, 2 copy its 7e3 bit patterns
        float chanMaxStore[4];          // (recorder only) destination channel integer maxima
        bool copy;                      // false: clear only
        RtKey clearColor;               // surfaces to clear afterwards (Valid() = clear it)
        float clearValue[4];
        RtKey clearDepth;
        float depthValue;
        uint32_t stencilValue;
        // Write the result back to guest memory too (small 32bpp resolves the
        // CPU may read, as real hardware leaves every resolve in memory):
        // tiled surface at wbAddr, wbPitch texels wide, with wbEndian swapping;
        // wbSwap exchanges red and blue; wbDepth packs depth as (d24 << 8).
        uint32_t destAddr = 0;          // guest address of the destination (dump names)
        uint32_t wbAddr = 0, wbPitch = 0;
        uint8_t wbEndian = 0, wbSwap = 0, wbDepth = 0;
        uint8_t wbHalf2 = 0;            // 16:16 float destination: red and green halves, X in the low 16 bits
    };

    struct Cmd { enum Type : uint8_t { Draw, Resolve } type; uint32_t index; };

    struct ShaderSource
    {
        uint64_t hash;
        bool pixel;
        std::string wgsl;
        uint32_t textureMask;
        uint8_t textureDimension[32];
    };

    struct TextureUpload
    {
        uint32_t id, width, height;
        uint32_t layers = 1;             // 6: cube map (faces +X -X +Y -Y +Z -Z)
        bool is3d = false;               // 3D texture of `depth` slices (one after another in rgba)
        uint32_t depth = 1;
        bool half = false;               // RGBA16F texels (8 bytes) instead of RGBA8
        std::vector<std::vector<uint8_t>> mips; // levels 1.. (each max(1, size >> level), all layers)
        std::vector<uint8_t> rgba;
    };

    struct Frame
    {
        uint64_t number = 0;
        std::vector<ShaderSource> shaders;   // first use of each shader
        std::vector<TextureUpload> uploads;
        PodVector<uint8_t> uniforms;         // constant + misc blocks, kUniformAlign-aligned
        PodVector<uint32_t> vertexData;      // endian-swapped vertex buffers (dwords)
        PodVector<uint32_t> indices;         // 32-bit indices
        std::vector<TextureBinding> textures;
        std::vector<DrawCmd> draws;
        std::vector<ResolveCmd> resolves;
        std::vector<Cmd> cmds;
        uint32_t presentId = 0, presentW = 0, presentH = 0; // texture to show (0 = keep previous)
        std::vector<uint32_t> releases;      // texture ids the recorder evicted: free them
        const char* dumpDir = nullptr;       // FRAME_DUMP: save every resolve output of this frame
    };

    // Hand a finished frame to the backend. Thread-safe; blocks while too many
    // frames are queued so the guest can't run arbitrarily far ahead.
    void SubmitFrame(std::unique_ptr<Frame> frame);
}
