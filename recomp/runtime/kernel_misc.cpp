// Kernel/XAM imports: strings, time, XEX queries, console settings, formatting.
#include "guest.h"

#include <ctime>
#include <functional>
#include "platform.h"

std::string LoadAnsiString(uint32_t s)
{
    uint16_t len = Load<uint16_t>(s);
    uint32_t buf = Load<uint32_t>(s + 4);
    return buf ? std::string((const char*)GuestPtr(buf), len) : std::string();
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------
// void RtlInitAnsiString(ANSI_STRING* dest, const char* src)
GUEST_IMPORT(RtlInitAnsiString)
{
    uint32_t dest = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t len = src ? uint16_t(strlen((const char*)GuestPtr(src))) : 0;
    Store<uint16_t>(dest, len);
    Store<uint16_t>(dest + 2, src ? len + 1 : 0);
    Store<uint32_t>(dest + 4, src);
}

// void RtlInitUnicodeString(UNICODE_STRING* dest, const wchar16* src)
GUEST_IMPORT(RtlInitUnicodeString)
{
    uint32_t dest = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t chars = 0;
    if (src)
        while (Load<uint16_t>(src + chars * 2)) chars++;
    Store<uint16_t>(dest, chars * 2);
    Store<uint16_t>(dest + 2, src ? (chars + 1) * 2 : 0);
    Store<uint32_t>(dest + 4, src);
}

GUEST_IMPORT(RtlFreeAnsiString)
{
    uint32_t buf = Load<uint32_t>(ctx.r3.u32 + 4);
    if (buf) KernelFree(buf);
    Store<uint32_t>(ctx.r3.u32 + 4, 0);
}

// NTSTATUS RtlUnicodeStringToAnsiString(ANSI_STRING* dest, UNICODE_STRING* src, u32 allocate)
GUEST_IMPORT(RtlUnicodeStringToAnsiString)
{
    uint32_t dest = ctx.r3.u32, src = ctx.r4.u32;
    uint16_t chars = Load<uint16_t>(src) / 2;
    uint32_t sbuf = Load<uint32_t>(src + 4);
    uint32_t dbuf;
    if (ctx.r5.u32)
    {
        dbuf = KernelAlloc(chars + 1);
        Store<uint32_t>(dest + 4, dbuf);
        Store<uint16_t>(dest + 2, chars + 1);
    }
    else
    {
        dbuf = Load<uint32_t>(dest + 4);
        chars = std::min<uint16_t>(chars, Load<uint16_t>(dest + 2) ? Load<uint16_t>(dest + 2) - 1 : 0);
    }
    for (uint16_t i = 0; i < chars; i++)
    {
        uint16_t c = Load<uint16_t>(sbuf + i * 2);
        Store<uint8_t>(dbuf + i, c < 0x80 ? uint8_t(c) : '?');
    }
    Store<uint8_t>(dbuf + chars, 0);
    Store<uint16_t>(dest, chars);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS RtlMultiByteToUnicodeN(wchar16* dest, u32 destBytes, u32* resultBytes, const char* src, u32 srcBytes)
GUEST_IMPORT(RtlMultiByteToUnicodeN)
{
    uint32_t dest = ctx.r3.u32, destBytes = ctx.r4.u32, out = ctx.r5.u32, src = ctx.r6.u32, srcBytes = ctx.r7.u32;
    uint32_t n = std::min(destBytes / 2, srcBytes);
    for (uint32_t i = 0; i < n; i++)
        Store<uint16_t>(dest + i * 2, Load<uint8_t>(src + i));
    if (out) Store<uint32_t>(out, n * 2);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS RtlUnicodeToMultiByteN(char* dest, u32 destBytes, u32* resultBytes, const wchar16* src, u32 srcBytes)
GUEST_IMPORT(RtlUnicodeToMultiByteN)
{
    uint32_t dest = ctx.r3.u32, destBytes = ctx.r4.u32, out = ctx.r5.u32, src = ctx.r6.u32, srcBytes = ctx.r7.u32;
    uint32_t n = std::min(destBytes, srcBytes / 2);
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = Load<uint16_t>(src + i * 2);
        Store<uint8_t>(dest + i, c < 0x100 ? uint8_t(c) : '?');
    }
    if (out) Store<uint32_t>(out, n);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(RtlUpcaseUnicodeChar)
{
    uint16_t c = ctx.r3.u16;
    ctx.r3.u64 = (c >= 'a' && c <= 'z') ? c - 32 : c;
}

// i32 RtlCompareStringN(const char* a, u32 aLen, const char* b, u32 bLen, u32 caseInsensitive)
GUEST_IMPORT(RtlCompareStringN)
{
    uint32_t a = ctx.r3.u32, alen = ctx.r4.u32, b = ctx.r5.u32, blen = ctx.r6.u32;
    bool ci = ctx.r7.u32;
    uint32_t n = std::min(alen, blen);
    for (uint32_t i = 0; i < n; i++)
    {
        int ca = Load<uint8_t>(a + i), cb = Load<uint8_t>(b + i);
        if (ci) { ca = toupper(ca); cb = toupper(cb); }
        if (ca != cb) { ctx.r3.s64 = ca - cb; return; }
    }
    ctx.r3.s64 = int64_t(alen) - int64_t(blen);
}

// u32 RtlCompareMemoryUlong(void* src, u32 length, u32 pattern): bytes matching
GUEST_IMPORT(RtlCompareMemoryUlong)
{
    uint32_t src = ctx.r3.u32, len = ctx.r4.u32, pattern = ctx.r5.u32;
    uint32_t i = 0;
    while (i + 4 <= len && Load<uint32_t>(src + i) == pattern) i += 4;
    ctx.r3.u64 = i;
}

GUEST_IMPORT(RtlFillMemoryUlong)
{
    for (uint32_t i = 0; i + 4 <= ctx.r4.u32; i += 4)
        Store<uint32_t>(ctx.r3.u32 + i, ctx.r5.u32);
}

GUEST_IMPORT(RtlNtStatusToDosError)
{
    uint32_t s = ctx.r3.u32;
    switch (s)
    {
    case STATUS_SUCCESS: ctx.r3.u64 = 0; break;
    case STATUS_NO_SUCH_FILE: case STATUS_OBJECT_NAME_NOT_FOUND: ctx.r3.u64 = 2; break; // ERROR_FILE_NOT_FOUND
    case STATUS_OBJECT_PATH_NOT_FOUND: ctx.r3.u64 = 3; break;
    case STATUS_ACCESS_DENIED: ctx.r3.u64 = 5; break;
    case STATUS_INVALID_HANDLE: ctx.r3.u64 = 6; break;
    case STATUS_NO_MEMORY: ctx.r3.u64 = 8; break;
    case STATUS_END_OF_FILE: ctx.r3.u64 = 38; break;
    case STATUS_INVALID_PARAMETER: ctx.r3.u64 = 87; break;
    case STATUS_PENDING: ctx.r3.u64 = 997; break;
    case STATUS_NO_MORE_FILES: ctx.r3.u64 = 18; break;
    default: ctx.r3.u64 = (s & 0xFFFF0000) == 0x80070000 ? (s & 0xFFFF) : 317; break; // ERROR_MR_MID_NOT_FOUND
    }
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
GUEST_IMPORT(KeQuerySystemTime)
{
    Store<uint64_t>(ctx.r3.u32, uint64_t(PlatformFileTimeNow()));
}

// The Xenon time base runs at 50 MHz.
GUEST_IMPORT(KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000; }

// Calendar conversion for 100 ns ticks since 1601-01-01 (proleptic Gregorian).
namespace
{
    constexpr int64_t TICKS_PER_DAY = 864000000000ll;

    // Days from 1970-01-01 for a civil date, and back (Howard Hinnant's algorithms).
    int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d)
    {
        y -= m <= 2;
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        int64_t yoe = y - era * 400;
        int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }
    void CivilFromDays(int64_t z, int64_t& y, int64_t& m, int64_t& d)
    {
        z += 719468;
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        int64_t doe = z - era * 146097;
        int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int64_t mp = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp + (mp < 10 ? 3 : -9);
        y = yoe + era * 400 + (m <= 2);
    }
    const int64_t kEpochDays1601 = DaysFromCivil(1601, 1, 1);
}

// TIME_FIELDS { u16 year, month, day, hour, minute, second, milliseconds, weekday }
GUEST_IMPORT(RtlTimeToTimeFields)
{
    int64_t t = Load<int64_t>(ctx.r3.u32);
    int64_t days = t / TICKS_PER_DAY, rem = t % TICKS_PER_DAY;
    int64_t y, m, d;
    CivilFromDays(days + kEpochDays1601, y, m, d);
    uint32_t f = ctx.r4.u32;
    Store<uint16_t>(f + 0, uint16_t(y)); Store<uint16_t>(f + 2, uint16_t(m)); Store<uint16_t>(f + 4, uint16_t(d));
    Store<uint16_t>(f + 6, uint16_t(rem / 36000000000ll));
    Store<uint16_t>(f + 8, uint16_t(rem / 600000000 % 60));
    Store<uint16_t>(f + 10, uint16_t(rem / 10000000 % 60));
    Store<uint16_t>(f + 12, uint16_t(rem / 10000 % 1000));
    Store<uint16_t>(f + 14, uint16_t((days + 1) % 7)); // 1601-01-01 was a Monday; 0 = Sunday
}

GUEST_IMPORT(RtlTimeFieldsToTime)
{
    uint32_t f = ctx.r3.u32;
    int64_t y = Load<uint16_t>(f + 0), m = Load<uint16_t>(f + 2), d = Load<uint16_t>(f + 4);
    int64_t h = Load<uint16_t>(f + 6), mi = Load<uint16_t>(f + 8), sec = Load<uint16_t>(f + 10), ms = Load<uint16_t>(f + 12);
    bool ok = y >= 1601 && m >= 1 && m <= 12 && d >= 1 && d <= 31 && h < 24 && mi < 60 && sec < 60 && ms < 1000;
    if (ok)
    {
        int64_t days = DaysFromCivil(y, m, d) - kEpochDays1601;
        int64_t t = days * TICKS_PER_DAY + h * 36000000000ll + mi * 600000000ll + sec * 10000000ll + ms * 10000ll;
        Store<uint64_t>(ctx.r4.u32, uint64_t(t));
    }
    ctx.r3.u64 = ok;
}

// ---------------------------------------------------------------------------
// XEX and console information
// ---------------------------------------------------------------------------
// void* RtlImageXexHeaderField(void* xexHeader, u32 key)
GUEST_IMPORT(RtlImageXexHeaderField) { ctx.r3.u64 = XexHeaderField(ctx.r4.u32); }

// u32 XexCheckExecutablePrivilege(u32 privilege): bit of the SYSTEM_FLAGS header
GUEST_IMPORT(XexCheckExecutablePrivilege)
{
    uint32_t p = ctx.r3.u32;
    ctx.r3.u64 = p < 32 ? (g_xex.systemFlags >> p) & 1 : 0;
}

// NTSTATUS XexGetModuleHandle(const char* name, u32* handle)
GUEST_IMPORT(XexGetModuleHandle)
{
    std::string name = LoadGuestString(ctx.r3.u32);
    Log("XexGetModuleHandle(\"%s\")", name.c_str());
    if (ctx.r4.u32) Store<uint32_t>(ctx.r4.u32, name.empty() ? g_xex.headerAddr : 0);
    ctx.r3.u64 = name.empty() ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

// NTSTATUS XexGetProcedureAddress(u32 module, u32 ordinal, u32* address)
GUEST_IMPORT(XexGetProcedureAddress)
{
    Log("XexGetProcedureAddress(%08X, %u) not supported", ctx.r3.u32, ctx.r4.u32);
    if (ctx.r5.u32) Store<uint32_t>(ctx.r5.u32, 0);
    ctx.r3.u64 = STATUS_NOT_IMPLEMENTED;
}

GUEST_IMPORT(XGetLanguage) { ctx.r3.u64 = 1; }          // English
GUEST_IMPORT(XGetGameRegion) { ctx.r3.u64 = 0x00FF; }   // North America (all)
GUEST_IMPORT(XGetAVPack) { ctx.r3.u64 = 8; }            // HDMI
GUEST_IMPORT(XamGetSystemVersion) { ctx.r3.u64 = 0x20096B00; } // 2.0.17150.0

// void XGetVideoMode(XVIDEO_MODE* mode)
GUEST_IMPORT(XGetVideoMode)
{
    uint32_t m = ctx.r3.u32;
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

// NTSTATUS XamGetExecutionId(XEX_EXECUTION_ID** out)
GUEST_IMPORT(XamGetExecutionId)
{
    uint32_t id = XexHeaderField(0x00040006);
    Store<uint32_t>(ctx.r3.u32, id);
    ctx.r3.u64 = id ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

// NTSTATUS ExGetXConfigSetting(u16 category, u16 setting, void* buffer, u16 size, u16* requiredSize)
GUEST_IMPORT(ExGetXConfigSetting)
{
    uint16_t category = ctx.r3.u16, setting = ctx.r4.u16;
    uint32_t buffer = ctx.r5.u32, size = ctx.r6.u16, required = ctx.r7.u32;
    uint32_t value = 0, valueSize = 4;
    bool known = true;
    if (category == 3) // XCONFIG_USER_CATEGORY
    {
        switch (setting)
        {
        case 0x01: value = 0; break;            // time zone bias
        case 0x09: value = 1; break;            // language: English
        case 0x0A: value = 0x00040000; break;   // video flags: widescreen
        case 0x0B: value = 0; break;            // audio flags
        case 0x0C: value = 0; break;            // retail flags
        case 0x0E: value = 103; break;          // country: US
        case 0x0F: value = 0; break;            // parental control flags
        default: known = false; break;
        }
    }
    else if (category == 2 && setting == 2) // XCONFIG_SECURED_CATEGORY / AV region
    {
        value = 0x1000; // NTSC-M
    }
    else
        known = false;

    if (!known)
    {
        Log("ExGetXConfigSetting(%u, %u) unknown, returning zeros", category, setting);
        if (buffer) memset(GuestPtr(buffer), 0, size);
        if (required) Store<uint16_t>(required, uint16_t(size));
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    }
    if (buffer && size >= valueSize) Store<uint32_t>(buffer, value);
    if (required) Store<uint16_t>(required, uint16_t(valueSize));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Debugging / errors
// ---------------------------------------------------------------------------
GUEST_IMPORT(DbgBreakPoint) { Log("DbgBreakPoint (lr %08llX)", ctx.lr); }
GUEST_IMPORT(KeBugCheck) { Fatal("KeBugCheck(%08X)", ctx.r3.u32); }
GUEST_IMPORT(KeBugCheckEx) { Fatal("KeBugCheckEx(%08X, %08X, %08X, %08X, %08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }

// void RtlRaiseException(EXCEPTION_RECORD* record): { code, flags, record, address, numParams, info[] }
GUEST_IMPORT(RtlRaiseException)
{
    uint32_t rec = ctx.r3.u32;
    uint32_t code = Load<uint32_t>(rec);
    if (code == 0x406D1388) // MSVC "set thread name": info[1] = name, info[2] = thread id (-1 = caller)
    {
        uint32_t nameAddr = Load<uint32_t>(rec + 20 + 4), threadId = Load<uint32_t>(rec + 20 + 8);
        std::string name = LoadGuestString(nameAddr, 63);
        Log("thread %d named \"%s\"", threadId == 0xFFFFFFFF ? int(CurrentThreadId()) : int(threadId), name.c_str());
        SetThreadName(threadId, name.c_str());
        return;
    }
    Fatal("RtlRaiseException(code %08X, flags %08X, address %08X)", code, Load<uint32_t>(rec + 4), Load<uint32_t>(rec + 12));
}

// ---------------------------------------------------------------------------
// Formatting: sprintf family. Variadic arguments occupy 64-bit slots: the
// first ones in r3..r10 after the fixed parameters, the rest on the stack.
// ---------------------------------------------------------------------------
static std::string FormatGuest(const std::string& fmt, const std::function<uint64_t()>& nextArg)
{
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++)
    {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t start = i++;
        if (i < fmt.size() && fmt[i] == '%') { out += '%'; continue; }

        std::string spec = "%";
        while (i < fmt.size() && strchr("-+ #0", fmt[i])) spec += fmt[i++];
        if (i < fmt.size() && fmt[i] == '*') { spec += std::to_string(int32_t(nextArg())); i++; }
        while (i < fmt.size() && isdigit((unsigned char)fmt[i])) spec += fmt[i++];
        if (i < fmt.size() && fmt[i] == '.')
        {
            spec += fmt[i++];
            if (i < fmt.size() && fmt[i] == '*') { spec += std::to_string(int32_t(nextArg())); i++; }
            while (i < fmt.size() && isdigit((unsigned char)fmt[i])) spec += fmt[i++];
        }
        // Length modifiers: h, l, ll, I64, w
        bool is64 = false, isShort = false, isWide = false;
        for (;;)
        {
            if (i < fmt.size() && fmt[i] == 'h') { isShort = true; i++; }
            else if (i < fmt.size() && fmt[i] == 'l') { if (i + 1 < fmt.size() && fmt[i + 1] == 'l') { is64 = true; i++; } else isWide = true; i++; }
            else if (i < fmt.size() && fmt[i] == 'w') { isWide = true; i++; }
            else if (fmt.compare(i, 3, "I64") == 0) { is64 = true; i += 3; }
            else break;
        }
        if (i >= fmt.size()) { out += fmt.substr(start); break; }
        char conv = fmt[i];
        char buf[512];
        switch (conv)
        {
        case 'd': case 'i':
        {
            uint64_t a = nextArg();
            snprintf(buf, sizeof(buf), (spec + "lld").c_str(), is64 ? (long long)a : isShort ? (long long)int16_t(a) : (long long)int32_t(a));
            out += buf;
            break;
        }
        case 'u': case 'x': case 'X': case 'o':
        {
            uint64_t a = nextArg();
            snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), is64 ? (unsigned long long)a : isShort ? (unsigned long long)uint16_t(a) : (unsigned long long)uint32_t(a));
            out += buf;
            break;
        }
        case 'c':
            snprintf(buf, sizeof(buf), (spec + "c").c_str(), int(uint8_t(nextArg())));
            out += buf;
            break;
        case 'p':
            snprintf(buf, sizeof(buf), "%08X", uint32_t(nextArg()));
            out += buf;
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        {
            uint64_t bits = nextArg();
            double d;
            memcpy(&d, &bits, 8);
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), d);
            out += buf;
            break;
        }
        case 's': case 'S':
        {
            uint32_t p = uint32_t(nextArg());
            std::string s;
            if (!p) s = "(null)";
            else if (isWide || conv == 'S')
                for (uint32_t k = 0; Load<uint16_t>(p + k * 2); k++) { uint16_t c = Load<uint16_t>(p + k * 2); s += c < 0x80 ? char(c) : '?'; }
            else
                s = LoadGuestString(p, 1 << 20);
            std::string spec2 = spec + "s";
            std::vector<char> big(s.size() + 512);
            snprintf(big.data(), big.size(), spec2.c_str(), s.c_str());
            out += big.data();
            break;
        }
        case 'n':
            nextArg();
            break;
        default:
            out += fmt.substr(start, i - start + 1);
            break;
        }
    }
    return out;
}

// Arguments starting at register index `firstReg` (3 = r3), continuing on the stack.
static std::function<uint64_t()> RegisterArgs(PPCContext& ctx, int firstReg)
{
    auto idx = std::make_shared<int>(firstReg);
    return [&ctx, idx]() -> uint64_t {
        int r = (*idx)++;
        switch (r)
        {
        case 3: return ctx.r3.u64; case 4: return ctx.r4.u64; case 5: return ctx.r5.u64; case 6: return ctx.r6.u64;
        case 7: return ctx.r7.u64; case 8: return ctx.r8.u64; case 9: return ctx.r9.u64; case 10: return ctx.r10.u64;
        default: return StackArg64(ctx, uint32_t(r - 3)); // caller's parameter save area
        }
    };
}

static std::function<uint64_t()> VaListArgs(uint32_t va)
{
    auto p = std::make_shared<uint32_t>(va);
    return [p]() -> uint64_t { uint64_t v = Load<uint64_t>(*p); *p += 8; return v; };
}

static uint32_t WriteResult(uint32_t dest, int64_t count, const std::string& s)
{
    size_t n = count < 0 ? s.size() + 1 : std::min<size_t>(count, s.size() + 1);
    memcpy(GuestPtr(dest), s.c_str(), n);
    // _snprintf returns -1 when the output was truncated
    return (count >= 0 && s.size() >= size_t(count)) ? (s.size() == size_t(count) ? uint32_t(count) : 0xFFFFFFFF) : uint32_t(s.size());
}

// int sprintf(char* dest, const char* fmt, ...)
GUEST_IMPORT(sprintf)
{
    std::string s = FormatGuest(LoadGuestString(ctx.r4.u32, 1 << 16), RegisterArgs(ctx, 5));
    ctx.r3.u64 = WriteResult(ctx.r3.u32, -1, s);
}

// int _snprintf(char* dest, size_t count, const char* fmt, ...)
GUEST_IMPORT(_snprintf)
{
    std::string s = FormatGuest(LoadGuestString(ctx.r5.u32, 1 << 16), RegisterArgs(ctx, 6));
    ctx.r3.u64 = WriteResult(ctx.r3.u32, ctx.r4.u32, s);
}

// int _vsnprintf(char* dest, size_t count, const char* fmt, va_list args)
GUEST_IMPORT(_vsnprintf)
{
    std::string s = FormatGuest(LoadGuestString(ctx.r5.u32, 1 << 16), VaListArgs(ctx.r6.u32));
    ctx.r3.u64 = WriteResult(ctx.r3.u32, ctx.r4.u32, s);
}

// u32 DbgPrint(const char* fmt, ...)
GUEST_IMPORT(DbgPrint)
{
    std::string s = FormatGuest(LoadGuestString(ctx.r3.u32, 1 << 16), RegisterArgs(ctx, 4));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    Log("DbgPrint: %s", s.c_str());
    ctx.r3.u64 = 0;
}

// void XamLoaderTerminateTitle(): the console ends the title immediately, all
// threads included; letting it return would run exit cleanup under live threads.
GUEST_IMPORT(XamLoaderTerminateTitle)
{
    Log("title requested termination (XamLoaderTerminateTitle)");
    PlatformExit(0);
}

// Time base read by mftb (see PPC_TIMEBASE_EXTERNAL): the Xenon's counts at
// 50 MHz, matching KeQueryPerformanceFrequency. Derived from the host's
// high-resolution counter.
uint64_t PPCTimeBase()
{
    if (prof::Enabled())
        prof::timebaseReads.fetch_add(1, std::memory_order_relaxed);
    uint64_t ns = PlatformCounterNs();
    return (ns / 1000000000ull) * 50000000ull + (ns % 1000000000ull) / 20ull;
}
