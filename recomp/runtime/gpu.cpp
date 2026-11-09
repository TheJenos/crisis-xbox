// GPU front end: video imports, interrupts, and a PM4 command processor.
//
// This is a "null renderer": it consumes the command stream the title writes
// to the ring buffer, keeps GPU registers, fences and read pointers up to date,
// and delivers interrupts, but draws nothing. Draws and swaps are counted.
//
// Register block: the title accesses GPU registers as memory at 0x7FC80000 +
// index * 4 (big-endian, plain RAM in this runtime). The command processor
// reads and writes the same memory.
#include "guest.h"
#include "display.h"
#include "xenos_texture.h"
#include "xenos_shader.h"
#include "gpu_draw.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "platform.h"

namespace
{
    constexpr uint32_t REG_BASE = 0x7FC80000;
    uint32_t ReadReg(uint32_t index) { return Load<uint32_t>(REG_BASE + index * 4); }
    void WriteReg(uint32_t index, uint32_t value) { Store<uint32_t>(REG_BASE + index * 4, value); }

    // Register indices (Xenos / R500 naming)
    constexpr uint32_t CP_RB_RPTR = 0x01C4, CP_RB_WPTR = 0x01C5;
    constexpr uint32_t SCRATCH_REG0 = 0x0578, SCRATCH_UMSK = 0x01DC, SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t RB_EDRAM_TIMING = 0x0F00, RB_BC_CONTROL = 0x0F01;
    constexpr uint32_t D1MODE_V_COUNTER = 0x194C, D1MODE_INTERRUPT_STATUS = 0x1951, D1MODE_VIEWPORT_SIZE = 0x1961;

    // Physical address (as the GPU sees it) to guest address: physical memory is mapped at 0xA0000000.
    uint32_t PhysToGuest(uint32_t p) { return 0xA0000000 | (p & 0x1FFFFFFF); }

    // Register writes issued by the command stream. Writing SCRATCH_REGn also
    // writes the value to memory at SCRATCH_ADDR + n*4 when bit n of SCRATCH_UMSK
    // is set; the D3D runtime uses this to signal the CPU and itself.
    void CpWriteReg(uint32_t index, uint32_t value)
    {
        WriteReg(index, value);
        if (index - 0x4000u < 0x800u) // ALU constants (shader float constants)
            NoteConstantWrite(index - 0x4000u, value);
        if (index >= SCRATCH_REG0 && index < SCRATCH_REG0 + 8)
        {
            uint32_t n = index - SCRATCH_REG0;
            if ((ReadReg(SCRATCH_UMSK) >> n) & 1)
                Store<uint32_t>(PhysToGuest(ReadReg(SCRATCH_ADDR) + n * 4), value);
        }
    }

    // GPU memory accesses carry an endian mode in the low 2 bits of the address:
    // 0 none, 1 swap bytes in 16-bit words, 2 swap bytes in 32-bit words, 3 swap 16-bit halves.
    uint32_t GpuSwap(uint32_t v, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);
        case 2: return __builtin_bswap32(v);
        case 3: return (v << 16) | (v >> 16);
        default: return v;
        }
    }
    // The GPU is little-endian: store the swapped value's little-endian bytes.
    void GpuWriteMemory(uint32_t addrWithEndian, uint32_t value)
    {
        uint32_t v = GpuSwap(value, addrWithEndian & 3);
        memcpy(GuestPtr(PhysToGuest(addrWithEndian & ~3u)), &v, 4);
    }
    uint32_t GpuReadMemory(uint32_t addrWithEndian)
    {
        uint32_t v;
        memcpy(&v, GuestPtr(PhysToGuest(addrWithEndian & ~3u)), 4);
        return GpuSwap(v, addrWithEndian & 3);
    }

    // ---------------------------------------------------------------------------
    // Interrupt / DPC delivery: a queue serviced by a runtime-owned guest thread.
    // ---------------------------------------------------------------------------
    // cpu >= 0: run as if on that hardware thread (the PCR's current-CPU byte at
    // r13+0x10C), which interrupt handlers use to acknowledge their CPU's bit.
    struct GuestCallbackJob { uint32_t function; uint32_t args[4]; uint32_t argCount; int cpu = -1; };

    class CallbackQueue
    {
    public:
        void Push(const GuestCallbackJob& job)
        {
            { std::lock_guard lock(mutex_); jobs_.push_back(job); }
            cv_.notify_one();
        }
        void Run(PPCContext& ctx)
        {
            for (;;)
            {
                GuestCallbackJob job;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [&] { return !jobs_.empty(); });
                    job = jobs_.front();
                    jobs_.pop_front();
                }
                if (job.cpu >= 0)
                    Store<uint8_t>(ctx.r13.u32 + 0x10C, uint8_t(job.cpu));
                PPCRegister* regs[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6 };
                for (uint32_t i = 0; i < job.argCount; i++)
                    regs[i]->u64 = job.args[i];
                if (calls_++ < 3)
                    Log("GPU: calling %08X(%08X, %08X)", job.function, job.args[0], job.args[1]);
                GuestCall(ctx, job.function);
                if (calls_ <= 3)
                    Log("GPU: %08X returned", job.function);
            }
        }
    private:
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<GuestCallbackJob> jobs_;
        uint64_t calls_ = 0;
    };

    CallbackQueue g_interrupts;
    CallbackQueue g_dpcs;
    std::atomic<uint32_t> g_interruptCallback{ 0 }, g_interruptUserData{ 0 };

    // Deliver interrupt `source` (0 = vblank, 1 = command processor) to each
    // hardware thread in cpuMask, as the hardware does.
    void RaiseInterrupt(uint32_t source, uint32_t cpuMask)
    {
        uint32_t cb = g_interruptCallback;
        if (!cb)
            return;
        for (int cpu = 0; cpu < 6; cpu++)
            if (cpuMask & (1u << cpu))
            {
                GuestCallbackJob job{ cb, { source, g_interruptUserData.load() }, 2 };
                job.cpu = cpu;
                g_interrupts.Push(job);
            }
    }

    // ---------------------------------------------------------------------------
    // Command processor state
    // ---------------------------------------------------------------------------
    // Guards the ring state: VdInitializeRingBuffer can move the ring (and the
    // title frees the old one), so the processor must not be mid-buffer then.
    std::mutex g_ringMutex;
    std::atomic<uint32_t> g_ringBase{ 0 };       // guest address
    std::atomic<uint32_t> g_ringSizeDwords{ 0 };
    std::atomic<uint32_t> g_rptrWriteBack{ 0 };  // guest address, 0 = disabled
    uint32_t g_rptr = 0;                          // dword index into the ring

    struct Stats { uint64_t packets = 0, draws = 0, swaps = 0, interrupts = 0, indirect = 0, waits = 0; } g_stats;
    std::atomic<uint64_t> g_frames{ 0 };

    // Reads dwords from either the ring (wrapping) or a linear indirect buffer.
    struct PacketReader
    {
        uint32_t base, sizeDwords, index;
        bool wrap;
        uint32_t consumed = 0; // dwords read so far
        uint32_t Next()
        {
            uint32_t v = Load<uint32_t>(base + index * 4);
            index = wrap ? (index + 1) % sizeDwords : index + 1;
            consumed++;
            return v;
        }
    };

    bool CompareFunc(uint32_t func, uint32_t value, uint32_t ref)
    {
        switch (func & 7)
        {
        case 0: return false;
        case 1: return value < ref;
        case 2: return value <= ref;
        case 3: return value == ref;
        case 4: return value != ref;
        case 5: return value >= ref;
        case 6: return value > ref;
        default: return true;
        }
    }

    void ExecuteBuffer(PacketReader& r, uint32_t endIndex, int depth);

    // Type 3 opcodes
    enum : uint32_t
    {
        PM4_ME_INIT = 0x48, PM4_NOP = 0x10, PM4_INDIRECT_BUFFER = 0x3F, PM4_INDIRECT_BUFFER_PFD = 0x37,
        PM4_WAIT_FOR_IDLE = 0x26, PM4_WAIT_REG_MEM = 0x3C, PM4_REG_RMW = 0x21, PM4_COND_WRITE = 0x45,
        PM4_EVENT_WRITE = 0x46, PM4_EVENT_WRITE_SHD = 0x58, PM4_EVENT_WRITE_EXT = 0x59, PM4_EVENT_WRITE_ZPD = 0x5B,
        PM4_MEM_WRITE = 0x3D, PM4_INTERRUPT = 0x54, PM4_DRAW_INDX = 0x22, PM4_DRAW_INDX_2 = 0x36,
        PM4_XE_SWAP = 0x64,
        PM4_SET_CONSTANT = 0x2D, PM4_SET_CONSTANT2 = 0x55, PM4_LOAD_ALU_CONSTANT = 0x2F,
        PM4_SET_SHADER_CONSTANTS = 0x56, PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B,
    };

    // CP_TRACE=1 logs every type 3 packet (first 2000) with its leading payload.
    bool g_trace = getenv("CP_TRACE") != nullptr;
    std::atomic<int> g_traced{ 0 };

    // VdSwap marks the end of a frame in the command stream with a NOP whose
    // first payload dword is this tag.
    constexpr uint32_t SWAP_TAG = 0x53574150; // 'SWAP'

    // Constant register banks addressed by SET_CONSTANT / LOAD_ALU_CONSTANT.
    uint32_t ConstantBase(uint32_t type)
    {
        switch (type)
        {
        case 0: return 0x4000; // ALU (float) constants
        case 1: return 0x4800; // fetch constants (texture / vertex)
        case 2: return 0x4900; // bool constants
        case 3: return 0x4908; // loop constants
        default: return 0x2000; // registers
        }
    }

    // Shader microcode loaded into the sequencer, keyed by a content hash, with
    // its WGSL translation (xenos_wgsl.cpp).
    struct LoadedShader
    {
        uint32_t type;
        std::vector<uint32_t> words;
        xenos::Translation translation;
    };
    std::unordered_map<uint64_t, LoadedShader> g_shaders;
    uint64_t g_currentShader[2] = {}; // vertex, pixel
    size_t g_shadersTranslated = 0, g_shadersFailed = 0;

    void LoadShader(uint32_t type, std::vector<uint32_t> words)
    {
        uint64_t h = 1469598103934665603ull ^ type;
        for (uint32_t w : words) { h ^= w; h *= 1099511628211ull; }
        g_currentShader[type & 1] = h;
        if (!g_shaders.count(h))
        {
            // SHADER_DUMP=<dir>: save each distinct shader's microcode (host-endian
            // dwords) as vs_<hash>.bin / ps_<hash>.bin for offline work.
            if (const char* dir = getenv("SHADER_DUMP"))
            {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s_%016llx.bin", dir, (type & 1) ? "ps" : "vs", (unsigned long long)h);
                if (FILE* f = fopen(path, "wb"))
                {
                    fwrite(words.data(), 4, words.size(), f);
                    fclose(f);
                }
            }
            size_t n = words.size();
            bool pixel = type & 1;
            xenos::DecodedShader decoded = xenos::Decode(words.data(), words.size(), pixel);
            // SHADER_PROBE=<pixel shader hash>:<instruction>:<register>[:<scale>] (debugging).
            static const char* probe = getenv("SHADER_PROBE");
            xenos::g_probe.active = false;
            if (probe && pixel && strtoull(probe, nullptr, 16) == h)
            {
                unsigned instr = 0, reg = 0;
                float scale = 1.0f;
                char swz[8] = "xyzw";
                if (sscanf(strchr(probe, ':') ? strchr(probe, ':') + 1 : "", "%u:%u:%f:%4[xyzw]", &instr, &reg, &scale, swz) >= 2)
                {
                    xenos::g_probe = { true, instr, reg, scale };
                    memcpy(xenos::g_probe.swizzle, swz, sizeof(swz));
                }
            }
            xenos::Translation tr = xenos::TranslateToWgsl(decoded);
            xenos::g_probe.active = false;
            if (tr.error.empty())
            {
                g_shadersTranslated++;
                // SHADER_WGSL=<dir>: save each translation as <vs|ps>_<hash>.wgsl.
                if (const char* dir = getenv("SHADER_WGSL"))
                {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/%s_%016llx.wgsl", dir, pixel ? "ps" : "vs", (unsigned long long)h);
                    if (FILE* f = fopen(path, "wb"))
                    {
                        fwrite(tr.wgsl.data(), 1, tr.wgsl.size(), f);
                        fclose(f);
                    }
                }
            }
            else
            {
                g_shadersFailed++;
                Log("GPU: %s shader %016llx not translated: %s", pixel ? "pixel" : "vertex", (unsigned long long)h, tr.error.c_str());
            }
            g_shaders[h] = { type, std::move(words), std::move(tr) };
            if (g_shaders.size() <= 5 || g_shaders.size() % 100 == 0)
                Log("GPU: %zu distinct shaders loaded (latest: %s, %zu dwords); %zu translated to WGSL, %zu not",
                    g_shaders.size(), pixel ? "pixel" : "vertex", n, g_shadersTranslated, g_shadersFailed);
        }
    }

    // Textures referenced by the current frame's draws (debug view). All fetch
    // constant slots are scanned at each draw; slots the shader doesn't use may
    // hold stale textures, which is fine for picking what to show.
    std::vector<XenosTexture> g_frameTextures;
    uint64_t g_gpuFrames = 0;
    // TEXTURE_VIEW=1: show the largest texture each frame uses instead of the
    // rendered front buffer (the old debug view).
    const bool g_textureView = getenv("TEXTURE_VIEW") != nullptr;

    void CollectDrawTextures()
    {
        // FETCH_DUMP=1: log the non-empty fetch constant slots at the first draws.
        static const bool dump = getenv("FETCH_DUMP") != nullptr;
        static int dumped = 0;
        if (dump && g_stats.draws > uint64_t(atoi(getenv("FETCH_DUMP"))) && dumped < 6)
        {
            dumped++;
            for (uint32_t slot = 0; slot < 32; slot++)
            {
                uint32_t f[6], any = 0;
                for (int i = 0; i < 6; i++) any |= f[i] = ReadReg(0x4800 + slot * 6 + i);
                if (any)
                    Log("GPU: draw %llu fetch slot %2u: %08X %08X %08X %08X %08X %08X", (unsigned long long)g_stats.draws,
                        slot, f[0], f[1], f[2], f[3], f[4], f[5]);
            }
        }
        for (uint32_t slot = 0; slot < 32; slot++)
        {
            uint32_t f[6];
            for (int i = 0; i < 6; i++)
                f[i] = ReadReg(0x4800 + slot * 6 + i);
            XenosTexture t;
            if (!ParseTextureFetch(f, t))
                continue;
            if (std::find(g_frameTextures.begin(), g_frameTextures.end(), t) == g_frameTextures.end())
                g_frameTextures.push_back(t);
        }
    }

    // End of a frame on the GPU timeline: show the largest decodable texture the
    // frame drew with. Real rendering will replace this debug view.
    void OnGpuFrameEnd()
    {
        static const bool wanted = !getenv("NO_WINDOW") || getenv("DUMP_FRAMES");
        g_gpuFrames++;
        const XenosTexture* best = nullptr;
        for (const auto& t : g_frameTextures)
            if (CanDecodeTexture(t.format) && (!best || uint64_t(t.width) * t.height > uint64_t(best->width) * best->height))
                best = &t;

        if (!best && !g_frameTextures.empty())
        {
            static int reported = 0;
            if (reported++ < 5)
            {
                std::string list;
                for (const auto& t : g_frameTextures)
                {
                    char b[64];
                    snprintf(b, sizeof(b), " %ux%u fmt%u%s", t.width, t.height, t.format, t.tiled ? "t" : "");
                    list += b;
                }
                Log("GPU: frame %llu: no decodable texture among %zu:%s", (unsigned long long)g_gpuFrames, g_frameTextures.size(), list.c_str());
            }
        }
        static XenosTexture lastShown;
        if (best && !(*best == lastShown))
        {
            static int changes = 0;
            if (changes++ < 40)
                Log("GPU: frame %llu shows texture %ux%u %s at %08X (%s, pitch %u, endian %u); %zu textures this frame",
                    (unsigned long long)g_gpuFrames, best->width, best->height, TextureFormatName(best->format),
                    best->address, best->tiled ? "tiled" : "linear", best->pitch, best->endian, g_frameTextures.size());
            lastShown = *best;
        }
        if (best && wanted)
        {
            static std::vector<uint8_t> rgba;
            if (DecodeTexture(*best, rgba))
            {
                char title[160];
                snprintf(title, sizeof(title), "Crysis 2 (recompiled) - frame %llu - texture %ux%u %s (debug view)",
                         (unsigned long long)g_gpuFrames, best->width, best->height, TextureFormatName(best->format));
                DisplaySubmitFrame(rgba.data(), best->width, best->height, title);
            }
        }
        g_frameTextures.clear();
    }

    // Packets whose whole payload is needed. Returns false for other opcodes.
    bool ExecuteFullPayload(PacketReader& r, uint32_t opcode, uint32_t count)
    {
        switch (opcode)
        {
        case PM4_SET_CONSTANT: case PM4_SET_CONSTANT2: case PM4_LOAD_ALU_CONSTANT:
        case PM4_SET_SHADER_CONSTANTS: case PM4_IM_LOAD: case PM4_IM_LOAD_IMMEDIATE: case PM4_NOP:
        case PM4_DRAW_INDX: case PM4_DRAW_INDX_2:
            break;
        default:
            return false;
        }
        // (Reused: one per packet was an allocation per draw. Only the command
        // processor thread gets here, and these opcodes don't nest.)
        static std::vector<uint32_t> p;
        p.resize(count);
        for (uint32_t i = 0; i < count; i++) p[i] = r.Next();

        switch (opcode)
        {
        case PM4_SET_CONSTANT: // p[0] = type << 16 | index, then values
        {
            uint32_t base = ConstantBase((p[0] >> 16) & 0xFF) + (p[0] & 0x7FF);
            for (uint32_t i = 1; i < count; i++) CpWriteReg(base + i - 1, p[i]);
            break;
        }
        case PM4_SET_CONSTANT2: // p[0] = absolute register index, then values
        case PM4_SET_SHADER_CONSTANTS:
            for (uint32_t i = 1; i < count; i++) CpWriteReg((p[0] & 0xFFFF) + i - 1, p[i]);
            break;
        case PM4_LOAD_ALU_CONSTANT: // address, type << 16 | index, size: values from memory
        {
            if (count < 3) break;
            uint32_t addr = PhysToGuest(p[0] & 0x3FFFFFFF);
            uint32_t base = ConstantBase((p[1] >> 16) & 0xFF) + (p[1] & 0x7FF);
            uint32_t n = p[2] & 0xFFF;
            for (uint32_t i = 0; i < n; i++) CpWriteReg(base + i, Load<uint32_t>(addr + i * 4));
            break;
        }
        case PM4_IM_LOAD: // address | shader type, start << 16 | size in dwords
        {
            if (count < 2) break;
            uint32_t addr = PhysToGuest(p[0] & ~3u), n = p[1] & 0xFFFF;
            std::vector<uint32_t> words(n);
            for (uint32_t i = 0; i < n; i++) words[i] = Load<uint32_t>(addr + i * 4);
            LoadShader(p[0] & 3, std::move(words));
            break;
        }
        case PM4_IM_LOAD_IMMEDIATE: // shader type, start << 16 | size, then the microcode
        {
            if (count < 2) break;
            uint32_t n = std::min<uint32_t>(p[1] & 0xFFFF, count - 2);
            LoadShader(p[0] & 3, std::vector<uint32_t>(p.begin() + 2, p.begin() + 2 + n));
            break;
        }
        case PM4_NOP:
            if (count >= 4 && p[0] == SWAP_TAG)
            {
                if (g_textureView)
                    OnGpuFrameEnd();
                else
                    g_gpuFrames++;
                if (RenderingEnabled())
                    RecordFrameEnd(p[1], p[2], p[3]);
            }
            break;
        case PM4_DRAW_INDX:   // viz query, draw initiator, [index base, index size]
        case PM4_DRAW_INDX_2: // draw initiator, immediate indices
        {
            g_stats.draws++;
            if (g_textureView)
                CollectDrawTextures();
            if (!RenderingEnabled())
                break;
            bool two = opcode == PM4_DRAW_INDX_2;
            uint32_t initiator = two ? (count > 0 ? p[0] : 0) : (count > 1 ? p[1] : 0);
            uint32_t indexBase = !two && count > 2 ? p[2] : 0, indexSize = !two && count > 3 ? p[3] : 0;
            DrawShaders shaders;
            for (int stage = 0; stage < 2; stage++)
            {
                auto it = g_shaders.find(g_currentShader[stage]);
                const xenos::Translation* t = (it != g_shaders.end() && it->second.translation.error.empty()) ? &it->second.translation : nullptr;
                (stage ? shaders.psHash : shaders.vsHash) = g_currentShader[stage];
                (stage ? shaders.ps : shaders.vs) = t;
            }
            RecordDraw(initiator, indexBase, indexSize, two ? p.data() + 1 : nullptr, two && count > 1 ? count - 1 : 0, shaders);
            break;
        }
        }
        return true;
    }

    void ExecuteType3(PacketReader& r, uint32_t opcode, uint32_t count, int depth)
    {
        if (g_trace && g_traced++ < 2000)
        {
            uint32_t peek[4] = {};
            for (uint32_t i = 0; i < std::min<uint32_t>(count, 4); i++)
                peek[i] = Load<uint32_t>(r.base + ((r.wrap ? (r.index + i) % r.sizeDwords : r.index + i) * 4));
            Log("CP%*s op %02X x%u: %08X %08X %08X %08X", depth * 2, "", opcode, count, peek[0], peek[1], peek[2], peek[3]);
        }
        if (ExecuteFullPayload(r, opcode, count))
            return;
        uint32_t p[16] = {};
        uint32_t take = std::min<uint32_t>(count, 16);
        // Opcodes with large payloads (constants, shaders) are skipped without buffering.
        bool needPayload = opcode == PM4_INDIRECT_BUFFER || opcode == PM4_INDIRECT_BUFFER_PFD ||
                           opcode == PM4_WAIT_REG_MEM || opcode == PM4_REG_RMW || opcode == PM4_COND_WRITE ||
                           opcode == PM4_EVENT_WRITE_SHD || opcode == PM4_EVENT_WRITE_EXT ||
                           opcode == PM4_MEM_WRITE || opcode == PM4_INTERRUPT;
        if (needPayload)
            for (uint32_t i = 0; i < take; i++) p[i] = r.Next();
        uint32_t remaining = needPayload ? count - take : count;

        switch (opcode)
        {
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            g_stats.indirect++;
            uint32_t addr = PhysToGuest(p[0]), len = p[1] & 0xFFFFF;
            if (depth < 4)
            {
                PacketReader ib{ addr, len, 0, false };
                ExecuteBuffer(ib, len, depth + 1);
            }
            break;
        }
        case PM4_WAIT_REG_MEM:
        {
            // wait_info: bits 0..2 compare function, bit 4 = memory (else register)
            g_stats.waits++;
            uint32_t info = p[0], addr = p[1], ref = p[2], mask = p[3];
            auto start = std::chrono::steady_clock::now();
            bool warned = false;
            for (bool first = true;; first = false)
            {
                uint32_t v = (info & 0x10) ? GpuReadMemory(addr) : ReadReg(addr & 0xFFFF);
                if (CompareFunc(info, v & mask, ref))
                {
                    if (!first && prof::Enabled())
                        prof::cpWaitNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
                    break;
                }
                if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
                {
                    Log("GPU: WAIT_REG_MEM on %s %08X (func %u, ref %08X, mask %08X) stuck, value %08X",
                        (info & 0x10) ? "memory" : "register", addr, info & 7, ref, mask, v);
                    warned = true;
                }
                PlatformYield();
            }
            break;
        }
        case PM4_REG_RMW:
        {
            uint32_t info = p[0], reg = info & 0x1FFF;
            uint32_t andMask = (info >> 30) & 1 ? ReadReg(p[1] & 0x1FFF) : p[1];
            uint32_t orMask = (info >> 31) & 1 ? ReadReg(p[2] & 0x1FFF) : p[2];
            CpWriteReg(reg, (ReadReg(reg) & andMask) | orMask);
            break;
        }
        case PM4_COND_WRITE:
        {
            // wait_info: function in bits 0..2, bit 4 = poll memory, bit 8 = write to memory
            uint32_t info = p[0], pollAddr = p[1], ref = p[2], mask = p[3], writeAddr = p[4], data = p[5];
            uint32_t v = (info & 0x10) ? GpuReadMemory(pollAddr) : ReadReg(pollAddr & 0xFFFF);
            if (CompareFunc(info, v & mask, ref))
            {
                if (info & 0x100) GpuWriteMemory(writeAddr, data);
                else CpWriteReg(writeAddr & 0xFFFF, data);
            }
            break;
        }
        case PM4_EVENT_WRITE_SHD:
        {
            // Fence: write a value (or the swap counter) to memory once prior work is done.
            uint32_t initiator = p[0], addr = p[1], value = p[2];
            GpuWriteMemory(addr, (initiator >> 31) & 1 ? uint32_t(g_stats.swaps) : value);
            break;
        }
        case PM4_EVENT_WRITE_EXT:
        {
            // Screen extents query: report "everything" (min 0, max 0x3FFF on each axis).
            uint32_t addr = p[1];
            uint16_t extents[6] = { 0, 0x3FFF, 0, 0x3FFF, 0, 1 };
            for (int i = 0; i < 6; i++)
                Store<uint16_t>(PhysToGuest(addr & ~3u) + i * 2, extents[i]);
            break;
        }
        case PM4_MEM_WRITE:
        {
            uint32_t addr = p[0];
            for (uint32_t i = 1; i < take; i++)
                GpuWriteMemory(addr + (i - 1) * 4, p[i]);
            break;
        }
        case PM4_INTERRUPT: // payload: CPU mask
            g_stats.interrupts++;
            RaiseInterrupt(1, p[0]);
            break;
        default:
            break;
        }
        for (uint32_t i = 0; i < remaining; i++)
            r.Next();
    }

    // Execute packets until the reader reaches endIndex. Progress is counted in
    // dwords so a packet whose payload runs past the end stops the buffer
    // instead of reading on through memory.
    void ExecuteBuffer(PacketReader& r, uint32_t endIndex, int depth)
    {
        uint32_t total = r.wrap ? (endIndex + r.sizeDwords - r.index) % r.sizeDwords : endIndex - r.index;
        r.consumed = 0;
        while (r.consumed < total)
        {
            uint32_t header = r.Next();
            g_stats.packets++;
            switch (header >> 30)
            {
            case 0: // write `count` registers starting at `base` (or one register repeatedly)
            {
                uint32_t count = ((header >> 16) & 0x3FFF) + 1, base = header & 0x7FFF;
                bool one = (header >> 15) & 1;
                for (uint32_t i = 0; i < count; i++)
                    CpWriteReg(one ? base : base + i, r.Next());
                break;
            }
            case 1: // two registers
            {
                uint32_t r1 = header & 0x7FF, r2 = (header >> 11) & 0x7FF;
                CpWriteReg(r1, r.Next());
                CpWriteReg(r2, r.Next());
                break;
            }
            case 2: // no-op
                break;
            case 3:
                ExecuteType3(r, (header >> 8) & 0x7F, ((header >> 16) & 0x3FFF) + 1, depth);
                break;
            }
        }
        if (r.consumed > total)
        {
            static std::atomic<int> warned{ 0 };
            if (warned++ < 10)
            {
                Log("GPU: packet ran %u dwords past the end of a %s (base %08X, %u dwords)",
                    r.consumed - total, r.wrap ? "ring buffer" : "indirect buffer", r.base, total);
                if (!r.wrap)
                {
                    std::string words;
                    for (uint32_t i = 0; i < std::min<uint32_t>(total + 1, 24); i++)
                    {
                        char b[12];
                        snprintf(b, sizeof(b), " %08X", Load<uint32_t>(r.base + i * 4));
                        words += b;
                    }
                    Log("GPU:   contents:%s", words.c_str());
                }
            }
        }
    }

    void CommandProcessorThread(PPCContext&)
    {
        uint32_t idle = 0;
        for (;;)
        {
            {
                std::lock_guard lock(g_ringMutex);
                uint32_t ring = g_ringBase, size = g_ringSizeDwords;
                uint32_t wptr = size ? ReadReg(CP_RB_WPTR) % size : 0;
                if (ring && size && wptr != g_rptr)
                {
                    PacketReader r{ ring, size, g_rptr, true };
                    uint32_t before = g_rptr;
                    uint64_t t0 = prof::Enabled() ? PlatformCounterNs() : 0;
                    ExecuteBuffer(r, wptr, 0);
                    if (t0) prof::cpBusyNs += PlatformCounterNs() - t0;
                    static uint64_t batches = 0;
                    if (++batches <= 3 || batches % 1000 == 0)
                        Log("GPU: batch %llu: ring dwords %u..%u, totals: %llu packets, %llu indirect buffers, %llu draws, %llu interrupts, %llu waits",
                            (unsigned long long)batches, before, wptr, (unsigned long long)g_stats.packets, (unsigned long long)g_stats.indirect,
                            (unsigned long long)g_stats.draws, (unsigned long long)g_stats.interrupts, (unsigned long long)g_stats.waits);
                    g_rptr = wptr;
                    WriteReg(CP_RB_RPTR, g_rptr);
                    if (uint32_t wb = g_rptrWriteBack)
                        Store<uint32_t>(wb, g_rptr);
                    idle = 0;
                    continue;
                }
            }
            // Spin briefly for low latency, then back off.
            if (++idle < 1000) PlatformYield();
            else PlatformSleepMs(1);
        }
    }

    // Vblank (60 Hz like the console): advance the scanline counter and raise
    // interrupt source 0. The title presents every second vblank (30 fps) and
    // times its simulation with the CPU clock, so VBLANK_HZ=120 lets it run at
    // up to 60 fps without speeding anything up.
    void VblankThread()
    {
        static const double hz = [] {
            const char* v = getenv("VBLANK_HZ");
            return v ? std::clamp(atof(v), 30.0, 240.0) : 60.0;
        }();
        const auto period = std::chrono::nanoseconds(int64_t(1e9 / hz));
        const uint64_t logEvery = uint64_t(hz * 10); // every 10 s
        Log("GPU: vblank at %.0f Hz", hz);
        auto next = std::chrono::steady_clock::now();
        uint64_t n = 0;
        for (;;)
        {
            next += period;
            std::this_thread::sleep_until(next);
            WriteReg(D1MODE_V_COUNTER, 720);
            RaiseInterrupt(0, 1u << 2); // vblank goes to hardware thread 2
            if (++n % logEvery == 0)
                Log("GPU: %llu vblanks, %llu swaps, %llu draws, %llu packets, %llu indirect buffers, %llu interrupts",
                    (unsigned long long)n, (unsigned long long)g_stats.swaps, (unsigned long long)g_stats.draws,
                    (unsigned long long)g_stats.packets, (unsigned long long)g_stats.indirect, (unsigned long long)g_stats.interrupts);
        }
    }

    std::once_flag g_started;
    void StartGpu()
    {
        std::call_once(g_started, [] {
            // Registers the title reads back expecting hardware values.
            WriteReg(RB_EDRAM_TIMING, 0x08100748);
            WriteReg(RB_BC_CONTROL, 0x0000200E);
            WriteReg(D1MODE_INTERRUPT_STATUS, 1);
            WriteReg(D1MODE_VIEWPORT_SIZE, (1280u << 16) | 720u);
            DisplayStart();
            StartNativeGuestThread("GPU command processor", CommandProcessorThread);
            StartNativeGuestThread("GPU interrupts", [](PPCContext& ctx) { g_interrupts.Run(ctx); });
            StartNativeGuestThread("DPCs", [](PPCContext& ctx) { g_dpcs.Run(ctx); });
            std::thread(VblankThread).detach();
        });
    }

    void FillVideoMode(uint32_t m)
    {
        memset(GuestPtr(m), 0, 48);
        Store<uint32_t>(m + 0, 1280);  // DisplayWidth
        Store<uint32_t>(m + 4, 720);   // DisplayHeight
        Store<uint32_t>(m + 8, 0);     // IsInterlaced
        Store<uint32_t>(m + 12, 1);    // IsWidescreen
        Store<uint32_t>(m + 16, 1);    // IsHiDef
        Store<float>(m + 20, 60.0f);   // RefreshRate
        Store<uint32_t>(m + 24, 1);    // VideoStandard: NTSC
        Store<uint32_t>(m + 28, 0x4A); // pixel clock in MHz (74.25 for 720p); titles divide the GPU clock by it
        Store<uint32_t>(m + 32, 1);
    }
}

// ---------------------------------------------------------------------------
// Video imports
// ---------------------------------------------------------------------------
GUEST_IMPORT(VdInitializeEngines) { StartGpu(); ctx.r3.u64 = 1; }
GUEST_IMPORT(VdShutdownEngines) {}

// void VdSetGraphicsInterruptCallback(u32 callback, u32 userData)
GUEST_IMPORT(VdSetGraphicsInterruptCallback)
{
    g_interruptCallback = ctx.r3.u32;
    g_interruptUserData = ctx.r4.u32;
    Log("GPU: interrupt callback %08X (user data %08X)", ctx.r3.u32, ctx.r4.u32);
}

// void VdInitializeRingBuffer(u32 physicalAddress, u32 sizeLog2): size is 2^(log2 + 3) bytes
GUEST_IMPORT(VdInitializeRingBuffer)
{
    uint32_t bytes = 1u << (ctx.r4.u32 + 3);
    {
        std::lock_guard lock(g_ringMutex);
        g_rptr = 0;
        g_ringSizeDwords = bytes / 4;
        g_ringBase = PhysToGuest(ctx.r3.u32);
        WriteReg(CP_RB_RPTR, 0);
        WriteReg(CP_RB_WPTR, 0);
    }
    Log("GPU: ring buffer at %08X, %X bytes", g_ringBase.load(), bytes);
    StartGpu();
}

// void VdEnableRingBufferRPtrWriteBack(u32 physicalAddress, u32 blockSizeLog2)
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    std::lock_guard lock(g_ringMutex);
    g_rptrWriteBack = PhysToGuest(ctx.r3.u32);
    Log("GPU: read pointer write-back at %08X", g_rptrWriteBack.load());
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}

// void VdGetSystemCommandBuffer(u32* p0, u32* p1)
GUEST_IMPORT(VdGetSystemCommandBuffer)
{
    memset(GuestPtr(ctx.r3.u32), 0, 0x94);
    Store<uint32_t>(ctx.r3.u32, 0xBEEF0000);
    Store<uint32_t>(ctx.r4.u32, 0xBEEF0001);
}

GUEST_IMPORT(VdQueryVideoMode) { FillVideoMode(ctx.r3.u32); }
GUEST_IMPORT(VdQueryVideoFlags) { ctx.r3.u64 = 0x7; } // widescreen, 1024+ wide, 720+ high

// void VdGetCurrentDisplayInformation(X_DISPLAY_INFO* info): front buffer and display sizes
GUEST_IMPORT(VdGetCurrentDisplayInformation)
{
    uint32_t info = ctx.r3.u32;
    memset(GuestPtr(info), 0, 0x58);
    Store<uint16_t>(info + 0x00, 1280); // front buffer width
    Store<uint16_t>(info + 0x02, 720);  // front buffer height
    Store<uint16_t>(info + 0x40, 1280); // display width
    Store<uint16_t>(info + 0x42, 720);  // display height
    Store<uint32_t>(info + 0x44, 60);   // refresh rate
}

// void VdGetCurrentDisplayGamma(u32* type, float* power)
GUEST_IMPORT(VdGetCurrentDisplayGamma)
{
    Store<uint32_t>(ctx.r3.u32, 2);
    Store<float>(ctx.r4.u32, 2.22222233f);
}

GUEST_IMPORT(VdSetDisplayMode) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdPersistDisplay) { if (ctx.r4.u32) Store<uint32_t>(ctx.r4.u32, 1); ctx.r3.u64 = 1; }
GUEST_IMPORT(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
GUEST_IMPORT(VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }

// u32 VdInitializeScalerCommandBuffer(u32 sourceXY, u32 sourceWH, u32 scaleXY, u32 scaleWH,
//                                     u32, u32, u32*, u32, u32* dest)
// Fills the scaler command buffer (9th argument, on the stack) with no-op packets
// and returns its length in dwords.
GUEST_IMPORT(VdInitializeScalerCommandBuffer)
{
    uint32_t dest = StackArg32(ctx, 8);
    const uint32_t dwords = 0x1CC / 4;
    if (dest >= 0x10000)
        for (uint32_t i = 0; i < dwords; i++)
            Store<uint32_t>(dest + i * 4, 0x80000000); // type 2 no-op packets
    else
        Log("GPU: VdInitializeScalerCommandBuffer: unexpected destination %08X", dest);
    ctx.r3.u64 = dwords;
}

// void VdSwap(u32* bufferPtr, u32* frontBufferFetchConstant, ...): the title reserves
// 64 dwords in its command buffer for the swap; fill them with one no-op packet
// carrying the front buffer (address, width, height) for the command processor.
// Optional frame limiter: FPS_LIMIT=<n> caps swaps at n a second. Off by
// default: the title paces itself against the emulated vblank, and a separate
// timer beats against that (a 30 cap gave 17 fps on the title screen).
static void LimitFrameRate()
{
    static const double limit = [] { const char* v = getenv("FPS_LIMIT"); return v ? atof(v) : 0.0; }();
    if (limit <= 0)
        return;
    static uint64_t next = 0;
    const uint64_t period = uint64_t(1e9 / limit);
    uint64_t now = PlatformCounterNs();
    if (now < next)
    {
        uint64_t wait = next - now;
        if (wait > 2000000)
            PlatformSleepMsPrecise(uint32_t(wait / 1000000) - 1); // the last part is spun below
        while (PlatformCounterNs() < next)
            PlatformYield();
        next += period;
    }
    else
        next = now + period; // late: don't bank time after a slow frame
}

GUEST_IMPORT(VdSwap)
{
    LimitFrameRate();
    uint32_t buf = ctx.r3.u32;
    uint32_t fetch[6] = {};
    if (ctx.r4.u32 >= 0x10000)
        for (int i = 0; i < 6; i++) fetch[i] = Load<uint32_t>(ctx.r4.u32 + i * 4);
    XenosTexture front;
    bool haveFront = ParseTextureFetch(fetch, front);
    Store<uint32_t>(buf, (3u << 30) | (62u << 16) | (PM4_NOP << 8)); // type 3 NOP, 63 payload dwords
    Store<uint32_t>(buf + 4, SWAP_TAG); // frame boundary marker for the command processor
    Store<uint32_t>(buf + 8, haveFront ? front.address : 0);
    Store<uint32_t>(buf + 12, haveFront ? front.width : 0);
    Store<uint32_t>(buf + 16, haveFront ? front.height : 0);
    for (uint32_t i = 5; i < 64; i++)
        Store<uint32_t>(buf + i * 4, 0);
    static int logged = 0;
    if (logged++ < 3)
        Log("GPU: VdSwap front buffer %s: %08X %ux%u format %u", haveFront ? "ok" : "unparsed", front.address, front.width, front.height, front.format);
    uint64_t n = ++g_stats.swaps;
    if (n == 1 || n % 300 == 0)
        Log("GPU: swap %llu (%llu draws so far)", (unsigned long long)n, (unsigned long long)g_stats.draws);
    ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// DPCs: KDPC { u16 type; u8 selectedCpu; u8 desiredCpu; LIST_ENTRY @4; u32 routine @0xC;
// u32 context @0x10; u32 arg1 @0x14; u32 arg2 @0x18 }. Routines run on the DPC thread.
// ---------------------------------------------------------------------------
GUEST_IMPORT(KeInitializeDpc)
{
    uint32_t dpc = ctx.r3.u32;
    memset(GuestPtr(dpc), 0, 0x1C);
    Store<uint16_t>(dpc, 19);
    Store<uint32_t>(dpc + 0xC, ctx.r4.u32);
    Store<uint32_t>(dpc + 0x10, ctx.r5.u32);
}

// u32 KeInsertQueueDpc(KDPC* dpc, u32 arg1, u32 arg2): routine(dpc, context, arg1, arg2)
GUEST_IMPORT(KeInsertQueueDpc)
{
    uint32_t dpc = ctx.r3.u32;
    Store<uint32_t>(dpc + 0x14, ctx.r4.u32);
    Store<uint32_t>(dpc + 0x18, ctx.r5.u32);
    g_dpcs.Push({ Load<uint32_t>(dpc + 0xC), { dpc, Load<uint32_t>(dpc + 0x10), ctx.r4.u32, ctx.r5.u32 }, 4 });
    StartGpu();
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(KiApcNormalRoutineNop) {}
