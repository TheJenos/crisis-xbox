// Host platform services used by the runtime. Implemented by
// platform_win32.cpp (native Windows) and platform_web.cpp (WebAssembly).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Guest address space. Guest addresses 0x40000000-0xBFFFFFFF are backed (the
// 0xC0000000 and 0xE0000000 physical views fold onto 0xA0000000, see
// ppc_fold_view). *base is chosen so that base + guestAddress is the host
// address; addresses below 0x40000000 are never valid.
// ---------------------------------------------------------------------------
constexpr uint32_t GUEST_LOW = 0x40000000, GUEST_HIGH = 0xC0000000;
bool PlatformReserveGuest(uint8_t** base);
bool PlatformCommit(uint8_t* p, size_t n);    // make a range usable (new pages read as zero)
void PlatformDecommit(uint8_t* p, size_t n);  // the range reads as zero afterwards

// ---------------------------------------------------------------------------
// Threads and time
// ---------------------------------------------------------------------------
// Start a host thread with at least `stackSize` bytes of stack.
bool PlatformStartThread(std::function<void()> body, size_t stackSize);
void PlatformSleepMs(uint32_t ms);
void PlatformSleepMsPrecise(uint32_t ms); // for the title's own sleeps: no timer-tick rounding

// Debug dumps: the browser PUTs the bytes to the local server (saved when it
// runs with DUMP_DIR); native writes the file into the working directory.
void PlatformUploadDump(const char* name, const void* data, size_t size);
void PlatformYield();
uint64_t PlatformCounterNs();      // monotonic
int64_t PlatformFileTimeNow();     // 100 ns units since 1601-01-01 UTC

// ---------------------------------------------------------------------------
// Host files: the disc image and the writable overlay.
// ---------------------------------------------------------------------------
struct PlatformFile;
enum class OpenMode { Read, ReadWrite, CreateReadWrite, TruncateReadWrite };
PlatformFile* PlatformOpenFile(const std::string& path, OpenMode mode);
bool PlatformReadAt(PlatformFile* f, uint64_t offset, void* dest, uint32_t size); // all-or-nothing
bool PlatformWriteAt(PlatformFile* f, uint64_t offset, const void* src, uint32_t size);
uint64_t PlatformFileSize(PlatformFile* f);
void PlatformCloseFile(PlatformFile* f);
bool PlatformSetFileSize(PlatformFile* f, uint64_t size);

enum class PathKind { Missing, File, Directory };
PathKind PlatformPathKind(const std::string& path);
bool PlatformMakeDirectory(const std::string& path);
char PlatformPathSeparator();
struct PlatformDirEntry { std::string name; bool isDir; uint64_t size; };
bool PlatformListDirectory(const std::string& path, std::vector<PlatformDirEntry>& out); // without "." and ".."
bool PlatformRemoveFile(const std::string& path);

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
[[noreturn]] void PlatformExit(int code);
void PlatformInstallCrashHandler(); // logs host faults with guest context where supported
