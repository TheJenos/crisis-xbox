// File system: serves the title's disc (d:\, game:\, \Device\Cdrom0\). The disc is
// read-only, as on the console. It comes either from the ISO image, or from an
// extracted copy (a folder, or a URL in the browser) described by
// disc-index.txt; extracted files are opened only when the title first reads
// them.
//
// The ISO holds an XDVDFS volume: a volume descriptor 32 sectors into the game
// partition, and directories stored as binary trees of entries
//   { u16 left, u16 right (dword offsets), u32 sector, u32 size, u8 attributes, u8 nameLength, name }.
#include "guest.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "platform.h"

namespace
{
    constexpr uint64_t SECTOR = 2048;
    constexpr uint32_t ATTR_DIRECTORY = 0x10, ATTR_NORMAL = 0x80, ATTR_READONLY = 0x01;

    struct DiscEntry
    {
        std::string name;                   // original case
        uint64_t offset = 0;                // byte offset in the ISO
        uint32_t size = 0;
        bool isDir = false;
        std::vector<DiscEntry*> children;   // sorted case-insensitively
        std::string relPath;                // extracted disc: path under the disc root ('/' separators)
        PlatformFile* host = nullptr;       // extracted disc: opened on first read
    };

    PlatformFile* g_iso = nullptr;
    std::string g_discRoot;                               // extracted disc: folder or URL prefix ending in '/'
    std::mutex g_openMutex;
    uint64_t g_partition = 0;
    std::vector<std::unique_ptr<DiscEntry>> g_entries;
    DiscEntry* g_root = nullptr;
    std::unordered_map<std::string, DiscEntry*> g_byPath; // lowercase, '\'-separated, no leading slash
    int64_t g_discTime = 0;                               // timestamp reported for every disc file

    std::string Lower(std::string s)
    {
        for (char& c : s) c = char(tolower((unsigned char)c));
        return s;
    }

    bool ReadIso(uint64_t offset, void* dest, uint32_t size)
    {
        return PlatformReadAt(g_iso, offset, dest, size);
    }

    // URL-encode a disc path for the browser (keeps '/', letters, digits and "-._").
    std::string UrlEncode(const std::string& path)
    {
        std::string out;
        for (unsigned char c : path)
        {
            if (isalnum(c) || c == '/' || c == '-' || c == '.' || c == '_')
                out += char(c);
            else
            {
                char b[4];
                snprintf(b, sizeof(b), "%%%02X", c);
                out += b;
            }
        }
        return out;
    }

    // Read part of a disc file, from the ISO or from its extracted copy.
    bool ReadDiscEntry(DiscEntry* e, uint64_t pos, void* dest, uint32_t n)
    {
        // READ_DELAY_MS=<ms>: delay every disc read (debugging: imitate slow HTTP reads).
        static const uint32_t delay = getenv("READ_DELAY_MS") ? uint32_t(atoi(getenv("READ_DELAY_MS"))) : 0;
        if (delay)
            PlatformSleepMs(delay);
        if (g_discRoot.empty())
            return ReadIso(e->offset + pos, dest, n);
        {
            std::lock_guard lock(g_openMutex);
            if (!e->host)
            {
                bool url = g_discRoot.rfind("url:", 0) == 0 || g_discRoot.rfind("http", 0) == 0;
                std::string path = g_discRoot + (url ? UrlEncode(e->relPath) : e->relPath);
                if (!url)
                    std::replace(path.begin(), path.end(), '/', PlatformPathSeparator());
                e->host = PlatformOpenFile(path, OpenMode::Read);
                if (!e->host)
                {
                    Log("disc: cannot open extracted file %s", path.c_str());
                    return false;
                }
            }
        }
        return PlatformReadAt(e->host, pos, dest, n);
    }

    void ReadDirectory(DiscEntry* dir, const std::string& prefix)
    {
        std::vector<uint8_t> table(dir->size);
        if (dir->size == 0 || !ReadIso(dir->offset, table.data(), dir->size))
            return;
        std::vector<uint32_t> stack{ 0 };
        std::vector<bool> seen(table.size() / 4 + 1);
        while (!stack.empty())
        {
            uint32_t off = stack.back();
            stack.pop_back();
            if (off + 14 > table.size() || seen[off / 4])
                continue;
            seen[off / 4] = true;
            uint16_t left = table[off] | table[off + 1] << 8, right = table[off + 2] | table[off + 3] << 8;
            if (left == 0xFFFF)
                continue; // padding
            uint32_t sector, size;
            memcpy(&sector, &table[off + 4], 4);
            memcpy(&size, &table[off + 8], 4);
            uint8_t attr = table[off + 12], len = table[off + 13];
            if (off + 14 + len > table.size())
                continue;

            auto e = std::make_unique<DiscEntry>();
            e->name.assign((const char*)&table[off + 14], len);
            e->offset = g_partition + sector * SECTOR;
            e->size = size;
            e->isDir = attr & ATTR_DIRECTORY;
            std::string path = prefix + Lower(e->name);
            g_byPath[path] = e.get();
            dir->children.push_back(e.get());
            if (e->isDir)
                ReadDirectory(e.get(), path + "\\");
            g_entries.push_back(std::move(e));

            if (left) stack.push_back(left * 4u);
            if (right) stack.push_back(right * 4u);
        }
        std::sort(dir->children.begin(), dir->children.end(),
                  [](DiscEntry* a, DiscEntry* b) { return Lower(a->name) < Lower(b->name); });
    }

    // Mounted save containers (XamContentCreate): drive name -> directory
    // under the disc root, which only exists in the writable overlay.
    std::mutex g_mountMutex;
    std::map<std::string, std::string> g_mounts; // lowercase "name:" -> lowercase disc path

    // Map a guest path to a disc path (lowercase, relative to the disc root).
    // Returns false for devices this runtime doesn't serve.
    bool ToDiscPath(const std::string& guestPath, std::string& out)
    {
        std::string p = Lower(guestPath);
        std::replace(p.begin(), p.end(), '/', '\\');
        static const char* prefixes[] = { "\\??\\d:", "\\device\\cdrom0", "d:", "game:", "\\??\\game:" };
        bool matched = false;
        {
            std::lock_guard lock(g_mountMutex);
            std::string q = p.compare(0, 4, "\\??\\") == 0 ? p.substr(4) : p;
            for (const auto& [drive, dir] : g_mounts)
                if (q.compare(0, drive.size(), drive) == 0)
                {
                    p = "\\" + dir + "\\" + q.substr(drive.size()) + "\\";
                    matched = true;
                    break;
                }
        }
        for (const char* pre : prefixes)
            if (!matched && p.compare(0, strlen(pre), pre) == 0)
            {
                p = p.substr(strlen(pre));
                matched = true;
                break;
            }
        if (!matched)
            return false;
        // Collapse separators and resolve "." / "..".
        std::vector<std::string> parts;
        size_t i = 0;
        while (i <= p.size())
        {
            size_t j = p.find('\\', i);
            if (j == std::string::npos) j = p.size();
            std::string part = p.substr(i, j - i);
            if (part == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (!part.empty() && part != ".") parts.push_back(part);
            i = j + 1;
        }
        out.clear();
        for (size_t k = 0; k < parts.size(); k++)
            out += (k ? "\\" : "") + parts[k];
        return true;
    }

    DiscEntry* Lookup(const std::string& discPath)
    {
        if (discPath.empty())
            return g_root;
        auto it = g_byPath.find(discPath);
        return it == g_byPath.end() ? nullptr : it->second;
    }

    bool WildcardMatch(const char* pattern, const char* name)
    {
        if (*pattern == 0) return *name == 0;
        if (*pattern == '*') return WildcardMatch(pattern + 1, name) || (*name && WildcardMatch(pattern, name + 1));
        if (*name && (*pattern == '?' || tolower((unsigned char)*pattern) == tolower((unsigned char)*name)))
            return WildcardMatch(pattern + 1, name + 1);
        return false;
    }

    // Open files: the handle points at a permanently signaled notification
    // event (so waits on the file complete), mapped to this host-side state.
    struct OpenDiscFile
    {
        DiscEntry* entry;
        std::string path;
        uint64_t position = 0;
        size_t enumIndex = 0;
        std::string enumPattern; // NT keeps the first call's pattern for the whole enumeration
        // Overlay files: a host handle (none for directories) and the entry
        // describing them, owned here instead of by the disc index.
        PlatformFile* host = nullptr;
        std::shared_ptr<DiscEntry> hostEntry;
        std::vector<std::shared_ptr<DiscEntry>> hostChildren; // overlay directory listing
        // Saved-game files (under _content): `writing` from the first change
        // until the title closes its last handle (counted in g_savesWriting).
        bool content = false, writing = false;
    };
    std::mutex g_filesMutex;
    std::unordered_map<uint32_t, OpenDiscFile> g_files; // keyed by the guest file object

    // Saved games still being written. The title writes a checkpoint in pieces
    // over several seconds (a compressor thread appends 32 KB blocks, then the
    // table footer), so a copy taken in between is a truncated file it can't
    // load. The page (saves.js) asks crysis_saves_busy() before storing them.
    std::atomic<int> g_savesWriting{ 0 };
    std::atomic<uint32_t> g_savesChanges{ 0 }; // every write / resize / truncation of a save file

    // A saved-game file changed (written or resized).
    void NoteContentChange(OpenDiscFile* f)
    {
        if (!f->content)
            return;
        g_savesChanges++;
        std::lock_guard lock(g_filesMutex);
        if (!f->writing)
        {
            f->writing = true;
            g_savesWriting++;
        }
    }

    OpenDiscFile* FileFromHandle(uint32_t handle)
    {
        ObjectKind kind;
        uint32_t obj;
        if (!LookupHandle(handle, &kind, &obj) || kind != ObjectKind::File)
            return nullptr;
        std::lock_guard lock(g_filesMutex);
        auto it = g_files.find(obj);
        return it == g_files.end() ? nullptr : &it->second;
    }

    // OBJECT_ATTRIBUTES { u32 rootDirectory, ANSI_STRING* objectName, u32 attributes }
    std::string PathFromAttributes(uint32_t attrs)
    {
        uint32_t name = Load<uint32_t>(attrs + 4);
        std::string path = name ? LoadAnsiString(name) : std::string();
        uint32_t root = Load<uint32_t>(attrs);
        if (root && root != 0xFFFFFFFD) // relative to an open directory
            if (OpenDiscFile* dir = FileFromHandle(root))
                path = "d:\\" + dir->path + (dir->path.empty() ? "" : "\\") + path;
        return path;
    }

    // IO_STATUS_BLOCK { u32 status, u32 information }
    void SetIoStatus(uint32_t iosb, uint32_t status, uint32_t info)
    {
        if (!iosb) return;
        Store<uint32_t>(iosb, status);
        Store<uint32_t>(iosb + 4, info);
    }

    uint64_t AllocationSize(const DiscEntry* e) { return (uint64_t(e->size) + SECTOR - 1) & ~(SECTOR - 1); }
    uint32_t Attributes(const DiscEntry* e) { return e->isDir ? ATTR_DIRECTORY : (ATTR_NORMAL | ATTR_READONLY); }

    // FILE_NETWORK_OPEN_INFORMATION: 4 times, allocation size, end of file, attributes (56 bytes)
    void WriteNetworkOpenInfo(uint32_t info, const DiscEntry* e)
    {
        for (int i = 0; i < 4; i++) Store<int64_t>(info + i * 8, g_discTime);
        Store<uint64_t>(info + 32, AllocationSize(e));
        Store<uint64_t>(info + 40, e->size);
        Store<uint32_t>(info + 48, Attributes(e));
        Store<uint32_t>(info + 52, 0);
    }

    // Writable overlay: OVERLAY_DIR or "overlay" under the working directory.
    std::string OverlayRoot()
    {
        static const std::string root = getenv("OVERLAY_DIR") ? getenv("OVERLAY_DIR") : "overlay";
        return root;
    }
    std::string OverlayPath(const std::string& discPath)
    {
        std::string p = OverlayRoot() + (discPath.empty() ? "" : "\\" + discPath);
        std::replace(p.begin(), p.end(), '\\', PlatformPathSeparator());
        return p;
    }
    void CreateParentDirectories(const std::string& hostPath)
    {
        for (size_t i = 0; (i = hostPath.find(PlatformPathSeparator(), i + 1)) != std::string::npos;)
            PlatformMakeDirectory(hostPath.substr(0, i));
    }

    // Size of an open file, refreshed from the host for overlay files.
    uint64_t FileSize(OpenDiscFile* f)
    {
        if (f->host)
            f->entry->size = uint32_t(PlatformFileSize(f->host));
        return f->entry->size;
    }

    bool ReadFileData(OpenDiscFile* f, uint64_t pos, void* dest, uint32_t n)
    {
        if (!f->host)
            return ReadDiscEntry(f->entry, pos, dest, n);
        return PlatformReadAt(f->host, pos, dest, n);
    }

    // Dispositions: 0 supersede, 1 open, 2 create, 3 open-if, 4 overwrite, 5 overwrite-if.
    // options bit 0 (FILE_DIRECTORY_FILE) asks for a directory.
    uint32_t OpenCommon(uint32_t handleOut, uint32_t attrs, uint32_t iosb, uint32_t disposition, uint32_t options, const char* api)
    {
        std::string guestPath = PathFromAttributes(attrs);
        std::string discPath;
        if (handleOut) Store<uint32_t>(handleOut, 0);

        if (!ToDiscPath(guestPath, discPath))
        {
            Log("%s(\"%s\"): device not supported", api, guestPath.c_str());
            SetIoStatus(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        bool creates = disposition == 0 || disposition == 2 || disposition == 3 || disposition == 5;
        bool truncates = disposition == 0 || disposition == 4 || disposition == 5;
        std::string hostPath = OverlayPath(discPath);
        PathKind hostKind = PlatformPathKind(hostPath);
        DiscEntry* e = Lookup(discPath);
        // Overlay files shadow disc files, but a directory that exists on the disc
        // is served from the disc so it still lists the disc's contents.
        bool inOverlay = !discPath.empty() && hostKind != PathKind::Missing && !(e && e->isDir);
        if (inOverlay)
            e = nullptr;
        uint32_t information = 1; // FILE_OPENED

        if ((inOverlay || e) && disposition == 2)
        {
            SetIoStatus(iosb, 0xC0000035, 0); // STATUS_OBJECT_NAME_COLLISION
            return 0xC0000035;
        }
        if (e && !inOverlay && truncates)
        {
            SetIoStatus(iosb, STATUS_ACCESS_DENIED, 0); // the disc is read-only
            return STATUS_ACCESS_DENIED;
        }
        if (!e && !inOverlay)
        {
            if (!creates)
            {
                Log("%s(\"%s\") -> not found", api, guestPath.c_str());
                SetIoStatus(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            CreateParentDirectories(hostPath);
            if (options & 1)
                PlatformMakeDirectory(hostPath);
            hostKind = PlatformPathKind(hostPath);
            inOverlay = true;
            information = 2; // FILE_CREATED
        }

        OpenDiscFile file{ e, discPath };
        if (inOverlay)
        {
            auto entry = std::make_shared<DiscEntry>();
            entry->name = discPath.substr(discPath.find_last_of('\\') + 1);
            entry->isDir = hostKind == PathKind::Directory;
            if (!entry->isDir)
            {
                OpenMode mode = (information == 2 || truncates) ? OpenMode::TruncateReadWrite : OpenMode::ReadWrite;
                file.content = discPath.compare(0, 9, "_content\\") == 0;
                if (file.content && mode == OpenMode::TruncateReadWrite) // created or emptied: a save begins
                {
                    file.writing = true;
                    g_savesWriting++;
                    g_savesChanges++;
                }
                file.host = PlatformOpenFile(hostPath, mode);
                if (!file.host)
                {
                    if (file.writing)
                        g_savesWriting--;
                    Log("%s(\"%s\"): cannot open overlay file %s", api, guestPath.c_str(), hostPath.c_str());
                    SetIoStatus(iosb, STATUS_ACCESS_DENIED, 0);
                    return STATUS_ACCESS_DENIED;
                }
                if (truncates && information == 1)
                    information = 3; // FILE_OVERWRITTEN
            }
            if (entry->isDir)
            {
                // List the host directory so enumeration sees overlay files (saves).
                std::vector<PlatformDirEntry> list;
                PlatformListDirectory(hostPath, list);
                for (const auto& h : list)
                {
                    auto child = std::make_shared<DiscEntry>();
                    child->name = h.name;
                    child->isDir = h.isDir;
                    child->size = uint32_t(h.size);
                    file.hostChildren.push_back(child);
                    entry->children.push_back(child.get());
                }
                std::sort(entry->children.begin(), entry->children.end(),
                          [](DiscEntry* a, DiscEntry* b) { return Lower(a->name) < Lower(b->name); });
            }
            file.hostEntry = entry;
            file.entry = entry.get();
            if (!entry->isDir)
                FileSize(&file);
        }
        Log("%s(\"%s\") -> %s%s", api, guestPath.c_str(), file.entry->isDir ? "directory" : "file",
            inOverlay ? (information == 2 ? " (created in overlay)" : " (overlay)") : "");

        uint32_t obj = CreateDispatcherObject(16, 0, 1);
        {
            std::lock_guard lock(g_filesMutex);
            g_files[obj] = std::move(file);
        }
        uint32_t handle = CreateHandle(ObjectKind::File, obj);
        if (handleOut) Store<uint32_t>(handleOut, handle);
        SetIoStatus(iosb, STATUS_SUCCESS, information);
        return STATUS_SUCCESS;
    }
}

// Extracted disc: `root` is a folder (or a URL in the browser) holding the
// disc's files and disc-index.txt, one entry per line, parents first:
//   D<TAB><path>/            a directory
//   F<TAB><size><TAB><path>  a file
static bool MountExtracted(std::string root)
{
    const bool isUrl = root.rfind("url:", 0) == 0 || root.rfind("http", 0) == 0;
    const char last = root.empty() ? 0 : root.back();
    if (last != '/' && last != '\\')
        root += isUrl ? '/' : PlatformPathSeparator();
    PlatformFile* index = PlatformOpenFile(root + "disc-index.txt", OpenMode::Read);
    if (!index)
        return false;
    std::string text(size_t(PlatformFileSize(index)), char(0));
    bool ok = PlatformReadAt(index, 0, text.data(), uint32_t(text.size()));
    PlatformCloseFile(index);
    if (!ok)
        return false;

    g_discRoot = root;
    auto rootEntry = std::make_unique<DiscEntry>();
    rootEntry->isDir = true;
    g_root = rootEntry.get();
    g_entries.push_back(std::move(rootEntry));

    const char TAB = '\t', NL = '\n', CR = '\r';
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find(NL, pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == CR) line.pop_back();
        if (line.size() < 3 || line[1] != TAB) continue;
        auto e = std::make_unique<DiscEntry>();
        std::string rel;
        if (line[0] == 'D')
        {
            e->isDir = true;
            rel = line.substr(2);
        }
        else
        {
            size_t tab = line.find(TAB, 2);
            if (tab == std::string::npos) continue;
            e->size = uint32_t(strtoull(line.c_str() + 2, nullptr, 10));
            rel = line.substr(tab + 1);
        }
        while (!rel.empty() && rel.back() == '/') rel.pop_back();
        if (rel.empty()) continue;
        e->relPath = rel;
        size_t slash = rel.find_last_of('/');
        e->name = slash == std::string::npos ? rel : rel.substr(slash + 1);
        std::string key = Lower(rel);
        std::replace(key.begin(), key.end(), '/', '\\');
        DiscEntry* parent = slash == std::string::npos ? g_root : Lookup(key.substr(0, slash));
        if (!parent)
            continue;
        parent->children.push_back(e.get());
        g_byPath[key] = e.get();
        g_entries.push_back(std::move(e));
    }
    for (auto& e : g_entries)
        std::sort(e->children.begin(), e->children.end(),
                  [](DiscEntry* a, DiscEntry* b) { return Lower(a->name) < Lower(b->name); });
    g_discTime = 129452256000000000ll; // 2011-03-22, the game's release date
    Log("disc: mounted extracted disc at %s, %zu entries (files are opened on first use)", root.c_str(), g_byPath.size());
    return true;
}

bool DiscMount(const char* isoPath)
{
    // A folder, or a URL ending in '/', is an extracted disc.
    std::string p = isoPath;
    bool isUrl = p.rfind("url:", 0) == 0 || p.rfind("http", 0) == 0;
    if ((isUrl && p.back() == '/') || (!isUrl && PlatformPathKind(p) == PathKind::Directory))
        return MountExtracted(p);
    g_iso = PlatformOpenFile(isoPath, OpenMode::Read);
    if (!g_iso)
        return false;
    for (uint64_t off : { 0xFD90000ull, 0x2080000ull, 0x18300000ull, 0ull }) // XGD2, XGD3, XGD1, plain
    {
        char magic[20];
        if (ReadIso(off + 32 * SECTOR, magic, 20) && memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0)
        {
            g_partition = off;
            uint32_t rootSector, rootSize;
            ReadIso(off + 32 * SECTOR + 20, &rootSector, 4);
            ReadIso(off + 32 * SECTOR + 24, &rootSize, 4);
            auto root = std::make_unique<DiscEntry>();
            root->isDir = true;
            root->offset = off + rootSector * SECTOR;
            root->size = rootSize;
            g_root = root.get();
            g_entries.push_back(std::move(root));
            ReadDirectory(g_root, "");

            g_discTime = 129452256000000000ll; // disc files report the game's release date, 2011-03-22
            Log("disc: mounted %s (partition at %llX), %zu entries", isoPath, (unsigned long long)off, g_byPath.size());
            return true;
        }
    }
    return false;
}

bool DiscReadFile(const std::string& path, std::vector<uint8_t>& out)
{
    DiscEntry* e = Lookup(Lower(path));
    if (!e || e->isDir)
        return false;
    out.resize(e->size);
    return ReadDiscEntry(e, 0, out.data(), e->size);
}

// ---------------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------------
// NTSTATUS NtCreateFile(u32* handle, u32 access, OBJECT_ATTRIBUTES* attrs, IO_STATUS_BLOCK* iosb,
//                       i64* allocationSize, u32 fileAttributes, u32 shareAccess, u32 disposition, u32 options)
GUEST_IMPORT(NtCreateFile)
{
    uint32_t options = StackArg32(ctx, 8); // 9th argument, on the stack
    ctx.r3.u64 = OpenCommon(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, options, "NtCreateFile");
}

// NTSTATUS NtOpenFile(u32* handle, u32 access, OBJECT_ATTRIBUTES* attrs, IO_STATUS_BLOCK* iosb, u32 share, u32 options)
GUEST_IMPORT(NtOpenFile)
{
    ctx.r3.u64 = OpenCommon(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32, "NtOpenFile");
}

// NTSTATUS NtReadFile(u32 handle, u32 event, u32 apcRoutine, u32 apcContext, IO_STATUS_BLOCK* iosb,
//                     void* buffer, u32 length, i64* byteOffset)
GUEST_IMPORT(NtReadFile)
{
    uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, iosb = ctx.r7.u32;
    uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    OpenDiscFile* f = FileFromHandle(handle);
    if (!f || f->entry->isDir)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    if (apc)
        Log("NtReadFile: APC completion routine %08X not delivered (not implemented)", apc);

    // A byte offset of -2 (FILE_USE_FILE_POINTER_POSITION) means "current position".
    int64_t offset = offsetPtr ? Load<int64_t>(offsetPtr) : -2;
    uint64_t pos = offset == -2 ? f->position : uint64_t(offset);
    uint32_t status = STATUS_SUCCESS, got = 0;
    if (pos >= FileSize(f))
        status = STATUS_END_OF_FILE;
    else
    {
        got = uint32_t(std::min<uint64_t>(length, f->entry->size - pos));
        if (!ReadFileData(f, pos, GuestPtr(buffer), got))
        {
            Log("NtReadFile: disc read failed (%s @ %llX)", f->path.c_str(), (unsigned long long)pos);
            status = STATUS_UNSUCCESSFUL;
            got = 0;
        }
    }
    f->position = pos + got;
    static std::atomic<int> logged{ 0 };
    static const bool readTrace = getenv("READ_TRACE") != nullptr; // every read (debugging)
    if (readTrace || logged++ < 200)
        Log("NtReadFile(%s @ %llX, %X bytes) -> %X, %X read", f->path.c_str(), (unsigned long long)pos, length, status, got);
    SetIoStatus(iosb, status, got);
    if (event)
    {
        ObjectKind kind;
        uint32_t obj;
        if (LookupHandle(event, &kind, &obj))
            SignalEventObject(obj);
    }
    ctx.r3.u64 = status;
}

// NTSTATUS NtReadFileScatter(u32 handle, u32 event, u32 apcRoutine, u32 apcContext, IO_STATUS_BLOCK* iosb,
//                            u32* segments, u32 length, i64* byteOffset): one 4 KB page per segment
GUEST_IMPORT(NtReadFileScatter)
{
    uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, iosb = ctx.r7.u32;
    uint32_t segments = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    OpenDiscFile* f = FileFromHandle(handle);
    if (!f || f->entry->isDir)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    int64_t offset = offsetPtr ? Load<int64_t>(offsetPtr) : -2;
    uint64_t pos = offset == -2 ? f->position : uint64_t(offset);
    uint32_t total = 0, status = STATUS_SUCCESS;
    if (pos >= FileSize(f))
        status = STATUS_END_OF_FILE;
    for (uint32_t i = 0; status == STATUS_SUCCESS && total < length && pos + total < f->entry->size; i++)
    {
        uint32_t page = Load<uint32_t>(segments + i * 8) & ~0xFFFu; // FILE_SEGMENT_ELEMENT is 64-bit
        uint32_t chunk = uint32_t(std::min<uint64_t>({ uint64_t(0x1000), uint64_t(length - total), f->entry->size - pos - total }));
        if (!ReadFileData(f, pos + total, GuestPtr(page), chunk))
            status = STATUS_UNSUCCESSFUL;
        total += chunk;
    }
    f->position = pos + total;
    SetIoStatus(iosb, status, total);
    if (event)
    {
        ObjectKind kind;
        uint32_t obj;
        if (LookupHandle(event, &kind, &obj))
            SignalEventObject(obj);
    }
    ctx.r3.u64 = status;
}

// NTSTATUS NtWriteFile(u32 handle, u32 event, u32 apcRoutine, u32 apcContext, IO_STATUS_BLOCK* iosb,
//                      void* buffer, u32 length, i64* byteOffset)
// Only overlay files are writable; byte offset -1 appends, -2 uses the file position.
GUEST_IMPORT(NtWriteFile)
{
    uint32_t event = ctx.r4.u32, iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    OpenDiscFile* f = FileFromHandle(ctx.r3.u32);
    if (!f || !f->host)
    {
        SetIoStatus(iosb, f ? STATUS_ACCESS_DENIED : STATUS_INVALID_HANDLE, 0);
        ctx.r3.u64 = f ? STATUS_ACCESS_DENIED : STATUS_INVALID_HANDLE;
        return;
    }
    int64_t offset = offsetPtr ? Load<int64_t>(offsetPtr) : -2;
    uint64_t pos = offset == -1 ? FileSize(f) : offset == -2 ? f->position : uint64_t(offset);
    NoteContentChange(f); // (before the data lands, so the page never stores a half-written save)
    bool ok = PlatformWriteAt(f->host, pos, GuestPtr(buffer), length);
    uint32_t written = ok ? length : 0;
    uint32_t status = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    f->position = pos + written;
    static std::atomic<int> logged{ 0 };
    static const bool writeTrace = getenv("WRITE_TRACE") != nullptr; // every write (debugging)
    if (writeTrace || logged++ < 5)
        Log("NtWriteFile(%s @ %llX, %X bytes) -> %X", f->path.c_str(), (unsigned long long)pos, length, status);
    SetIoStatus(iosb, status, written);
    if (event)
    {
        ObjectKind kind;
        uint32_t obj;
        if (LookupHandle(event, &kind, &obj))
            SignalEventObject(obj);
    }
    ctx.r3.u64 = status;
}
GUEST_IMPORT(NtWriteFileGather)
{
    SetIoStatus(ctx.r7.u32, STATUS_ACCESS_DENIED, 0);
    ctx.r3.u64 = STATUS_ACCESS_DENIED;
}
GUEST_IMPORT(NtFlushBuffersFile) { ctx.r3.u64 = STATUS_SUCCESS; }

// NTSTATUS NtQueryInformationFile(u32 handle, IO_STATUS_BLOCK* iosb, void* info, u32 length, u32 class)
GUEST_IMPORT(NtQueryInformationFile)
{
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, cls = ctx.r7.u32;
    OpenDiscFile* f = FileFromHandle(ctx.r3.u32);
    if (!f)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    FileSize(f);
    const DiscEntry* e = f->entry;
    uint32_t written = 0;
    memset(GuestPtr(info), 0, length);
    switch (cls)
    {
    case 4: // FileBasicInformation: 4 times, attributes
        for (int i = 0; i < 4; i++) Store<int64_t>(info + i * 8, g_discTime);
        Store<uint32_t>(info + 32, Attributes(e));
        written = 40;
        break;
    case 5: // FileStandardInformation: allocation size, end of file, links, delete pending, directory
        Store<uint64_t>(info + 0, AllocationSize(e));
        Store<uint64_t>(info + 8, e->size);
        Store<uint32_t>(info + 16, 1);
        Store<uint8_t>(info + 21, e->isDir);
        written = 24;
        break;
    case 6: // FileInternalInformation: a unique file index (its disc offset)
        Store<uint64_t>(info, e->offset);
        written = 8;
        break;
    case 14: // FilePositionInformation
        Store<uint64_t>(info, f->position);
        written = 8;
        break;
    case 26: // FileSectorInformation (Xbox): first disc sector of the file
        Store<uint32_t>(info, uint32_t(e->offset / SECTOR));
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInfo(info, e);
        written = 56;
        break;
    default:
        Log("NtQueryInformationFile(%s): class %u not implemented", f->path.c_str(), cls);
        SetIoStatus(iosb, STATUS_NOT_IMPLEMENTED, 0);
        ctx.r3.u64 = STATUS_NOT_IMPLEMENTED;
        return;
    }
    SetIoStatus(iosb, STATUS_SUCCESS, written);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtSetInformationFile(u32 handle, IO_STATUS_BLOCK* iosb, void* info, u32 length, u32 class)
GUEST_IMPORT(NtSetInformationFile)
{
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    OpenDiscFile* f = FileFromHandle(ctx.r3.u32);
    if (!f)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    if (cls == 14) // FilePositionInformation
    {
        f->position = Load<uint64_t>(info);
        SetIoStatus(iosb, STATUS_SUCCESS, 0);
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    }
    if (f->host || f->hostEntry)
    {
        if ((cls == 19 || cls == 20) && f->host) // allocation / end of file
        {
            NoteContentChange(f);
            PlatformSetFileSize(f->host, uint64_t(Load<int64_t>(info)));
        }
        SetIoStatus(iosb, STATUS_SUCCESS, 0); // other classes (times, delete-on-close) are accepted
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    }
    Log("NtSetInformationFile(%s): class %u refused on read-only disc", f->path.c_str(), cls);
    SetIoStatus(iosb, STATUS_ACCESS_DENIED, 0);
    ctx.r3.u64 = STATUS_ACCESS_DENIED;
}

// NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES* attrs, FILE_NETWORK_OPEN_INFORMATION* info)
GUEST_IMPORT(NtQueryFullAttributesFile)
{
    std::string discPath;
    DiscEntry* e = ToDiscPath(PathFromAttributes(ctx.r3.u32), discPath) ? Lookup(discPath) : nullptr;
    if (!e)
    {
        ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    WriteNetworkOpenInfo(ctx.r4.u32, e);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryVolumeInformationFile(u32 handle, IO_STATUS_BLOCK* iosb, void* info, u32 length, u32 class)
GUEST_IMPORT(NtQueryVolumeInformationFile)
{
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, cls = ctx.r7.u32;
    if (!FileFromHandle(ctx.r3.u32))
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    memset(GuestPtr(info), 0, length);
    uint32_t written = 0;
    switch (cls)
    {
    case 1: // FileFsVolumeInformation: creation time, serial, label length, supports objects, label
        Store<int64_t>(info, g_discTime);
        Store<uint32_t>(info + 8, 0x12345678);
        written = 18;
        break;
    case 3: // FileFsSizeInformation: total units, available units, sectors per unit, bytes per sector
    {
        uint64_t sectors = 0;
        sectors = PlatformFileSize(g_iso) / SECTOR;
        Store<uint64_t>(info + 0, sectors);
        Store<uint64_t>(info + 8, 0);
        Store<uint32_t>(info + 16, 1);
        Store<uint32_t>(info + 20, uint32_t(SECTOR));
        written = 24;
        break;
    }
    case 5: // FileFsAttributeInformation: attributes, max name length, name length, name (UTF-16)
    {
        const char* fs = "XDVDFS";
        Store<uint32_t>(info + 0, 0);
        Store<uint32_t>(info + 4, 255);
        Store<uint32_t>(info + 8, uint32_t(strlen(fs) * 2));
        for (size_t i = 0; i < strlen(fs) && 12 + i * 2 + 2 <= length; i++)
            Store<uint16_t>(info + 12 + uint32_t(i) * 2, uint16_t(fs[i]));
        written = 12 + uint32_t(strlen(fs) * 2);
        break;
    }
    default:
        Log("NtQueryVolumeInformationFile: class %u not implemented", cls);
        SetIoStatus(iosb, STATUS_NOT_IMPLEMENTED, 0);
        ctx.r3.u64 = STATUS_NOT_IMPLEMENTED;
        return;
    }
    SetIoStatus(iosb, STATUS_SUCCESS, written);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryDirectoryFile(u32 handle, u32 event, u32 apcRoutine, u32 apcContext, IO_STATUS_BLOCK* iosb,
//                               FILE_DIRECTORY_INFORMATION* info, u32 length, ANSI_STRING* pattern, u32 restartScan)
// Returns one entry per call:
//   { u32 next, u32 index, i64 times[4], i64 endOfFile, i64 allocationSize, u32 attributes, u32 nameLength, char name[] }
GUEST_IMPORT(NtQueryDirectoryFile)
{
    uint32_t iosb = ctx.r7.u32, info = ctx.r8.u32, length = ctx.r9.u32, patternPtr = ctx.r10.u32;
    bool restart = StackArg32(ctx, 8) != 0; // 9th argument, on the stack
    OpenDiscFile* f = FileFromHandle(ctx.r3.u32);
    if (!f || !f->entry->isDir)
    {
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    // The pattern applies from the first call (or a restart) on; later calls
    // may pass none and continue with the same filter.
    if (restart || f->enumIndex == 0 || f->enumPattern.empty())
    {
        std::string given = patternPtr ? LoadAnsiString(patternPtr) : "";
        if (!given.empty() || f->enumPattern.empty())
            f->enumPattern = given.empty() ? "*" : given;
    }
    const std::string& pattern = f->enumPattern;
    if (restart)
        f->enumIndex = 0;

    const auto& children = f->entry->children;
    while (f->enumIndex < children.size() && !WildcardMatch(pattern.c_str(), children[f->enumIndex]->name.c_str()))
        f->enumIndex++;
    if (f->enumIndex >= children.size())
    {
        uint32_t status = STATUS_NO_MORE_FILES;
        SetIoStatus(iosb, status, 0);
        ctx.r3.u64 = status;
        return;
    }
    const DiscEntry* e = children[f->enumIndex];
    uint32_t needed = 64 + uint32_t(e->name.size());
    if (needed > length)
    {
        SetIoStatus(iosb, 0x80000005, 0); // STATUS_BUFFER_OVERFLOW
        ctx.r3.u64 = 0x80000005;
        return;
    }
    f->enumIndex++;
    memset(GuestPtr(info), 0, needed);
    Store<uint32_t>(info + 0, 0);
    Store<uint32_t>(info + 4, uint32_t(f->enumIndex));
    for (int i = 0; i < 4; i++) Store<int64_t>(info + 8 + i * 8, g_discTime);
    Store<uint64_t>(info + 40, e->size);
    Store<uint64_t>(info + 48, AllocationSize(e));
    Store<uint32_t>(info + 56, Attributes(e));
    Store<uint32_t>(info + 60, uint32_t(e->name.size()));
    memcpy(GuestPtr(info + 64), e->name.data(), e->name.size());
    SetIoStatus(iosb, STATUS_SUCCESS, needed);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// Save containers (used by XAM content functions).
void FsMountContent(const std::string& drive, const std::string& discDir)
{
    std::lock_guard lock(g_mountMutex);
    g_mounts[Lower(drive) + ":"] = Lower(discDir);
    Log("fs: mounted %s: at overlay %s", drive.c_str(), discDir.c_str());
}

void FsUnmountContent(const std::string& drive)
{
    std::lock_guard lock(g_mountMutex);
    g_mounts.erase(Lower(drive) + ":");
}

std::string FsOverlayHostPath(const std::string& discPath) { return OverlayPath(Lower(discPath)); }

// The title closed its last handle to a file object (objects.cpp CloseHandle).
// A saved-game file it changed is complete now. (The entry stays: another
// thread may still be inside a call that looked it up.)
void FsFileClosed(uint32_t object)
{
    static const bool writeTrace = getenv("WRITE_TRACE") != nullptr;
    std::lock_guard lock(g_filesMutex);
    auto it = g_files.find(object);
    if (it == g_files.end() || !it->second.writing)
        return;
    it->second.writing = false;
    g_savesWriting--;
    if (writeTrace)
        Log("fs: %s closed (%llu bytes)", it->second.path.c_str(),
            (unsigned long long)(it->second.host ? PlatformFileSize(it->second.host) : 0));
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// For the page's saved-game store (web-src/saves.js): how many save files the
// title is writing right now, and a counter of changes to them. A save is safe
// to copy when nothing is being written and the counter didn't move meanwhile.
extern "C" EMSCRIPTEN_KEEPALIVE int crysis_saves_writing() { return g_savesWriting.load(); }
extern "C" EMSCRIPTEN_KEEPALIVE double crysis_saves_changes() { return double(g_savesChanges.load()); }
#endif
