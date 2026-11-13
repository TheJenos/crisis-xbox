// Multiplayer: Xbox LIVE's sessions, matchmaking and QoS, emulated.
//
// The title creates sessions through XAM (app 0xFB messages, below): the host's
// session gets a key (XNKID, XNKEY) and the host's network address (XNADDR),
// which is how players find and reach each other. Host sessions are listed for
// the others through the page (web-src/mp.js: BroadcastChannel between tabs of
// one browser, or Firebase between machines); the title's searches read that
// list (crysis_mp_set_directory). Packets between players go through the page
// too (net.cpp).
//
// Addresses: each player has a 24-bit ID (MP_ID, from the page). Its IP is
// 10.x.y.z (the ID's bytes) and every other part of its XNADDR follows from
// the IP, so a player's address can be made from their IP alone.
//
// Layouts of the 0xFB messages were read from the title's own requests
// (XAM_DUMP=1 logs each message's words the first time).
#include "guest.h"
#include "mp.h"
#include "platform.h"

#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace
{
    constexpr uint32_t X_E_SUCCESS = 0, X_E_FAIL = 0x80004005;
    constexpr uint32_t XSESSION_CREATE_HOST = 0x1, XSESSION_CREATE_USES_MATCHMAKING = 0x8;

    std::mutex g_mutex;

    std::mt19937_64& Rng()
    {
        static std::mt19937_64 rng(std::random_device{}() ^ (uint64_t(MpId()) << 32) ^ PlatformCounterNs());
        return rng;
    }

    std::string Hex(const uint8_t* p, size_t n)
    {
        static const char* d = "0123456789abcdef";
        std::string s;
        for (size_t i = 0; i < n; i++)
            s += d[p[i] >> 4], s += d[p[i] & 15];
        return s;
    }

    std::vector<uint8_t> Unhex(const std::string& s)
    {
        std::vector<uint8_t> out;
        for (size_t i = 0; i + 1 < s.size(); i += 2)
            out.push_back(uint8_t(strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
        return out;
    }

    // XUSER_PROPERTY values as set by the title: type, then the value's bytes
    // (big-endian ints, or the data of a binary/string value).
    struct Property { uint8_t type; std::vector<uint8_t> value; };

    // This player's contexts and properties (XUserSetContext/Property): a hosted
    // session is listed with them, and searches filter on them.
    std::map<uint32_t, uint32_t> g_contexts;
    std::map<uint32_t, Property> g_properties;

    struct Session
    {
        uint32_t flags = 0, publicSlots = 0, privateSlots = 0, filledPublic = 0, filledPrivate = 0;
        uint8_t info[60] = {}; // XSESSION_INFO: XNKID[8], XNADDR host[36], XNKEY[16]
        bool host = false;
        std::vector<uint8_t> qos; // the host's QoS listen data (XNetQosListen)
    };
    std::map<uint32_t, Session> g_sessions; // by session object
    uint8_t g_currentKey[8] = {};           // the session last created or joined

    // Other players' listed sessions (from the page).
    struct Listing
    {
        uint8_t info[60] = {};
        uint32_t flags = 0, publicSlots = 0, privateSlots = 0, filledPublic = 0, filledPrivate = 0;
        std::vector<std::pair<uint32_t, uint32_t>> contexts;
        std::vector<std::pair<uint32_t, Property>> properties;
        std::vector<uint8_t> qos;
        std::string hostName;
    };
    std::vector<Listing> g_directory;

    // This session as one line for the page:
    // key info flags pub priv fpub fpriv contexts properties qos name
    std::string ListingLine(const Session& s)
    {
        std::ostringstream o;
        o << Hex(s.info, 8) << ' ' << Hex(s.info, 60) << ' ' << s.flags << ' ' << s.publicSlots << ' ' << s.privateSlots << ' '
          << s.filledPublic << ' ' << s.filledPrivate << ' ';
        std::string ctx, props;
        for (auto& [id, v] : g_contexts)
            ctx += (ctx.empty() ? "" : ",") + std::to_string(id) + ":" + std::to_string(v);
        for (auto& [id, p] : g_properties)
            props += (props.empty() ? "" : ",") + std::to_string(id) + ":" + std::to_string(p.type) + ":" + Hex(p.value.data(), p.value.size());
        o << (ctx.empty() ? "-" : ctx) << ' ' << (props.empty() ? "-" : props) << ' ' << (s.qos.empty() ? "-" : Hex(s.qos.data(), s.qos.size()))
          << ' ' << MpGamertag();
        return o.str();
    }

    void Publish(const Session& s)
    {
        if (!s.host)
            return;
        std::string line = ListingLine(s);
#ifdef __EMSCRIPTEN__
        char* copy = strdup(line.c_str());
        MAIN_THREAD_ASYNC_EM_ASM({ if (Module.mpPublish) Module.mpPublish(UTF8ToString($0 >>> 0)); _free($0); }, copy);
#endif
    }

    // This player's status for the others (c2/players: menus, lobby, in match).
    void SetStatus(const char* status) // (a string literal)
    {
#ifdef __EMSCRIPTEN__
        MAIN_THREAD_ASYNC_EM_ASM({ if (window.crysisMp) crysisMp.setStatus(UTF8ToString($0 >>> 0)); }, status);
#else
        (void)status;
#endif
    }

    void Unpublish(const Session& s)
    {
        if (!s.host)
            return;
        std::string key = Hex(s.info, 8);
#ifdef __EMSCRIPTEN__
        char* copy = strdup(key.c_str());
        MAIN_THREAD_ASYNC_EM_ASM({ if (Module.mpUnpublish) Module.mpUnpublish(UTF8ToString($0 >>> 0)); _free($0); }, copy);
#endif
    }

    // Logs a message the first time, with its words (layouts for later work).
    void LogOnce(uint32_t message, const char* what, uint32_t buffer, uint32_t length)
    {
        static std::set<uint32_t> seen;
        if (!seen.insert(message).second)
            return;
        std::string words;
        for (uint32_t i = 0; buffer && i < std::min<uint32_t>(length, 48); i += 4)
        {
            char w[12];
            snprintf(w, sizeof(w), " %08X", Load<uint32_t>(buffer + i));
            words += w;
        }
        Log("mp: %s (XGI %08X, %u bytes):%s", what, message, length, words.c_str());
    }

    // XSessionSearch results: XSESSION_SEARCHRESULT_HEADER {count, results*},
    // then the results (XSESSION_SEARCHRESULT, 92 bytes: XSESSION_INFO, open
    // public/private slots, filled public/private, property and context counts
    // and pointers), then their properties (24 bytes: id, XUSER_DATA type at +8,
    // value at +16) and contexts (8 bytes: id, value).
    uint32_t WriteSearchResults(uint32_t results, uint32_t size, uint32_t maxResults)
    {
        if (!results || size < 8)
            return 0;
        std::vector<const Listing*> found;
        for (const Listing& l : g_directory)
            if ((l.flags & XSESSION_CREATE_USES_MATCHMAKING) && l.filledPublic < l.publicSlots && found.size() < maxResults)
                found.push_back(&l);
        uint32_t entries = results + 8, extra = entries + uint32_t(found.size()) * 92, end = results + size, count = 0;
        for (const Listing* l : found)
        {
            uint32_t need = uint32_t(l->properties.size()) * 24 + uint32_t(l->contexts.size()) * 8;
            for (auto& [id, p] : l->properties)
                if (p.type == 4 || p.type == 6) need += (uint32_t(p.value.size()) + 7) & ~7u;
            if (entries + (count + 1) * 92 > end || extra + need > end)
                break;
            uint32_t r = entries + count * 92;
            memcpy(GuestPtr(r), l->info, 60);
            Store<uint32_t>(r + 60, l->publicSlots - l->filledPublic);
            Store<uint32_t>(r + 64, l->privateSlots > l->filledPrivate ? l->privateSlots - l->filledPrivate : 0);
            Store<uint32_t>(r + 68, l->filledPublic);
            Store<uint32_t>(r + 72, l->filledPrivate);
            Store<uint32_t>(r + 76, uint32_t(l->properties.size()));
            Store<uint32_t>(r + 80, uint32_t(l->contexts.size()));
            uint32_t props = extra, ctxs = extra + uint32_t(l->properties.size()) * 24, data = ctxs + uint32_t(l->contexts.size()) * 8;
            Store<uint32_t>(r + 84, l->properties.empty() ? 0 : props);
            Store<uint32_t>(r + 88, l->contexts.empty() ? 0 : ctxs);
            for (size_t i = 0; i < l->properties.size(); i++)
            {
                const auto& [id, p] = l->properties[i];
                uint32_t e = props + uint32_t(i) * 24;
                memset(GuestPtr(e), 0, 24);
                Store<uint32_t>(e, id);
                Store<uint8_t>(e + 8, p.type);
                if (p.type == 4 || p.type == 6) // unicode string, binary: size and pointer
                {
                    Store<uint32_t>(e + 16, uint32_t(p.value.size()));
                    Store<uint32_t>(e + 20, data);
                    memcpy(GuestPtr(data), p.value.data(), p.value.size());
                    data += (uint32_t(p.value.size()) + 7) & ~7u;
                }
                else
                    memcpy(GuestPtr(e + 16), p.value.data(), std::min<size_t>(8, p.value.size()));
            }
            for (size_t i = 0; i < l->contexts.size(); i++)
            {
                Store<uint32_t>(ctxs + uint32_t(i) * 8, l->contexts[i].first);
                Store<uint32_t>(ctxs + uint32_t(i) * 8 + 4, l->contexts[i].second);
            }
            extra = data;
            count++;
        }
        Store<uint32_t>(results, count);
        Store<uint32_t>(results + 4, count ? entries : 0);
        return count;
    }
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
bool MpEnabled()
{
    static const bool live = getenv("LIVE") && atoi(getenv("LIVE")) != 0;
    return live;
}

uint32_t MpId()
{
    static const uint32_t id = [] {
        const char* v = getenv("MP_ID");
        uint32_t x = v ? uint32_t(strtoul(v, nullptr, 16)) & 0xFFFFFF : 1;
        if ((x & 0xFF) == 0 || (x & 0xFF) == 0xFF) x ^= 0x01; // (no .0 or .255 host byte)
        return x ? x : 1;
    }();
    return id;
}

uint32_t MpOwnIp() { return 0x0A000000 | MpId(); }

uint64_t MpXuid(bool online) { return (online ? 0x0009000000000000ull : 0xE000000000000000ull) | MpId(); }

std::string MpGamertag()
{
    static const std::string name = [] {
        const char* v = getenv("MP_NAME");
        std::string s = v && *v ? v : "Player";
        return s.substr(0, 15);
    }();
    return name;
}

// XNADDR: ina, inaOnline (IN_ADDR), wPortOnline, abEnet[6], abOnline[20].
void MpXnAddr(uint32_t ip, uint8_t* out)
{
    memset(out, 0, 36);
    for (int i = 0; i < 4; i++)
        out[i] = out[4 + i] = uint8_t(ip >> (24 - i * 8));
    out[8] = 1000 >> 8, out[9] = 1000 & 0xFF;
    const uint8_t enet[6] = { 0x00, 0x22, 0x48, uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip) };
    memcpy(out + 10, enet, 6);
    memcpy(out + 16, "C2MP", 4);
    for (int i = 0; i < 4; i++)
        out[20 + i] = uint8_t(ip >> (24 - i * 8));
}

uint32_t MpIpOfXnAddr(uint32_t xna) { return xna ? Load<uint32_t>(xna) : 0; }

bool MpInSession()
{
    std::lock_guard lock(g_mutex);
    return !g_sessions.empty();
}

void MpCurrentSessionKey(uint8_t* out8)
{
    std::lock_guard lock(g_mutex);
    memcpy(out8, g_currentKey, 8);
}

// The key of the session a player at `ip` shares with us: a session they host
// that we joined, else the match session we host (players come to join it),
// else the last one.
void MpSessionKeyFor(uint32_t ip, uint8_t* out8)
{
    std::lock_guard lock(g_mutex);
    for (auto& [obj, s] : g_sessions)
    {
        uint32_t host = uint32_t(s.info[8]) << 24 | uint32_t(s.info[9]) << 16 | uint32_t(s.info[10]) << 8 | s.info[11];
        if (!s.host && host == ip) { memcpy(out8, s.info, 8); return; }
    }
    for (auto& [obj, s] : g_sessions)
        if (s.host && (s.flags & XSESSION_CREATE_USES_MATCHMAKING)) { memcpy(out8, s.info, 8); return; }
    memcpy(out8, g_currentKey, 8);
}

// ---------------------------------------------------------------------------
// Session objects
// ---------------------------------------------------------------------------
// u32 XamSessionCreateHandle(u32* handle): a session is a kernel object the
// title passes (by pointer) in its 0xFB messages.
GUEST_IMPORT(XamSessionCreateHandle)
{
    uint32_t object = CreateDispatcherObject(16, 0, 0);
    uint32_t handle = CreateHandle(ObjectKind::Event, object);
    if (ctx.r3.u32) Store<uint32_t>(ctx.r3.u32, handle);
    ctx.r3.u64 = 0;
}

// u32 XamSessionRefObjByHandle(handle, u32* object)
GUEST_IMPORT(XamSessionRefObjByHandle)
{
    ObjectKind kind;
    uint32_t object = 0;
    bool ok = LookupHandle(ctx.r3.u32, &kind, &object);
    if (ctx.r4.u32) Store<uint32_t>(ctx.r4.u32, ok ? object : 0);
    ctx.r3.u64 = ok ? 0 : 0xC0000008; // STATUS_INVALID_HANDLE
}

// ---------------------------------------------------------------------------
// App 0xFB (XGI) messages
// ---------------------------------------------------------------------------
uint32_t MpXgiMessage(uint32_t message, uint32_t b, uint32_t length, bool* handled)
{
    *handled = true;
    auto w = [&](uint32_t i) { return b && i * 4 + 4 <= length ? Load<uint32_t>(b + i * 4) : 0; };
    std::lock_guard lock(g_mutex);
    switch (message)
    {
    case 0x000B0006: // XUserSetContext: user, ?, ?, ?, context id, value
        LogOnce(message, "XUserSetContext", b, length);
        g_contexts[w(4)] = w(5);
        return X_E_SUCCESS;

    case 0x000B0007: // XUserSetProperty: user, ?, ?, ?, property id, size, value*
    {
        LogOnce(message, "XUserSetProperty", b, length);
        uint32_t id = w(4), size = w(5), value = w(6);
        Property p;
        p.type = uint8_t(id >> 28); // the type is in the ID's top nibble (XPROPERTYTYPEFROMID)
        if (value && size && size <= 1024)
        {
            const uint8_t* v = (const uint8_t*)GuestPtr(value);
            p.value.assign(v, v + size);
        }
        g_properties[id] = std::move(p);
        return X_E_SUCCESS;
    }

    case 0x000B0010: // XSessionCreate: session, flags, public slots, private slots, user, info*, nonce*
    {
        LogOnce(message, "XSessionCreate", b, length);
        Session& s = g_sessions[w(0)];
        s = Session{};
        s.flags = w(1), s.publicSlots = w(2), s.privateSlots = w(3);
        s.host = (s.flags & XSESSION_CREATE_HOST) != 0;
        uint32_t info = w(5), nonce = w(6);
        if (s.host)
        {
            // A new key: XNKID (an online peer session: 0x8_ in its top byte) and XNKEY.
            uint64_t kid = Rng()(), key1 = Rng()(), key2 = Rng()();
            for (int i = 0; i < 8; i++)
                s.info[i] = uint8_t(kid >> (56 - i * 8));
            s.info[0] = uint8_t(0x80 | (s.info[0] & 0x0F));
            MpXnAddr(MpOwnIp(), s.info + 8);
            for (int i = 0; i < 8; i++)
                s.info[44 + i] = uint8_t(key1 >> (56 - i * 8)), s.info[52 + i] = uint8_t(key2 >> (56 - i * 8));
            if (info) memcpy(GuestPtr(info), s.info, 60);
            if (nonce) Store<uint64_t>(nonce, Rng()());
        }
        else if (info)
            memcpy(s.info, GuestPtr(info), 60); // joining: the host's session, from a search
        memcpy(g_currentKey, s.info, 8);
        if (s.flags & XSESSION_CREATE_USES_MATCHMAKING)
            SetStatus("lobby");
        Log("mp: session %08X %s (flags %X, %u public slots, key %s, host %u.%u.%u.%u)", w(0), s.host ? "hosted" : "joined",
            s.flags, s.publicSlots, Hex(s.info, 8).c_str(), s.info[8], s.info[9], s.info[10], s.info[11]);
        Publish(s);
        return X_E_SUCCESS;
    }

    case 0x000B0011: // XSessionDelete: session
    {
        LogOnce(message, "XSessionDelete", b, length);
        auto it = g_sessions.find(w(0));
        if (it != g_sessions.end())
        {
            if (it->second.flags & XSESSION_CREATE_USES_MATCHMAKING)
                SetStatus("menus");
            Unpublish(it->second);
            g_sessions.erase(it);
        }
        return X_E_SUCCESS;
    }

    case 0x000B0012: // XSessionJoinLocal: session, count, ?, user indices*, private flags*
    {
        LogOnce(message, "XSessionJoinLocal", b, length);
        auto it = g_sessions.find(w(0));
        if (it == g_sessions.end())
            return X_E_SUCCESS;
        Session& s = it->second;
        uint32_t count = std::min<uint32_t>(w(1), 16), flags = w(4);
        for (uint32_t i = 0; i < count; i++)
        {
            bool priv = flags && Load<uint32_t>(flags + i * 4) != 0;
            (priv ? s.filledPrivate : s.filledPublic)++;
        }
        s.filledPublic = std::min(s.filledPublic, s.publicSlots);
        Publish(s);
        return X_E_SUCCESS;
    }

    case 0x000B0013: // XSessionLeaveLocal: session, count, ?, user indices*, ?
    {
        LogOnce(message, "XSessionLeaveLocal", b, length);
        auto it = g_sessions.find(w(0));
        if (it != g_sessions.end())
        {
            uint32_t count = std::min<uint32_t>(w(1), 16);
            it->second.filledPublic -= std::min(it->second.filledPublic, count);
            Publish(it->second);
        }
        return X_E_SUCCESS;
    }

    case 0x000B0014: // XSessionStart: session, flags, ?, ?
    case 0x000B0015: // XSessionEnd: session, ?
        LogOnce(message, message == 0x000B0014 ? "XSessionStart" : "XSessionEnd", b, length);
        SetStatus(message == 0x000B0014 ? "in match" : "lobby");
        return X_E_SUCCESS;

    case 0x000B0018: // XSessionModify: session, flags, public slots, private slots
    {
        LogOnce(message, "XSessionModify", b, length);
        auto it = g_sessions.find(w(0));
        if (it != g_sessions.end())
        {
            Session& s = it->second;
            s.flags = (s.flags & XSESSION_CREATE_HOST) | (w(1) & ~XSESSION_CREATE_HOST);
            s.publicSlots = w(2), s.privateSlots = w(3);
            Publish(s);
        }
        return X_E_SUCCESS;
    }

    case 0x000B0016: // XSessionSearch: procedure, user, results, properties, contexts, ...
    case 0x000B001C: // XSessionSearchEx: procedure, user, max results, props<<16|contexts, props*, contexts*, size, results*, users
    {
        LogOnce(message, message == 0x000B0016 ? "XSessionSearch" : "XSessionSearchEx", b, length);
        uint32_t found = message == 0x000B001C ? WriteSearchResults(w(7), w(6), w(2)) : 0;
        static int logged = 0;
        if (logged++ < 20)
            Log("mp: session search (procedure %u): %u found of %zu listed", w(0), found, g_directory.size());
        return X_E_SUCCESS;
    }

    default:
        LogOnce(message, "XGI message (not handled: succeeds)", b, length);
        *handled = false;
        return X_E_SUCCESS;
    }
}

// ---------------------------------------------------------------------------
// QoS: probes report every listed player as reachable, 20-25 ms away, with the
// data their host set (XNetQosListen).
// ---------------------------------------------------------------------------
// int XNetQosListen(caller, XNKID*, data, size, bitsPerSec, flags)
GUEST_IMPORT(NetDll_XNetQosListen)
{
    uint32_t kid = ctx.r4.u32, data = ctx.r5.u32, size = ctx.r6.u32, flags = ctx.r8.u32;
    if (kid && (flags & 4) && data && size <= 4096) // XNET_QOS_LISTEN_SET_DATA
    {
        std::lock_guard lock(g_mutex);
        for (auto& [obj, s] : g_sessions)
            if (s.host && memcmp(s.info, GuestPtr(kid), 8) == 0)
            {
                const uint8_t* p = (const uint8_t*)GuestPtr(data);
                s.qos.assign(p, p + size);
                Publish(s);
            }
    }
    ctx.r3.u64 = 0;
}

// int XNetQosLookup(caller, count, XNADDR**, XNKID**, XNKEY**, serverCount, IN_ADDR*, serviceIds*,
//                   probes, bitsPerSec, flags, event, XNQOS** result)
GUEST_IMPORT(NetDll_XNetQosLookup)
{
    uint32_t count = std::min<uint32_t>(ctx.r4.u32, 64), kids = ctx.r6.u32;
    uint32_t probes = StackArg32(ctx, 8), event = StackArg32(ctx, 11), out = StackArg32(ctx, 12);
    std::vector<std::vector<uint8_t>> data(count);
    {
        std::lock_guard lock(g_mutex);
        for (uint32_t i = 0; i < count && kids; i++)
        {
            uint32_t kid = Load<uint32_t>(kids + i * 4);
            for (const Listing& l : g_directory)
                if (kid && memcmp(l.info, GuestPtr(kid), 8) == 0)
                    data[i] = l.qos;
        }
    }
    // XNQOS: count, pending, then XNQOSINFO[24 bytes]: flags, reserved, probes sent,
    // probes received, data size, data*, min/median RTT (ms), up/down bits per second.
    uint32_t size = 8 + count * 24;
    for (auto& d : data) size += (uint32_t(d.size()) + 7) & ~7u;
    uint32_t q = KernelAlloc(std::max<uint32_t>(size, 32));
    memset(GuestPtr(q), 0, size);
    Store<uint32_t>(q, count);
    uint32_t extra = q + 8 + count * 24;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t e = q + 8 + i * 24;
        Store<uint8_t>(e, uint8_t(0x01 | 0x02 | (data[i].empty() ? 0 : 0x08))); // complete, contacted, data received
        Store<uint16_t>(e + 2, uint16_t(probes ? probes : 4));
        Store<uint16_t>(e + 4, uint16_t(probes ? probes : 4));
        Store<uint16_t>(e + 6, uint16_t(data[i].size()));
        if (!data[i].empty())
        {
            Store<uint32_t>(e + 8, extra);
            memcpy(GuestPtr(extra), data[i].data(), data[i].size());
            extra += (uint32_t(data[i].size()) + 7) & ~7u;
        }
        Store<uint16_t>(e + 12, 20);
        Store<uint16_t>(e + 14, 25);
        Store<uint32_t>(e + 16, 2000000);
        Store<uint32_t>(e + 20, 2000000);
    }
    if (out) Store<uint32_t>(out, q);
    ObjectKind kind;
    uint32_t obj;
    if (event && LookupHandle(event, &kind, &obj))
        SignalEventObject(obj);
    static int logged = 0;
    if (logged++ < 10)
        Log("mp: QoS lookup of %u player(s): all reachable", count);
    ctx.r3.u64 = 0;
}

// int XNetQosRelease(caller, XNQOS*)
GUEST_IMPORT(NetDll_XNetQosRelease)
{
    if (ctx.r4.u32) KernelFree(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// Page interface
// ---------------------------------------------------------------------------
void MpSendPacket(uint32_t fromIp, uint16_t fromPort, uint32_t toIp, uint16_t toPort, const uint8_t* data, size_t size)
{
#ifdef __EMSCRIPTEN__
    uint8_t* copy = (uint8_t*)malloc(size ? size : 1);
    memcpy(copy, data, size);
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            const bytes = new Uint8Array(wasmMemory.buffer, $4 >>> 0, $5).slice();
            _free($4);
            if (Module.mpSend) Module.mpSend($0 >>> 0, $1, $2 >>> 0, $3, bytes);
        },
        fromIp, fromPort, toIp, toPort, copy, int(size));
#else
    (void)fromIp, (void)fromPort, (void)toIp, (void)toPort, (void)data, (void)size;
#endif
}

// The sessions other players list, one per line (see ListingLine), from the page.
extern "C" void crysis_mp_set_directory(const char* text)
{
    std::vector<Listing> list;
    std::istringstream in(text ? text : "");
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream f(line);
        std::string key, info, ctx, props, qos, name;
        Listing l;
        if (!(f >> key >> info >> l.flags >> l.publicSlots >> l.privateSlots >> l.filledPublic >> l.filledPrivate >> ctx >> props >> qos))
            continue;
        std::getline(f, name);
        std::vector<uint8_t> bytes = Unhex(info);
        if (bytes.size() != 60)
            continue;
        memcpy(l.info, bytes.data(), 60);
        uint32_t hostIp = uint32_t(l.info[8]) << 24 | uint32_t(l.info[9]) << 16 | uint32_t(l.info[10]) << 8 | l.info[11];
        if (hostIp == MpOwnIp())
            continue; // (our own sessions are never search results)
        std::istringstream c(ctx == "-" ? "" : ctx);
        for (std::string item; std::getline(c, item, ',');)
        {
            size_t colon = item.find(':');
            if (colon != std::string::npos)
                l.contexts.push_back({ uint32_t(strtoul(item.c_str(), nullptr, 10)), uint32_t(strtoul(item.c_str() + colon + 1, nullptr, 10)) });
        }
        std::istringstream p(props == "-" ? "" : props);
        for (std::string item; std::getline(p, item, ',');)
        {
            size_t a = item.find(':'), b2 = a == std::string::npos ? a : item.find(':', a + 1);
            if (b2 == std::string::npos)
                continue;
            Property pr;
            pr.type = uint8_t(strtoul(item.c_str() + a + 1, nullptr, 10));
            pr.value = Unhex(item.substr(b2 + 1));
            l.properties.push_back({ uint32_t(strtoul(item.c_str(), nullptr, 10)), std::move(pr) });
        }
        if (qos != "-")
            l.qos = Unhex(qos);
        l.hostName = name.empty() ? "" : name.substr(1);
        list.push_back(std::move(l));
    }
    std::lock_guard lock(g_mutex);
    g_directory = std::move(list);
}
