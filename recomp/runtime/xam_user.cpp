// XAM: notifications, the signed-in user, controllers, profile settings and
// system UI, emulating an offline console with one local profile.
//
// Behaviour follows Xenia's XAM (src/xenia/kernel/xam): titles wait for the
// startup notifications (sign-in changed, UI on/off, input devices changed)
// before leaving their first screen, and system UI calls complete
// immediately with a default answer.
#include "guest.h"
#include "input.h"
#include "platform.h"

#include <deque>
#include <map>
#include <mutex>
#include <vector>

void FsMountContent(const std::string& drive, const std::string& discDir);
void FsUnmountContent(const std::string& drive);
std::string FsOverlayHostPath(const std::string& discPath);

namespace
{
    // Win32-style XAM error codes.
    constexpr uint32_t X_ERROR_SUCCESS = 0, X_ERROR_INVALID_PARAMETER = 0x57, X_ERROR_INSUFFICIENT_BUFFER = 0x7A,
                       X_ERROR_IO_PENDING = 0x3E5, X_ERROR_DEVICE_NOT_CONNECTED = 0x48F, X_ERROR_CANCELLED = 0x4C7,
                       X_ERROR_NO_SUCH_USER = 0x525, X_ERROR_EMPTY = 0x10D2;
    constexpr uint32_t X_E_SUCCESS = 0, X_E_INVALIDARG = 0x80070057, X_E_NO_SUCH_USER = 0x80070525;

    constexpr uint64_t kXuid = 0xE000000012345678ull; // offline profile
    const char* kGamertag = "Player";

    // XAM_OVERLAPPED: result, length, context, event, completion routine,
    // completion context, extended error.
    uint32_t CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t length = 0)
    {
        if (!ov)
            return result;
        Store<uint32_t>(ov + 0x00, result);
        Store<uint32_t>(ov + 0x04, length);
        Store<uint32_t>(ov + 0x18, result);
        uint32_t event = Load<uint32_t>(ov + 0x0C);
        ObjectKind kind;
        uint32_t obj;
        if (event && LookupHandle(event, &kind, &obj) && kind == ObjectKind::Event)
            SignalEventObject(obj);
        return X_ERROR_IO_PENDING;
    }

    // ---------------------------------------------------------------------
    // Notifications
    // ---------------------------------------------------------------------
    struct Listener
    {
        uint32_t handle, object;
        uint64_t mask;
        std::deque<std::pair<uint32_t, uint32_t>> queue;
    };
    std::mutex g_notifyMutex;
    std::vector<Listener> g_listeners;
    bool g_startupSent = false;

    // Notification ids carry their area in bits 16-24 (area 0 = system = mask bit 0).
    void Broadcast(uint32_t id, uint32_t param)
    {
        std::lock_guard lock(g_notifyMutex);
        uint32_t area = (id >> 16) & 0x1FF;
        for (auto& l : g_listeners)
            if ((l.mask >> area) & 1 || (area == 0 && (l.mask & 1)))
            {
                l.queue.push_back({ id, param });
                SignalEventObject(l.object);
            }
    }
}

// u32 XamNotifyCreateListener(u64 mask, u32 maxVersion)
GUEST_IMPORT(XamNotifyCreateListener)
{
    uint64_t mask = ctx.r3.u64;
    uint32_t object = CreateDispatcherObject(16, 0, 0); // notification event, set while non-empty
    uint32_t handle = CreateHandle(ObjectKind::Event, object);
    {
        std::lock_guard lock(g_notifyMutex);
        g_listeners.push_back({ handle, object, mask, {} });
        Listener& l = g_listeners.back();
        if (!g_startupSent && (mask & 1))
        {
            g_startupSent = true;
            const std::pair<uint32_t, uint32_t> startup[] = {
                { 0x09, 1 }, { 0x09, 0 },  // XN_SYS_UI on, off
                { 0x0A, 1 }, { 0x0A, 1 },  // XN_SYS_SIGNINCHANGED: user 0
                { 0x12, 0 }, { 0x12, 0 },  // XN_SYS_INPUTDEVICESCHANGED
                { 0x13, 0 }, { 0x13, 0 },  // XN_SYS_INPUTDEVICECONFIGCHANGED
            };
            for (auto& n : startup) l.queue.push_back(n);
            SignalEventObject(object);
        }
    }
    Log("XAM: notification listener %08X (mask %llX)", handle, (unsigned long long)mask);
    ctx.r3.u64 = handle;
}

// BOOL XNotifyGetNext(u32 handle, u32 matchId, u32* id, u32* param)
GUEST_IMPORT(XNotifyGetNext)
{
    uint32_t handle = ctx.r3.u32, match = ctx.r4.u32, idPtr = ctx.r5.u32, paramPtr = ctx.r6.u32;
    std::lock_guard lock(g_notifyMutex);
    ctx.r3.u64 = 0;
    for (auto& l : g_listeners)
    {
        if (l.handle != handle)
            continue;
        for (auto it = l.queue.begin(); it != l.queue.end(); ++it)
        {
            if (match && it->first != match)
                continue;
            if (idPtr) Store<uint32_t>(idPtr, it->first);
            if (paramPtr) Store<uint32_t>(paramPtr, it->second);
            static int logged = 0;
            if (logged++ < 20)
                Log("XAM: notification %08X (%08X) delivered", it->first, it->second);
            l.queue.erase(it);
            ctx.r3.u64 = 1;
            break;
        }
        if (l.queue.empty())
            Store<int32_t>(l.object + 4, 0); // reset the listener's event
        break;
    }
}

GUEST_IMPORT(XNotifyPositionUI) {}
GUEST_IMPORT(XNotifyDelayUI) { ctx.r3.u64 = 0; }

// ---------------------------------------------------------------------------
// Users: user 0 is signed in locally.
// ---------------------------------------------------------------------------
GUEST_IMPORT(XamUserGetSigninState) { ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0; }

// HRESULT XamUserGetXUID(u32 user, u32 typeMask, u64* xuid)
GUEST_IMPORT(XamUserGetXUID)
{
    uint32_t user = ctx.r3.u32, ptr = ctx.r5.u32;
    if (!ptr) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    Store<uint64_t>(ptr, user == 0 ? kXuid : 0);
    ctx.r3.u64 = user == 0 ? X_E_SUCCESS : user < 4 ? X_E_NO_SUCH_USER : X_E_INVALIDARG;
}

// HRESULT XamUserGetSigninInfo(u32 user, u32 flags, X_USER_SIGNIN_INFO* info): xuid, unk, state, unk, unk, name[16]
GUEST_IMPORT(XamUserGetSigninInfo)
{
    uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
    if (!info) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    memset(GuestPtr(info), 0, 40);
    if (user != 0) { ctx.r3.u64 = X_E_NO_SUCH_USER; return; }
    Store<uint64_t>(info, kXuid);
    Store<uint32_t>(info + 12, 1);
    memcpy(GuestPtr(info + 24), kGamertag, strlen(kGamertag));
    ctx.r3.u64 = X_E_SUCCESS;
}

// u32 XamUserGetName(u32 user, char* buffer, u32 length)
GUEST_IMPORT(XamUserGetName)
{
    uint32_t user = ctx.r3.u32, buf = ctx.r4.u32, len = ctx.r5.u32;
    if (user >= 4) { ctx.r3.u64 = X_ERROR_INVALID_PARAMETER; return; }
    if (user != 0) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    uint32_t n = std::min<uint32_t>(len, 16);
    if (buf && n)
    {
        memset(GuestPtr(buf), 0, n);
        memcpy(GuestPtr(buf), kGamertag, std::min<size_t>(strlen(kGamertag), n - 1));
    }
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// u32 XamUserCheckPrivilege(u32 user, u32 mask, u32* result): deny online privileges
GUEST_IMPORT(XamUserCheckPrivilege)
{
    uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    if (user != 0xFF && user >= 4) { ctx.r3.u64 = X_ERROR_INVALID_PARAMETER; return; }
    if (user != 0xFF && user != 0) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (out) Store<uint32_t>(out, 0);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

GUEST_IMPORT(XamUserAreUsersFriends)
{
    // (user, xuids, count, result*, overlapped)
    if (ctx.r6.u32) Store<uint32_t>(ctx.r6.u32, 0);
    ctx.r3.u64 = CompleteOverlapped(ctx.r7.u32, X_ERROR_SUCCESS);
}

// HRESULT XamUserGetDeviceContext(u32 user, u32 unk, u32* out)
GUEST_IMPORT(XamUserGetDeviceContext)
{
    if (ctx.r5.u32) Store<uint32_t>(ctx.r5.u32, 0);
    uint32_t user = ctx.r3.u32;
    ctx.r3.u64 = (user == 0 || (user & 0xFF) == 0xFF) ? X_E_SUCCESS : 0x8007048F;
}

// u32 XamUserReadProfileSettings(titleId, user, xuidCount, xuids*, settingCount, settingIds*,
//                                bufferSize*, buffer, overlapped): every setting reads as "not set".
GUEST_IMPORT(XamUserReadProfileSettings)
{
    uint32_t user = ctx.r4.u32, xuidCount = ctx.r5.u32, count = ctx.r7.u32, ids = ctx.r8.u32;
    uint32_t sizePtr = ctx.r9.u32, buffer = ctx.r10.u32, ov = StackArg32(ctx, 8);
    if (count < 1 || count > 32 || !sizePtr) { ctx.r3.u64 = X_ERROR_INVALID_PARAMETER; return; }
    uint32_t users = xuidCount ? xuidCount : 1;
    uint32_t dataSize = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = Load<uint32_t>(ids + i * 4);
        uint32_t type = (id >> 28) & 0xF, size = (id >> 16) & 0xFFF;
        if (type == 4 || type == 6) // wide string, binary
            dataSize += size;
    }
    uint32_t needed = 8 + users * (count * 40 + dataSize);
    uint32_t have = Load<uint32_t>(sizePtr);
    if (!buffer || have < needed)
    {
        Store<uint32_t>(sizePtr, needed);
        ctx.r3.u64 = X_ERROR_INSUFFICIENT_BUFFER;
        return;
    }
    if (!xuidCount && user != 0)
    {
        ctx.r3.u64 = CompleteOverlapped(ov, X_ERROR_NO_SUCH_USER);
        return;
    }
    memset(GuestPtr(buffer), 0, needed);
    Store<uint32_t>(buffer, count * users);
    Store<uint32_t>(buffer + 4, buffer + 8);
    for (uint32_t u = 0; u < users; u++)
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t s = buffer + 8 + (u * count + i) * 40;
            Store<uint32_t>(s + 0, 0); // from: not set
            if (xuidCount)
                Store<uint64_t>(s + 8, Load<uint64_t>(ctx.r6.u32 + u * 8));
            else
                Store<uint32_t>(s + 8, user);
            Store<uint32_t>(s + 16, Load<uint32_t>(ids + i * 4));
        }
    static int logged = 0;
    if (logged++ < 10)
        Log("XAM: XamUserReadProfileSettings(%u settings, first %08X) -> defaults", count, Load<uint32_t>(ids));
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// u32 XamUserWriteProfileSettings(titleId, user, count, settings*, overlapped)
GUEST_IMPORT(XamUserWriteProfileSettings)
{
    uint32_t ov = ctx.r7.u32;
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// System UI: answered immediately.
// ---------------------------------------------------------------------------
// u32 XamShowSigninUI(u32 panes, u32 flags)
GUEST_IMPORT(XamShowSigninUI)
{
    Broadcast(0x0A, 1); // signed in
    Broadcast(0x09, 0); // UI closed
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// u32 XamShowDeviceSelectorUI(user, contentType, contentFlags, u64 totalRequested, u32* deviceId, overlapped)
GUEST_IMPORT(XamShowDeviceSelectorUI)
{
    uint32_t deviceIdPtr = ctx.r7.u32, ov = ctx.r8.u32;
    if (deviceIdPtr) Store<uint32_t>(deviceIdPtr, 1); // the hard drive
    Broadcast(0x09, 1);
    Broadcast(0x09, 0);
    Log("XAM: device selector -> hard drive");
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// u32 XamShowMessageBoxUI(user, title, text, buttonCount, buttons, activeButton, flags, u32* result, overlapped)
GUEST_IMPORT(XamShowMessageBoxUI)
{
    uint32_t active = ctx.r8.u32, resultPtr = ctx.r10.u32, ov = StackArg32(ctx, 8);
    Log("XAM: message box \"%s\" -> button %u", LoadGuestString(ctx.r4.u32 ? ctx.r4.u32 : ctx.r5.u32, 64).c_str(), active);
    if (resultPtr) Store<uint32_t>(resultPtr, active);
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// u32 XamShowMessageBoxUIEx(user, title, text, buttonCount, buttons, activeButton, flags, unk, u32* result, overlapped)
GUEST_IMPORT(XamShowMessageBoxUIEx)
{
    uint32_t active = ctx.r8.u32, resultPtr = StackArg32(ctx, 8), ov = StackArg32(ctx, 9);
    if (resultPtr) Store<uint32_t>(resultPtr, active);
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// u32 XamShowKeyboardUI(user, flags, default, title, description, buffer, length, overlapped): cancelled
GUEST_IMPORT(XamShowKeyboardUI)
{
    uint32_t ov = ctx.r10.u32;
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, X_ERROR_CANCELLED) : X_ERROR_CANCELLED;
}

// ---------------------------------------------------------------------------
// Controllers: port 0 is a connected gamepad fed by the host (input.cpp).
// ---------------------------------------------------------------------------
// u32 XamInputGetCapabilities(u32 user, u32 flags, X_INPUT_CAPABILITIES* caps)
GUEST_IMPORT(XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, caps = ctx.r5.u32;
    if (!caps) { ctx.r3.u64 = 0xA0; return; } // ERROR_BAD_ARGUMENTS
    if ((user & 0xFF) == 0xFF) user = 0;
    if (user != 0 || ((flags & 0xFF) && !(flags & 1))) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    memset(GuestPtr(caps), 0, 20);
    Store<uint8_t>(caps + 0, 1);       // XINPUT_DEVTYPE_GAMEPAD
    Store<uint8_t>(caps + 1, 1);       // XINPUT_DEVSUBTYPE_GAMEPAD
    Store<uint16_t>(caps + 4, 0xFFFF); // all buttons
    Store<uint8_t>(caps + 6, 0xFF);
    Store<uint8_t>(caps + 7, 0xFF);
    for (int i = 0; i < 4; i++) Store<int16_t>(caps + 8 + i * 2, int16_t(0xFFC0));
    Store<uint16_t>(caps + 16, 0xFFFF);
    Store<uint16_t>(caps + 18, 0xFFFF);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// u32 XamInputGetState(u32 user, u32 flags, X_INPUT_STATE* state): packet number, buttons, triggers, sticks
GUEST_IMPORT(XamInputGetState)
{
    uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, state = ctx.r5.u32;
    if ((user & 0xFF) == 0xFF) user = 0;
    if (user != 0 || ((flags & 0xFF) && !(flags & 1))) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    if (!state) { ctx.r3.u64 = 0xA0; return; }
    GamepadState pad = InputGetGamepad();
    Store<uint32_t>(state + 0, pad.packet);
    Store<uint16_t>(state + 4, pad.buttons);
    Store<uint8_t>(state + 6, pad.leftTrigger);
    Store<uint8_t>(state + 7, pad.rightTrigger);
    Store<int16_t>(state + 8, pad.thumbLX);
    Store<int16_t>(state + 10, pad.thumbLY);
    Store<int16_t>(state + 12, pad.thumbRX);
    Store<int16_t>(state + 14, pad.thumbRY);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

GUEST_IMPORT(XamInputSetState)
{
    uint32_t user = ctx.r3.u32;
    if ((user & 0xFF) == 0xFF) user = 0;
    ctx.r3.u64 = user == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

// u32 XamInputGetKeystrokeEx(u32* user, u32 flags, X_INPUT_KEYSTROKE* key): no keystrokes
GUEST_IMPORT(XamInputGetKeystrokeEx)
{
    uint32_t userPtr = ctx.r3.u32;
    uint32_t user = userPtr ? Load<uint32_t>(userPtr) : 0;
    if ((user & 0xFF) == 0xFF) user = 0;
    ctx.r3.u64 = user == 0 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

GUEST_IMPORT(XamResetInactivity) { ctx.r3.u64 = 0; }
GUEST_IMPORT(XamEnableInactivityProcessing) { ctx.r3.u64 = 0; }

// ---------------------------------------------------------------------------
// Messages to system apps (XMsg*)
// ---------------------------------------------------------------------------
static uint32_t DispatchAppMessage(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length)
{
    static int logged = 0;
    if (logged++ < 20)
        Log("XAM: message app %02X id %08X (buffer %08X, %u bytes)", app, message, buffer, length);
    if (app == 0xFE && message == 0x00020021 && buffer) // XamApp: clear the output word
    {
        if (uint32_t out = Load<uint32_t>(buffer + 0x44)) Store<uint32_t>(out, 0);
        return X_E_SUCCESS;
    }
    if (app == 0xFC && message == 0x00058006 && buffer) // XLiveBase: NAT type = open
    {
        Store<uint32_t>(buffer, 1);
        return X_E_SUCCESS;
    }
    return X_E_SUCCESS;
}

// u32 XMsgInProcessCall(u32 app, u32 message, u32 arg1, u32 arg2)
GUEST_IMPORT(XMsgInProcessCall) { ctx.r3.u64 = DispatchAppMessage(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }

// u32 XMsgStartIORequest(u32 app, u32 message, XAM_OVERLAPPED* ov, u32 buffer, u32 length)
GUEST_IMPORT(XMsgStartIORequest)
{
    uint32_t result = DispatchAppMessage(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u64 = ctx.r5.u32 ? CompleteOverlapped(ctx.r5.u32, result) : result;
}

GUEST_IMPORT(XMsgCancelIORequest) { ctx.r3.u64 = 0; }

// u32 XamContentGetLicenseMask(u32* mask, overlapped): fully licensed
GUEST_IMPORT(XamContentGetLicenseMask)
{
    if (ctx.r3.u32) Store<uint32_t>(ctx.r3.u32, 0xFFFFFFFF);
    ctx.r3.u64 = ctx.r4.u32 ? CompleteOverlapped(ctx.r4.u32, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Content (saves): one hard drive. A container is a directory in the writable
// overlay, _content\<type>\<file name>, mounted as "<root>:" while open; its
// XCONTENT_DATA is kept next to it (<file name>.xcontent) for enumeration.
// ---------------------------------------------------------------------------
constexpr uint32_t X_ERROR_NO_MORE_FILES = 0x12, X_ERROR_FILE_NOT_FOUND = 2, X_ERROR_ALREADY_EXISTS = 0xB7;
constexpr uint32_t kContentDataSize = 0x134; // device id, type, display name[128] (UTF-16), file name[42]

namespace
{
    std::string ContentTypeDir(uint32_t type)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "_content\\%08x", type);
        return buf;
    }

    // The container's directory (disc-relative, in the overlay), from XCONTENT_DATA.
    std::string ContentDir(uint32_t data)
    {
        std::string name = LoadGuestString(data + 0x108, 42), safe;
        for (char c : name)
            safe += (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') ? char(tolower((unsigned char)c)) : '_';
        if (safe.empty()) safe = "default";
        return ContentTypeDir(Load<uint32_t>(data + 4)) + "\\" + safe;
    }

    void MakeDirectories(const std::string& discDir)
    {
        std::string path;
        size_t pos = 0;
        PlatformMakeDirectory(FsOverlayHostPath(""));
        while (pos != std::string::npos)
        {
            size_t next = discDir.find('\\', pos);
            path = discDir.substr(0, next);
            PlatformMakeDirectory(FsOverlayHostPath(path));
            pos = next == std::string::npos ? next : next + 1;
        }
    }

    struct ContentEnum { std::vector<std::vector<uint8_t>> items; size_t next = 0; uint32_t perCall = 1; };
    std::mutex g_enumMutex;
    std::map<uint32_t, ContentEnum> g_contentEnums; // by handle
}

// u32 XamContentCreateEx(user, rootName, XCONTENT_DATA*, flags, u32* disposition, u32* licenseMask,
//                        cacheSize, u64 contentSize, overlapped)
GUEST_IMPORT(XamContentCreateEx)
{
    uint32_t rootPtr = ctx.r4.u32, data = ctx.r5.u32, flags = ctx.r6.u32, dispPtr = ctx.r7.u32, licensePtr = ctx.r8.u32;
    uint32_t ov = StackArg32(ctx, 8);
    std::string root = LoadGuestString(rootPtr, 32), dir = ContentDir(data);
    std::string host = FsOverlayHostPath(dir);
    bool exists = PlatformPathKind(host) == PathKind::Directory;
    uint32_t mode = flags & 0xF; // 1 create new, 2 create always, 3 open existing, 4 open always, 5 truncate existing
    uint32_t result = X_ERROR_SUCCESS;
    bool create = false;
    if (mode == 1 && exists) result = X_ERROR_ALREADY_EXISTS;
    else if ((mode == 3 || mode == 5) && !exists) result = X_ERROR_FILE_NOT_FOUND;
    else create = !exists;
    if (result == X_ERROR_SUCCESS)
    {
        if (create)
        {
            MakeDirectories(dir);
            if (PlatformFile* meta = PlatformOpenFile(host + ".xcontent", OpenMode::TruncateReadWrite))
            {
                PlatformWriteAt(meta, 0, GuestPtr(data), kContentDataSize);
                PlatformCloseFile(meta);
            }
        }
        FsMountContent(root, dir);
        if (dispPtr) Store<uint32_t>(dispPtr, create ? 1 : 2); // created / opened
        if (licensePtr) Store<uint32_t>(licensePtr, 0xFFFFFFFF);
    }
    Log("XAM: content %s as %s: (mode %u) -> %s", dir.c_str(), root.c_str(), mode,
        result ? "not available" : create ? "created" : "opened");
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, result) : result;
}

// u32 XamContentClose(rootName, overlapped)
GUEST_IMPORT(XamContentClose)
{
    static const bool writeTrace = getenv("WRITE_TRACE") != nullptr;
    if (writeTrace)
        Log("XAM: content %s: closed", LoadGuestString(ctx.r3.u32, 32).c_str());
    FsUnmountContent(LoadGuestString(ctx.r3.u32, 32));
    ctx.r3.u64 = ctx.r4.u32 ? CompleteOverlapped(ctx.r4.u32, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// u32 XamContentDelete(user, XCONTENT_DATA*, overlapped): forgets the container (its files stay)
GUEST_IMPORT(XamContentDelete)
{
    std::string host = FsOverlayHostPath(ContentDir(ctx.r4.u32));
    uint32_t result = PlatformRemoveFile(host + ".xcontent") ? X_ERROR_SUCCESS : X_ERROR_FILE_NOT_FOUND;
    ctx.r3.u64 = ctx.r5.u32 ? CompleteOverlapped(ctx.r5.u32, result) : result;
}

// u32 XamContentGetCreator(user, XCONTENT_DATA*, u32* isCreator, u64* creatorXuid, overlapped)
GUEST_IMPORT(XamContentGetCreator)
{
    if (ctx.r5.u32) Store<uint32_t>(ctx.r5.u32, 1);
    if (ctx.r6.u32) Store<uint64_t>(ctx.r6.u32, kXuid);
    ctx.r3.u64 = ctx.r7.u32 ? CompleteOverlapped(ctx.r7.u32, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// u32 XamContentSetThumbnail(user, XCONTENT_DATA*, buffer, size, overlapped)
GUEST_IMPORT(XamContentSetThumbnail)
{
    ctx.r3.u64 = ctx.r7.u32 ? CompleteOverlapped(ctx.r7.u32, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// u32 XamContentCreateEnumerator(user, deviceId, contentType, flags, itemsPerCall, u32* bufferSize, u32* handle)
GUEST_IMPORT(XamContentCreateEnumerator)
{
    uint32_t type = ctx.r5.u32, perCall = std::max<uint32_t>(1, ctx.r7.u32), sizePtr = ctx.r8.u32, handlePtr = ctx.r9.u32;
    ContentEnum e;
    e.perCall = perCall;
    std::string typeHost = FsOverlayHostPath(ContentTypeDir(type));
    std::vector<PlatformDirEntry> list;
    PlatformListDirectory(typeHost, list);
    for (const auto& f : list)
    {
        if (f.isDir || f.name.size() < 10 || f.name.compare(f.name.size() - 9, 9, ".xcontent") != 0)
            continue;
        if (PlatformFile* meta = PlatformOpenFile(typeHost + PlatformPathSeparator() + f.name, OpenMode::Read))
        {
            std::vector<uint8_t> item(kContentDataSize);
            if (PlatformReadAt(meta, 0, item.data(), kContentDataSize))
                e.items.push_back(std::move(item));
            PlatformCloseFile(meta);
        }
    }
    if (sizePtr) Store<uint32_t>(sizePtr, perCall * kContentDataSize);
    uint32_t handle = CreateHandle(ObjectKind::Other, KernelAlloc(16));
    if (handlePtr) Store<uint32_t>(handlePtr, handle);
    Log("XAM: content enumerator (type %08X) -> %zu saved item(s)", type, e.items.size());
    std::lock_guard lock(g_enumMutex);
    g_contentEnums[handle] = std::move(e);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// u32 XamCreateEnumeratorHandle(user, appId, openMsg, closeMsg, extraSize, itemCount, flags, u32* handle)
GUEST_IMPORT(XamCreateEnumeratorHandle)
{
    if (ctx.r10.u32) Store<uint32_t>(ctx.r10.u32, CreateHandle(ObjectKind::Other, KernelAlloc(16)));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// u32 XamEnumerate(handle, flags, buffer, bufferSize, u32* itemsReturned, overlapped)
GUEST_IMPORT(XamEnumerate)
{
    uint32_t handle = ctx.r3.u32, buffer = ctx.r5.u32, bufferSize = ctx.r6.u32, items = ctx.r7.u32, ov = ctx.r8.u32;
    uint32_t count = 0;
    {
        std::lock_guard lock(g_enumMutex);
        auto it = g_contentEnums.find(handle);
        if (it != g_contentEnums.end() && buffer)
        {
            ContentEnum& e = it->second;
            while (e.next < e.items.size() && count < e.perCall && (count + 1) * kContentDataSize <= bufferSize)
            {
                memcpy(GuestPtr(buffer + count * kContentDataSize), e.items[e.next].data(), kContentDataSize);
                e.next++;
                count++;
            }
        }
    }
    if (items) Store<uint32_t>(items, count);
    uint32_t result = count ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES;
    ctx.r3.u64 = ov ? CompleteOverlapped(ov, result) : result;
}

// u32 XamContentGetDeviceData(deviceId, XDEVICE_DATA*): id, type, total, free, name[28]
GUEST_IMPORT(XamContentGetDeviceData)
{
    uint32_t id = ctx.r3.u32, data = ctx.r4.u32;
    if (id != 1 || !data) { ctx.r3.u64 = 0x48F; return; }
    memset(GuestPtr(data), 0, 0x50);
    Store<uint32_t>(data + 0, 1);
    Store<uint32_t>(data + 4, 1);
    Store<uint64_t>(data + 8, 20ull << 30);
    Store<uint64_t>(data + 16, 16ull << 30);
    const char* name = "Hard Drive";
    for (size_t i = 0; name[i]; i++) Store<uint16_t>(data + 24 + uint32_t(i) * 2, uint16_t(name[i]));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
