// Shared definitions for the host runtime that runs recompiled Xbox 360 code.
//
// Guest memory is one 4 GB region starting at g_base; a guest address is an
// offset into it. Guest data is big-endian, so all accesses go through
// Load/Store, which byte-swap.
#pragma once

#include "ppc_config.h"
#include "ppc_context.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

extern uint8_t* g_base;

// Implement a kernel/XAM import. Arguments arrive in r3..r10, the result goes in r3.
// IMPORT_TRACE=<guest thread id>: that thread counts its import calls (debugging).
extern bool g_importTrace;
extern thread_local bool g_importSample; // this call should log its arguments
// IMPORT_FIRST=1: log the first time each (thread, import, call site + 3 callers) occurs (debugging).
extern bool g_importFirst;
void ImportFirstSeen(const char* name, PPCContext& ctx);
void ImportTraceHit(const char* name);
#define GUEST_IMPORT(name)     static void ImportBody_##name(PPCContext& __restrict ctx, PPC_BASE_TYPE base);     PPC_FUNC(__imp__##name) { if (g_importTrace) ImportTraceHit(#name); if (g_importFirst) ImportFirstSeen(#name, ctx); ImportBody_##name(ctx, base); }     static void ImportBody_##name(PPCContext& __restrict ctx, PPC_BASE_TYPE base)

// ---------------------------------------------------------------------------
// Memory access
// ---------------------------------------------------------------------------
// Physical-memory views (0xC0000000, 0xE0000000) fold onto 0xA0000000, matching generated code.
inline void* GuestPtr(uint32_t addr) { return g_base + ppc_fold_view(addr); }
inline uint32_t GuestAddr(const void* p) { return uint32_t((const uint8_t*)p - g_base); }

template<class T> inline T ByteSwapT(T v)
{
    if constexpr (sizeof(T) == 1) return v;
    else if constexpr (sizeof(T) == 2) { uint16_t x; memcpy(&x, &v, 2); x = __builtin_bswap16(x); memcpy(&v, &x, 2); return v; }
    else if constexpr (sizeof(T) == 4) { uint32_t x; memcpy(&x, &v, 4); x = __builtin_bswap32(x); memcpy(&v, &x, 4); return v; }
    else { uint64_t x; memcpy(&x, &v, 8); x = __builtin_bswap64(x); memcpy(&v, &x, 8); return v; }
}

template<class T> inline T Load(uint32_t addr) { T v; memcpy(&v, GuestPtr(addr), sizeof(T)); return ByteSwapT(v); }
template<class T> inline void Store(uint32_t addr, T v) { v = ByteSwapT(v); memcpy(GuestPtr(addr), &v, sizeof(T)); }

inline std::string LoadGuestString(uint32_t addr, size_t max = 4096)
{
    std::string s;
    if (addr == 0) return s;
    const char* p = (const char*)GuestPtr(addr);
    for (size_t i = 0; i < max && p[i]; i++) s += p[i];
    return s;
}

// Arguments past the 8th live in the caller's parameter save area: 8-byte slots
// starting at r1 + 0x50, with 32-bit values in the low (second) half.
// `index` is 0-based, so the 9th argument is index 8.
inline uint64_t StackArg64(const PPCContext& ctx, uint32_t index) { return Load<uint64_t>(ctx.r1.u32 + 0x50 + (index - 8) * 8); }
inline uint32_t StackArg32(const PPCContext& ctx, uint32_t index) { return Load<uint32_t>(ctx.r1.u32 + 0x54 + (index - 8) * 8); }

// ANSI_STRING / UNICODE_STRING: { u16 Length, u16 MaximumLength, u32 Buffer }
std::string LoadAnsiString(uint32_t ansiString);

// ---------------------------------------------------------------------------
// Guest memory allocation (memory.cpp)
// ---------------------------------------------------------------------------
bool MemoryInit();
// Title-visible virtual memory (NtAllocateVirtualMemory), 64 KB granularity.
uint32_t VirtualAlloc(uint32_t size, uint32_t align = 0x10000);
uint32_t VirtualAllocAt(uint32_t addr, uint32_t size); // commit inside or claim a fixed range
void VirtualFree(uint32_t addr);
void VirtualDecommit(uint32_t addr, uint32_t size); // zero-on-recommit, keeps the reservation
uint32_t VirtualQuerySize(uint32_t addr);
// Physical memory (MmAllocatePhysicalMemoryEx), mapped at 0xA0000000+.
uint32_t PhysicalAlloc(uint32_t size, uint32_t align);
void PhysicalFree(uint32_t addr);
uint32_t PhysicalQuerySize(uint32_t addr);
// Small allocations for kernel structures the runtime creates (zeroed, 16-byte aligned).
uint32_t KernelAlloc(uint32_t size, uint32_t align = 16);
void KernelFree(uint32_t addr);
// Commit an arbitrary guest range (used for the image and function table).
bool CommitRange(uint32_t addr, uint32_t size);

// ---------------------------------------------------------------------------
// Threads (objects.cpp)
// ---------------------------------------------------------------------------
struct GuestThread;
GuestThread* CurrentThread();
uint32_t CurrentKThread(); // guest address of the current KTHREAD
uint32_t CurrentThreadId();

// Create a guest thread that calls `entry(r3 = arg)`, or `startup(entry, arg)` if startup != 0.
// Returns the handle; the thread starts suspended if `suspended`.
uint32_t CreateGuestThread(uint32_t stackSize, uint32_t startup, uint32_t entry, uint32_t arg,
                           bool suspended, uint32_t* threadIdOut, bool isMain = false);
void RunMainThread(uint32_t entry);

// Call a guest function at `addr` using the given context.
void GuestCall(PPCContext& ctx, uint32_t addr);
// Log the current guest thread's main registers (for crash reports).
void LogGuestRegisters();
void SetThreadName(uint32_t threadId, const char* name); // threadId 0xFFFFFFFF = current thread
void DumpThreads(); // log every guest thread's state (diagnostics)

// PROFILE=1 sampling profiler counters (objects.cpp); host code adds its costs here.
namespace prof
{
    extern std::atomic<uint64_t> cpBusyNs, cpWaitNs, submitWaitNs, timebaseReads;
    // Command processor time by phase (gpu_draw.cpp, see kCpPhaseNames in
    // objects.cpp) and work counters (bytes hashed, copied, ...).
    enum CpPhase { CP_INDICES, CP_STATE, CP_VERTEX, CP_CONSTANTS, CP_TEXTURES, CP_RECORD, CP_RESOLVE, CP_FRAME_END, CP_PHASES };
    enum CpCount { CPC_TEX_HASH_BYTES, CPC_TEX_DECODE_BYTES, CPC_VTX_COPY_BYTES, CPC_VTX_CHECK_BYTES, CPC_CONST_UPLOADS, CPC_INDICES, CPC_COUNTS };
    extern std::atomic<uint64_t> cpPhaseNs[CP_PHASES], cpCount[CPC_COUNTS];
    bool Enabled();
}
void XmaStart(); // XMA decoder emulation (audio.cpp)
void XmaPump();  // run the XMA decoder now (called when the title locks a context)

// Start a runtime-owned thread with full guest thread state; `body` runs on it
// and can call guest functions with the provided context.
void StartNativeGuestThread(const char* name, std::function<void(PPCContext&)> body);

// ---------------------------------------------------------------------------
// Handles and dispatcher objects (objects.cpp)
// ---------------------------------------------------------------------------
// Kernel objects live in guest memory as their real DISPATCHER_HEADER-based
// structures; a handle maps to the guest address of the object.
enum class ObjectKind : uint32_t { Event, Semaphore, Mutant, Thread, Timer, File, Other };
uint32_t CreateHandle(ObjectKind kind, uint32_t object);
bool LookupHandle(uint32_t handle, ObjectKind* kind, uint32_t* object);
bool CloseHandle(uint32_t handle);

// Wait for guest dispatcher objects. timeoutPtr: guest pointer to an i64 in
// 100 ns units (negative = relative), 0 = infinite.
uint32_t WaitForObjects(const uint32_t* objects, uint32_t count, bool waitAll, uint32_t timeoutPtr, bool alertable);
void SignalObjectsChanged();
void SignalEventObject(uint32_t obj);
// Create a guest object with a DISPATCHER_HEADER of the given type and signal state.
uint32_t CreateDispatcherObject(uint32_t size, uint8_t type, int32_t state);

// Disc image (fs.cpp)
bool DiscMount(const char* isoPath);
bool DiscReadFile(const std::string& path, std::vector<uint8_t>& out); // path relative to the disc root
void FsFileClosed(uint32_t object); // the title closed its last handle to a file object (CloseHandle)

// Loader-provided image information (main.cpp)
struct XexInfo
{
    uint32_t headerAddr;      // copy of the XEX header in guest memory
    uint32_t imageBase;
    uint32_t entryPoint;
    uint32_t stackSize;
    uint32_t tlsSlots, tlsRawData, tlsDataSize, tlsRawDataSize;
    uint32_t systemFlags;
    uint32_t titleId;
};
extern XexInfo g_xex;
uint32_t XexHeaderField(uint32_t key); // value or guest pointer, 0 if missing

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
void Log(const char* fmt, ...);
[[noreturn]] void Fatal(const char* fmt, ...);

// NTSTATUS values
constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_ABANDONED_WAIT_0 = 0x80;
constexpr uint32_t STATUS_USER_APC = 0xC0;
constexpr uint32_t STATUS_ALERTED = 0x101;
constexpr uint32_t STATUS_TIMEOUT = 0x102;
constexpr uint32_t STATUS_PENDING = 0x103;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
