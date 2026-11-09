// Guest memory: guest addresses 0x40000000-0xBFFFFFFF backed by host memory
// (see PlatformReserveGuest), committed on demand where the platform allows.
//
// Guest address map (following the Xbox 360 layout):
//   0x40000000-0x7EFFFFFF  title virtual memory (NtAllocateVirtualMemory)
//   0x7F000000-0x7FBFFFFF  runtime-created kernel structures (threads, PCRs, TLS, objects)
//   0x7FC80000-0x7FFFFFFF  hardware registers: GPU at 0x7FC80000, audio/XMA at 0x7FEA0000
//   0x82000000-...         XEX image, followed by the recompiled-function lookup table
//   0xA0000000-0xBFFFFFFF  physical memory, 64 KB page view (MmAllocatePhysicalMemoryEx)
//   0xC0000000-0xDFFFFFFF  the same physical memory, 16 MB page view
//   0xE0000000-0xFFFFFFFF  the same physical memory, 4 KB page view, offset by 0x1000
//
// The two upper views are folded onto 0xA0000000 in software on every access
// (ppc_fold_view, used by generated code and by GuestPtr/Load/Store).
#include "guest.h"
#include "platform.h"

#include <map>
#include <mutex>

uint8_t* g_base = nullptr;

namespace
{
    constexpr uint32_t PHYSICAL_BASE = 0xA0000000, PHYSICAL_SIZE = 0x20000000;

    // First-fit allocator over a guest address range. Freed ranges read back as
    // zero when reallocated, like fresh pages.
    class RangeAllocator
    {
    public:
        RangeAllocator(uint32_t begin, uint32_t end, uint32_t granularity)
            : begin_(begin), end_(end), granularity_(granularity) {}

        uint32_t Alloc(uint32_t size, uint32_t align)
        {
            std::lock_guard lock(mutex_);
            size = RoundUp(size == 0 ? 1 : size, granularity_);
            align = std::max(align, granularity_);
            uint32_t candidate = RoundUp(begin_, align);
            for (auto& [addr, len] : used_)
            {
                if (candidate + size <= addr)
                    break;
                candidate = std::max(candidate, RoundUp(addr + len, align));
            }
            if (uint64_t(candidate) + size > end_)
                return 0;
            if (!CommitRange(candidate, size))
                return 0;
            used_[candidate] = size;
            return candidate;
        }

        // Claim (or re-commit inside) a specific range.
        uint32_t AllocAt(uint32_t addr, uint32_t size)
        {
            std::lock_guard lock(mutex_);
            uint32_t start = addr & ~(granularity_ - 1);
            uint32_t len = RoundUp(addr + size, granularity_) - start;
            auto it = used_.upper_bound(start);
            if (it != used_.begin())
            {
                auto prev = std::prev(it);
                if (start >= prev->first && start + len <= prev->first + prev->second)
                    return CommitRange(start, len) ? addr : 0; // inside an existing reservation
            }
            for (auto& [a, l] : used_)
                if (start < a + l && a < start + len)
                    return 0; // overlaps something else
            if (start < begin_ || uint64_t(start) + len > end_ || !CommitRange(start, len))
                return 0;
            used_[start] = len;
            return addr;
        }

        bool Free(uint32_t addr)
        {
            std::lock_guard lock(mutex_);
            auto it = used_.find(addr);
            if (it == used_.end())
                return false;
            // Sub-page blocks share host pages with neighbours: zero them instead.
            if (granularity_ >= 0x1000)
                PlatformDecommit(g_base + it->first, it->second);
            else
                memset(g_base + it->first, 0, it->second);
            used_.erase(it);
            return true;
        }

        uint32_t QuerySize(uint32_t addr)
        {
            std::lock_guard lock(mutex_);
            auto it = used_.upper_bound(addr);
            if (it == used_.begin())
                return 0;
            --it;
            return addr < it->first + it->second ? it->second : 0;
        }

    private:
        static uint32_t RoundUp(uint64_t v, uint32_t a) { return uint32_t((v + a - 1) & ~uint64_t(a - 1)); }

        uint32_t begin_, end_, granularity_;
        std::map<uint32_t, uint32_t> used_;
        std::mutex mutex_;
    };

    RangeAllocator g_virtual(0x40000000, 0x7F000000, 0x10000);
    RangeAllocator g_kernel(0x7F000000, 0x7FC00000, 0x10); // below the hardware registers
    RangeAllocator g_physical(PHYSICAL_BASE, PHYSICAL_BASE + PHYSICAL_SIZE, 0x1000);
}

bool CommitRange(uint32_t addr, uint32_t size)
{
    return PlatformCommit(g_base + addr, size);
}

bool MemoryInit()
{
    if (!PlatformReserveGuest(&g_base))
        return false;
    // Generated code assumes base is 32-byte aligned (PPC_FUNC_PROLOGUE).
    if (((uintptr_t)g_base & 0x1F) != 0)
        return false;
    // Hardware register blocks are accessed as plain memory by the title. Back
    // them with RAM so reads return the last value written; the GPU command
    // processor keeps its registers in the same memory.
    return CommitRange(0x7FC80000, 0x80000000 - 0x7FC80000);
}

uint32_t VirtualAlloc(uint32_t size, uint32_t align) { return g_virtual.Alloc(size, align); }
uint32_t VirtualAllocAt(uint32_t addr, uint32_t size) { return g_virtual.AllocAt(addr, size); }
void VirtualFree(uint32_t addr) { g_virtual.Free(addr); }
void VirtualDecommit(uint32_t addr, uint32_t size) { PlatformDecommit(g_base + addr, size); }
uint32_t VirtualQuerySize(uint32_t addr) { return g_virtual.QuerySize(addr); }

// The allocator works in the 0xA0000000 view; any view's address maps to it.
static uint32_t ToPhysicalView(uint32_t addr) { return ppc_fold_view(addr); }
uint32_t PhysicalAlloc(uint32_t size, uint32_t align) { return g_physical.Alloc(size, align); }
void PhysicalFree(uint32_t addr) { g_physical.Free(ToPhysicalView(addr)); }
uint32_t PhysicalQuerySize(uint32_t addr) { return g_physical.QuerySize(ToPhysicalView(addr)); }

uint32_t KernelAlloc(uint32_t size, uint32_t align) { return g_kernel.Alloc(size, align); }
void KernelFree(uint32_t addr) { g_kernel.Free(addr); }

// ---------------------------------------------------------------------------
// Memory imports
// ---------------------------------------------------------------------------
constexpr uint32_t MEM_COMMIT_FLAG = 0x1000, MEM_RESERVE_FLAG = 0x2000;

// Page size requested by the allocation type: MEM_LARGE_PAGES (0x20000000) = 64 KB, otherwise 4 KB.
static uint32_t PageSizeForType(uint32_t type) { return (type & 0x20000000) ? 0x10000 : 0x1000; }

// NTSTATUS NtAllocateVirtualMemory(u32* baseAddress, u32* regionSize, u32 allocType, u32 protect, u32 debug)
// Like the real kernel, the returned base is rounded down to the page size and
// the size is rounded up to whole pages; callers use these to track heap ends.
GUEST_IMPORT(NtAllocateVirtualMemory)
{
    uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t want = Load<uint32_t>(basePtr), size = Load<uint32_t>(sizePtr);
    if (size == 0 || !(type & (MEM_COMMIT_FLAG | MEM_RESERVE_FLAG)))
    {
        ctx.r3.u64 = STATUS_INVALID_PARAMETER;
        return;
    }
    uint32_t page = PageSizeForType(type);
    uint32_t adjustedSize = (size + page - 1) & ~(page - 1);
    uint32_t adjustedBase = want & ~(page - 1);
    uint32_t addr = want ? VirtualAllocAt(adjustedBase, adjustedSize) : VirtualAlloc(adjustedSize, page);
    if (addr == 0)
    {
        Log("NtAllocateVirtualMemory(%08X, %X, %X) failed", want, size, type);
        ctx.r3.u64 = STATUS_NO_MEMORY;
        return;
    }
    Store<uint32_t>(basePtr, addr);
    Store<uint32_t>(sizePtr, adjustedSize);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtFreeVirtualMemory(u32* baseAddress, u32* regionSize, u32 freeType, u32 debug)
GUEST_IMPORT(NtFreeVirtualMemory)
{
    uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t addr = Load<uint32_t>(basePtr), size = Load<uint32_t>(sizePtr);
    if (type & 0x8000) // MEM_RELEASE: the whole allocation
        VirtualFree(addr);
    else if (type & 0x4000) // MEM_DECOMMIT: pages read back as zero once recommitted
        VirtualDecommit(addr, size);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryVirtualMemory(u32 address, MEMORY_BASIC_INFORMATION* info, ...)
GUEST_IMPORT(NtQueryVirtualMemory)
{
    uint32_t addr = ctx.r3.u32, info = ctx.r4.u32;
    uint32_t size = VirtualQuerySize(addr);
    memset(GuestPtr(info), 0, 28);
    Store<uint32_t>(info + 0, addr & ~0xFFFu);   // BaseAddress
    Store<uint32_t>(info + 4, addr & ~0xFFFu);   // AllocationBase
    Store<uint32_t>(info + 8, 4);                // AllocationProtect: PAGE_READWRITE
    Store<uint32_t>(info + 12, size);            // RegionSize
    Store<uint32_t>(info + 16, size ? 0x1000 : 0x10000); // State: MEM_COMMIT / MEM_FREE
    Store<uint32_t>(info + 20, 4);               // Protect
    Store<uint32_t>(info + 24, 0x20000);         // Type: MEM_PRIVATE
    ctx.r3.u64 = STATUS_SUCCESS;
}

// void* MmAllocatePhysicalMemoryEx(u32 flags, u32 size, u32 protect, u32 minAddr, u32 maxAddr, u32 alignment)
GUEST_IMPORT(MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ctx.r4.u32, protect = ctx.r5.u32, align = ctx.r8.u32;
    // Page size from the protect flags: 0x20000000 = 4 KB, 0x80000000 = 16 MB, otherwise 64 KB.
    uint32_t pageSize = (protect & 0x20000000) ? 0x1000 : (protect & 0x80000000) ? 0x1000000 : 0x10000;
    uint32_t addr = PhysicalAlloc(size, std::max(pageSize, align));
    if (!addr)
        Log("MmAllocatePhysicalMemoryEx(size %X, align %X) failed", size, align);
    // 16 MB-page allocations come back in the 0xC0000000 view, as from the kernel.
    // 4 KB pages stay in the 0xA0000000 view: on the console the 0xE0000000 view is
    // offset by 0x1000 from physical memory (and D3D adds that offset itself when it
    // computes GPU addresses), which 64 KB-granular host mappings can't reproduce.
    else if (pageSize == 0x1000000)
        addr = 0xC0000000 | (addr & 0x1FFFFFFF);
    ctx.r3.u64 = addr;
}

GUEST_IMPORT(MmFreePhysicalMemory) { PhysicalFree(ctx.r4.u32); }

GUEST_IMPORT(MmQueryAllocationSize)
{
    uint32_t addr = ctx.r3.u32;
    uint32_t size = addr >= 0xA0000000 ? PhysicalQuerySize(addr) : VirtualQuerySize(addr);
    ctx.r3.u64 = size;
}

GUEST_IMPORT(MmGetPhysicalAddress) { ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF; }

GUEST_IMPORT(MmQueryAddressProtect) { ctx.r3.u64 = 4; } // PAGE_READWRITE

// NTSTATUS MmQueryStatistics(MM_STATISTICS* stats)
GUEST_IMPORT(MmQueryStatistics)
{
    uint32_t stats = ctx.r3.u32;
    uint32_t len = Load<uint32_t>(stats);
    memset(GuestPtr(stats + 4), 0, len > 4 ? len - 4 : 0);
    Store<uint32_t>(stats + 4, 0x20000);  // TotalPhysicalPages (512 MB / 4 KB)
    Store<uint32_t>(stats + 8, 0x200);    // KernelPages
    Store<uint32_t>(stats + 12, 0x10000); // TitleAvailablePages
    Store<uint32_t>(stats + 16, 0x1E000); // TitleTotalVirtualMemoryBytes / page
    ctx.r3.u64 = STATUS_SUCCESS;
}

// void* ExAllocatePoolTypeWithTag(u32 size, u32 tag, u32 poolType)
GUEST_IMPORT(ExAllocatePoolTypeWithTag)
{
    uint32_t size = ctx.r3.u32;
    ctx.r3.u64 = size < 0x10000 ? KernelAlloc(size, 16) : PhysicalAlloc(size, 0x1000);
}
GUEST_IMPORT(ExAllocatePoolWithTag) { __imp__ExAllocatePoolTypeWithTag(ctx, base); }
GUEST_IMPORT(ExAllocatePool) { __imp__ExAllocatePoolTypeWithTag(ctx, base); }

GUEST_IMPORT(ExFreePool)
{
    uint32_t addr = ctx.r3.u32;
    if (addr >= 0xA0000000) PhysicalFree(addr);
    else KernelFree(addr);
}

// XamAlloc(u32 flags, u32 size, u32* result)
GUEST_IMPORT(XamAlloc)
{
    uint32_t size = ctx.r4.u32, out = ctx.r5.u32;
    uint32_t addr = size < 0x10000 ? KernelAlloc(size, 16) : VirtualAlloc(size);
    Store<uint32_t>(out, addr);
    ctx.r3.u64 = addr ? 0 : 0x8007000E; // E_OUTOFMEMORY
}

GUEST_IMPORT(XamFree)
{
    uint32_t addr = ctx.r3.u32;
    if (addr >= 0x7F000000 && addr < 0x7FC00000) KernelFree(addr);
    else VirtualFree(addr);
    ctx.r3.u64 = 0;
}
