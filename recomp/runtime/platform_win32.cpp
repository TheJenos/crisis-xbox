// Native Windows platform backend.
#ifdef _WIN32

#include "platform.h"
#include "guest.h"

#include <windows.h>

// ---------------------------------------------------------------------------
// Guest address space: one reservation for 0x40000000-0xBFFFFFFF, committed on demand.
// ---------------------------------------------------------------------------
bool PlatformReserveGuest(uint8_t** base)
{
    auto* window = (uint8_t*)::VirtualAlloc(nullptr, GUEST_HIGH - GUEST_LOW, MEM_RESERVE, PAGE_READWRITE);
    if (!window)
        return false;
    *base = window - GUEST_LOW;
    return true;
}

bool PlatformCommit(uint8_t* p, size_t n)
{
    uintptr_t start = uintptr_t(p) & ~uintptr_t(0xFFF);
    uintptr_t end = (uintptr_t(p) + n + 0xFFF) & ~uintptr_t(0xFFF);
    return ::VirtualAlloc((void*)start, end - start, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void PlatformDecommit(uint8_t* p, size_t n)
{
    uintptr_t start = uintptr_t(p) & ~uintptr_t(0xFFF);
    uintptr_t end = (uintptr_t(p) + n + 0xFFF) & ~uintptr_t(0xFFF);
    ::VirtualFree((void*)start, end - start, MEM_DECOMMIT);
}

// ---------------------------------------------------------------------------
// Threads and time
// ---------------------------------------------------------------------------
static DWORD WINAPI ThreadTrampoline(void* param)
{
    auto* body = (std::function<void()>*)param;
    (*body)();
    delete body;
    return 0;
}

bool PlatformStartThread(std::function<void()> body, size_t stackSize)
{
    auto* heapBody = new std::function<void()>(std::move(body));
    HANDLE h = ::CreateThread(nullptr, stackSize, ThreadTrampoline, heapBody, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h)
    {
        delete heapBody;
        return false;
    }
    ::CloseHandle(h);
    return true;
}

void PlatformSleepMs(uint32_t ms) { ::Sleep(ms); }
void PlatformSleepMsPrecise(uint32_t ms) { ::Sleep(ms); }

void PlatformUploadDump(const char* name, const void* data, size_t size)
{
    if (FILE* f = fopen(name, "wb"))
    {
        fwrite(data, 1, size, f);
        fclose(f);
    }
}
void PlatformYield() { ::SwitchToThread(); }

uint64_t PlatformCounterNs()
{
    static const uint64_t freq = [] { LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    uint64_t t = uint64_t(c.QuadPart);
    return (t / freq) * 1000000000ull + (t % freq) * 1000000000ull / freq;
}

int64_t PlatformFileTimeNow()
{
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    return (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// ---------------------------------------------------------------------------
// Host files
// ---------------------------------------------------------------------------
struct PlatformFile { HANDLE h; };

PlatformFile* PlatformOpenFile(const std::string& path, OpenMode mode)
{
    DWORD access = mode == OpenMode::Read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    DWORD disposition = mode == OpenMode::CreateReadWrite ? OPEN_ALWAYS
                      : mode == OpenMode::TruncateReadWrite ? CREATE_ALWAYS : OPEN_EXISTING;
    HANDLE h = ::CreateFileA(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             disposition, mode == OpenMode::Read ? FILE_FLAG_RANDOM_ACCESS : FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : new PlatformFile{ h };
}

bool PlatformReadAt(PlatformFile* f, uint64_t offset, void* dest, uint32_t size)
{
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD got = 0;
    return ::ReadFile(f->h, dest, size, &got, &ov) && got == size;
}

bool PlatformWriteAt(PlatformFile* f, uint64_t offset, const void* src, uint32_t size)
{
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset);
    ov.OffsetHigh = DWORD(offset >> 32);
    DWORD written = 0;
    return ::WriteFile(f->h, src, size, &written, &ov) && written == size;
}

void PlatformCloseFile(PlatformFile* f)
{
    if (!f) return;
    CloseHandle(f->h);
    delete f;
}

uint64_t PlatformFileSize(PlatformFile* f)
{
    LARGE_INTEGER sz;
    return ::GetFileSizeEx(f->h, &sz) ? uint64_t(sz.QuadPart) : 0;
}

bool PlatformSetFileSize(PlatformFile* f, uint64_t size)
{
    LARGE_INTEGER sz;
    sz.QuadPart = int64_t(size);
    return ::SetFilePointerEx(f->h, sz, nullptr, FILE_BEGIN) && ::SetEndOfFile(f->h);
}

PathKind PlatformPathKind(const std::string& path)
{
    DWORD a = ::GetFileAttributesA(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return PathKind::Missing;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? PathKind::Directory : PathKind::File;
}

bool PlatformMakeDirectory(const std::string& path) { return ::CreateDirectoryA(path.c_str(), nullptr) || ::GetLastError() == ERROR_ALREADY_EXISTS; }

bool PlatformListDirectory(const std::string& path, std::vector<PlatformDirEntry>& out)
{
    out.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = ::FindFirstFileA((path + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do
    {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        out.push_back({ fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                        (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow });
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
    return true;
}

bool PlatformRemoveFile(const std::string& path) { return ::DeleteFileA(path.c_str()) != 0; }
char PlatformPathSeparator() { return '\\'; }

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
void PlatformExit(int code)
{
    fflush(nullptr);
    ::ExitProcess(code);
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363 || code == 0x406D1388 || code < 0x80000000) // C++ throw, thread naming, informational
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t addr = info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[1] : 0;
    uintptr_t lo = uintptr_t(g_base) + GUEST_LOW, hi = uintptr_t(g_base) + GUEST_HIGH;
    bool guest = addr >= lo && addr < hi;
    // Offset from the exe's preferred base, for llvm-symbolizer --obj=crysis2.exe.
    uintptr_t module = (uintptr_t)::GetModuleHandleA(nullptr);
    uintptr_t pc = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
    Log("CRASH: exception %08lX at host %p (exe 0x%llX), data address %p%s%08llX",
        code, (void*)pc, (unsigned long long)(pc - module + 0x140000000ull), (void*)addr,
        guest ? " = guest " : "", guest ? (unsigned long long)(addr - uintptr_t(g_base)) : 0ull);
    LogGuestRegisters();
    return EXCEPTION_CONTINUE_SEARCH;
}

void PlatformInstallCrashHandler() { ::AddVectoredExceptionHandler(1, CrashHandler); }

#endif // _WIN32
