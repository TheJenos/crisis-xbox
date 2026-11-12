// Loader and entry point: maps the XEX into guest memory, builds the
// recompiled-function lookup table, resolves imported variables, and runs the
// title's entry point on a guest thread.
//
// usage: crysis2 <disc.iso>               (loads default.xex from the disc)
//        crysis2 <default.xex> [disc.iso]
#include "guest.h"

#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>
#include "platform.h"

#include <strings.h>

#include <image.h> // XenonUtils: XEX decryption/decompression

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern std::unordered_map<size_t, const char*> XboxKernelExports;
extern std::unordered_map<size_t, const char*> XamExports;

XexInfo g_xex{};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;

void Log(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_logMutex);
    fprintf(stderr, "[t%u] %s\n", CurrentThreadId(), buf);
    if (g_logFile)
    {
        fprintf(g_logFile, "[t%u] %s\n", CurrentThreadId(), buf);
        fflush(g_logFile);
    }
}

void Fatal(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("FATAL: %s", buf);
    PlatformExit(1);
}

// ---------------------------------------------------------------------------
// XEX header access
// ---------------------------------------------------------------------------
static uint32_t g_headerSize = 0;

uint32_t XexHeaderField(uint32_t key)
{
    uint32_t h = g_xex.headerAddr;
    uint32_t count = Load<uint32_t>(h + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t k = Load<uint32_t>(h + 0x18 + i * 8), v = Load<uint32_t>(h + 0x1C + i * 8);
        if (k != key)
            continue;
        // Keys whose low byte is 0 or 1 hold the value inline; others point to data.
        return (k & 0xFF) <= 1 ? v : h + v;
    }
    return 0;
}

// The title's icon (its dashboard tile) for the page's loading screen, which
// keeps it for the next visit (index.html Module.onTitleIcon). It is a PNG in
// the title's XDBF resource (listed in the RESOURCE_INFO header, loaded with
// the image): namespace 2 (images), id 0x8000.
static void PublishTitleIcon()
{
#ifdef __EMSCRIPTEN__
    uint32_t info = XexHeaderField(0x000002FF); // { u32 size; { char name[8]; u32 address, size; }... }
    if (!info)
        return;
    uint32_t count = (Load<uint32_t>(info) - 4) / 16;
    for (uint32_t i = 0; i < count && i < 64; i++)
    {
        uint32_t entry = info + 4 + i * 16, xdbf = Load<uint32_t>(entry + 8), size = Load<uint32_t>(entry + 12);
        if (size < 24 || Load<uint32_t>(xdbf) != 0x58444246) // "XDBF"
            continue;
        // Header: magic, version, entry table length, entry count, free table length, free count.
        uint32_t tableLength = Load<uint32_t>(xdbf + 8), entries = Load<uint32_t>(xdbf + 12), freeLength = Load<uint32_t>(xdbf + 16);
        for (uint32_t j = 0; j < entries && j < tableLength; j++)
        {
            uint32_t e = xdbf + 24 + j * 18; // { u16 namespace; u64 id; u32 offset, length; }
            if (Load<uint16_t>(e) != 2 || Load<uint64_t>(e + 2) != 0x8000)
                continue;
            // Data follows the tables (sized by their lengths; XenonUtils sizes the
            // entry table by the entry count instead, so accept either).
            uint32_t offset = Load<uint32_t>(e + 10), length = Load<uint32_t>(e + 14);
            for (uint32_t tables : { tableLength * 18 + freeLength * 8, entries * 18 + freeLength * 8 })
            {
                uint32_t png = xdbf + 24 + tables + offset;
                if (length < 8 || uint64_t(png) + length > uint64_t(xdbf) + size || Load<uint32_t>(png) != 0x89504E47) // PNG signature
                    continue;
                MAIN_THREAD_EM_ASM({ if (Module.onTitleIcon) Module.onTitleIcon(new Uint8Array(wasmMemory.buffer, $0, $1).slice()); },
                                   GuestPtr(png), length);
                return;
            }
            return;
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Imported variables
// ---------------------------------------------------------------------------
static uint32_t g_timeStampBundle = 0;

static uint32_t MakeVariable(const std::string& importName)
{
    // Export tables name variables with the import prefix ("__imp__KeTimeStampBundle").
    const std::string name = importName.rfind("__imp__", 0) == 0 ? importName.substr(7) : importName;
    if (name == "XboxHardwareInfo")
    {
        uint32_t v = KernelAlloc(16);
        Store<uint32_t>(v, 0);        // flags
        Store<uint8_t>(v + 4, 6);     // number of hardware threads
        return v;
    }
    if (name == "XboxKrnlVersion" || name == "XboxKrnlBaseVersion")
    {
        uint32_t v = KernelAlloc(8);
        Store<uint16_t>(v, 2); Store<uint16_t>(v + 2, 0); Store<uint16_t>(v + 4, 17559); Store<uint16_t>(v + 6, 0);
        return v;
    }
    if (name == "ExLoadedImageName")
    {
        const char* s = "\\Device\\Cdrom0\\default.xex";
        uint32_t v = KernelAlloc(uint32_t(strlen(s) + 1));
        memcpy(GuestPtr(v), s, strlen(s) + 1);
        return v;
    }
    if (name == "XexExecutableModuleHandle")
    {
        // Points to the module handle; the module's +0x58 is the XEX header base.
        uint32_t module = KernelAlloc(0x100);
        Store<uint32_t>(module + 0x58, g_xex.headerAddr);
        uint32_t v = KernelAlloc(4);
        Store<uint32_t>(v, module);
        return v;
    }
    if (name == "KeTimeStampBundle")
    {
        g_timeStampBundle = KernelAlloc(24);
        return g_timeStampBundle;
    }
    if (name == "VdGpuClockInMHz")
    {
        uint32_t v = KernelAlloc(4);
        Store<uint32_t>(v, 500);
        return v;
    }
    if (name == "ExConsoleGameRegion")
    {
        uint32_t v = KernelAlloc(4);
        Store<uint32_t>(v, 0xFFFFFFFF);
        return v;
    }
    // Object type tags, debugger data, device pointers: zeroed storage is enough for now.
    return KernelAlloc(64);
}

static void StartTimeStampThread()
{
    if (!g_timeStampBundle)
        return;
    std::thread([] {
        auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            uint64_t sys = uint64_t(PlatformFileTimeNow());
            uint64_t up100ns = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() * 10;
            Store<uint64_t>(g_timeStampBundle + 0, up100ns);            // interrupt time
            Store<uint64_t>(g_timeStampBundle + 8, sys);                // system time
            Store<uint32_t>(g_timeStampBundle + 16, uint32_t(up100ns / 10000)); // tick count (ms)
            PlatformSleepMs(1);
        }
    }).detach();
}

// Walk the import libraries in the raw XEX header and patch variable imports.
// Each imported function has two records: an import-table slot (type 0) and a
// thunk (type 1). XenonUtils replaced the thunks with stub code and registered
// them as symbols, so a slot with a symbol is a thunk; a type 0 slot whose name
// matches a thunk is that function's table entry; the rest are variables.
static void ResolveVariableImports(const std::vector<uint8_t>& file, const Image& image)
{
    auto be32 = [&](size_t o) { return __builtin_bswap32(*(const uint32_t*)&file[o]); };
    auto be16 = [&](size_t o) { return __builtin_bswap16(*(const uint16_t*)&file[o]); };
    uint32_t count = be32(0x14);
    uint32_t importOff = 0;
    for (uint32_t i = 0; i < count; i++)
        if (be32(0x18 + i * 8) == 0x000103FF)
            importOff = be32(0x1C + i * 8);
    if (!importOff)
        return;

    uint32_t strTableSize = be32(importOff + 4), numLibs = be32(importOff + 8);
    std::vector<std::string> libNames;
    size_t p = importOff + 12;
    for (uint32_t i = 0; i < numLibs; i++)
    {
        libNames.emplace_back((const char*)&file[p]);
        p += (libNames.back().size() + 1 + 3) & ~size_t(3);
    }

    p = importOff + 12 + strTableSize;
    for (uint32_t lib = 0; lib < numLibs; lib++)
    {
        uint32_t libSize = be32(p);
        uint16_t nameIdx = be16(p + 36), numImports = be16(p + 38);
        const auto& exports = libNames[nameIdx] == "xam.xex" ? XamExports : XboxKernelExports;

        // Non-thunk records were byte-swapped in place while loading, so they
        // read back host-endian: type in bits 24..31, ordinal in bits 0..15.
        std::set<std::string> functionNames;
        std::map<std::string, std::vector<uint32_t>> dataSlots;
        for (uint32_t k = 0; k < numImports; k++)
        {
            uint32_t slot = be32(p + 40 + k * 4);
            auto sym = image.symbols.find(slot);
            if (sym != image.symbols.end())
            {
                functionNames.insert(sym->name);
                continue;
            }
            uint32_t rec = *(uint32_t*)GuestPtr(slot);
            uint32_t ordinal = rec & 0xFFFF;
            auto it = exports.find(ordinal);
            std::string name = it != exports.end() ? it->second : libNames[nameIdx] + "#" + std::to_string(ordinal);
            dataSlots[name].push_back(slot);
        }
        for (auto& [name, slots] : dataSlots)
        {
            if (functionNames.count(name))
            {
                for (uint32_t slot : slots) Store<uint32_t>(slot, 0); // unused: calls go to the recompiled import
                continue;
            }
            uint32_t var = MakeVariable(name);
            for (uint32_t slot : slots) Store<uint32_t>(slot, var);
            Log("variable import %s -> %08X", name.c_str(), var);
        }
        p += libSize;
    }
}

static void UnknownFunction(PPCContext& ctx, PPC_BASE_TYPE)
{
#ifndef PPC_CONFIG_CTR_AS_LOCAL
    Fatal("indirect call to %08X, which is not a known function (return address %08llX)", ctx.ctr.u32, ctx.lr);
#else // ctr is a local of the calling function (XenonRecomp ctr_as_local): the target is not visible here
    Fatal("indirect call to a function that is not known (return address %08llX)", ctx.lr);
#endif
}

// GAME_CVARS="name=value;name2=value2": extra console variables for the title,
// appended to the overlay's system.cfg (read at startup; later lines win). The
// browser page sets e.g. sys_MaxFPS from its settings this way.
static void AppendGameCvars()
{
    const char* cvars = getenv("GAME_CVARS");
    if (!cvars || !*cvars)
        return;
    const char* dir = getenv("OVERLAY_DIR") ? getenv("OVERLAY_DIR") : "overlay";
    std::string path = std::string(dir) + PlatformPathSeparator() + "system.cfg";
    FILE* f = fopen(path.c_str(), "a");
    if (!f)
    {
        Log("GAME_CVARS: cannot open %s", path.c_str());
        return;
    }
    fputs("\n; GAME_CVARS (runtime)\n", f);
    std::string all = cvars;
    for (size_t at = 0; at <= all.size();)
    {
        size_t end = all.find(';', at);
        if (end == std::string::npos) end = all.size();
        std::string item = all.substr(at, end - at);
        size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0)
        {
            fprintf(f, "%s = %s\n", item.substr(0, eq).c_str(), item.substr(eq + 1).c_str());
            Log("GAME_CVARS: %s = %s", item.substr(0, eq).c_str(), item.substr(eq + 1).c_str());
        }
        at = end + 1;
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    g_logFile = fopen("runtime.log", "w");
    PlatformInstallCrashHandler();
    AppendGameCvars();
    if (argc < 2)
        Fatal("usage: crysis2 <disc.iso | extracted disc folder> | <default.xex> [disc]");

    std::string first = argv[1];
    auto endsWith = [](const std::string& s, const char* suffix) {
        size_t n = strlen(suffix);
        return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
    };
    // The disc: an .iso image, an extracted disc folder, or (browser) a URL ending in '/'.
    bool isUrlDir = (first.rfind("url:", 0) == 0 || first.rfind("http", 0) == 0) && first.back() == '/';
    bool fromDisc = endsWith(first, ".iso") || isUrlDir || PlatformPathKind(first) == PathKind::Directory;
    const char* isoPath = fromDisc ? argv[1] : (argc > 2 ? argv[2] : nullptr);
    const char* xexPath = fromDisc ? "default.xex" : argv[1];

    if (!MemoryInit())
        Fatal("could not reserve 4 GB of guest address space");
    Log("guest memory at %p", g_base);

    if (isoPath && !DiscMount(isoPath))
        Fatal("cannot mount disc image %s", isoPath);
    if (!isoPath)
        Log("no disc image given: every file open will fail");

    std::vector<uint8_t> file;
    if (fromDisc)
    {
        if (!DiscReadFile("default.xex", file))
            Fatal("default.xex not found on the disc");
    }
    else
    {
        std::ifstream in(xexPath, std::ios::binary);
        if (!in)
            Fatal("cannot open %s", xexPath);
        file.assign(std::istreambuf_iterator<char>(in), {});
    }

    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
        Fatal("failed to load %s", xexPath);
    if (image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE)
        Fatal("image %08zX+%X does not match the recompiled code (%08llX+%llX)", image.base, image.size, PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
    CommitRange(uint32_t(image.base), image.size);
    memcpy(GuestPtr(uint32_t(image.base)), image.data.get(), image.size);
    Log("loaded image %08zX-%08zX, entry %08zX", image.base, image.base + image.size, image.entry_point);

    // Recompiled-function lookup table: one 8-byte host pointer per guest instruction.
    uint32_t tableAddr = uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
    uint32_t tableSize = uint32_t(PPC_CODE_SIZE * 2);
    if (!CommitRange(tableAddr, tableSize))
        Fatal("could not commit the function table");
    auto** table = (PPCFunc**)GuestPtr(tableAddr);
    for (uint32_t i = 0; i < tableSize / 8; i++)
        table[i] = UnknownFunction;
    size_t mapped = 0;
    for (PPCFuncMapping* m = PPCFuncMappings; m->host; m++, mapped++)
        PPC_LOOKUP_FUNC(g_base, m->guest) = m->host;
    Log("function table: %zu functions", mapped);

    // XEX header copy for RtlImageXexHeaderField and friends.
    g_headerSize = __builtin_bswap32(*(uint32_t*)&file[8]);
    g_xex.headerAddr = KernelAlloc(g_headerSize);
    memcpy(GuestPtr(g_xex.headerAddr), file.data(), g_headerSize);
    g_xex.imageBase = uint32_t(image.base);
    g_xex.entryPoint = uint32_t(image.entry_point);
    g_xex.stackSize = XexHeaderField(0x00020200);
    if (!g_xex.stackSize) g_xex.stackSize = 0x40000;
    g_xex.systemFlags = XexHeaderField(0x00030000);
    if (uint32_t tls = XexHeaderField(0x00020104))
    {
        g_xex.tlsSlots = Load<uint32_t>(tls);
        g_xex.tlsRawData = Load<uint32_t>(tls + 4);
        g_xex.tlsDataSize = Load<uint32_t>(tls + 8);
        g_xex.tlsRawDataSize = Load<uint32_t>(tls + 12);
    }
    if (uint32_t exec = XexHeaderField(0x00040006))
        g_xex.titleId = Load<uint32_t>(exec + 12);
    Log("title %08X, stack %X, TLS: %u slots, data %08X (%X bytes, %X raw)", g_xex.titleId, g_xex.stackSize,
        g_xex.tlsSlots, g_xex.tlsRawData, g_xex.tlsDataSize, g_xex.tlsRawDataSize);
    PublishTitleIcon();

    ResolveVariableImports(file, image);
    StartTimeStampThread();
    XmaStart();

    // DUMP_THREADS=<seconds>[,<interval ms>,<count>]: log every thread's state
    // after that long, optionally repeatedly (DUMP_THREAD_FILTER=<name> limits
    // the dump to threads whose name contains it). Diagnostics.
    if (const char* dump = getenv("DUMP_THREADS"))
    {
        int seconds = atoi(dump), interval = 0, count = 1;
        if (const char* c = strchr(dump, ','))
        {
            interval = atoi(c + 1);
            if (const char* c2 = strchr(c + 1, ',')) count = atoi(c2 + 1);
        }
        std::thread([seconds, interval, count] {
            PlatformSleepMs(uint32_t(seconds) * 1000);
            for (int i = 0; i < count; i++)
            {
                DumpThreads();
                if (interval) PlatformSleepMs(uint32_t(interval));
            }
        }).detach();
    }

    Log("starting title at %08X", g_xex.entryPoint);
    RunMainThread(g_xex.entryPoint);
    Log("main thread returned");
    return 0;
}
