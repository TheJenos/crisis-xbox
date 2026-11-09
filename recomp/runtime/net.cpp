// Networking: an offline console with a working local (loopback) network.
//
// CryEngine runs single-player as a local server plus client that talk over a
// UDP (VDP) socket, so datagram sockets must work even with no network. They
// are emulated in-process: each socket has a local port, and a datagram sent to
// a port is delivered to the local socket bound to it (addresses are ignored;
// nothing leaves the process, which also works in the browser). Stream (TCP)
// sockets and everything online still fail with "network down".
//
// NetDll_* functions take an extra leading "caller" argument, so their real
// parameters start at r4. Arguments past the 8th real register come from the
// stack (StackArg32 index 8 = 9th guest argument).
#include "guest.h"
#include "platform.h"

#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace
{
    constexpr uint32_t WSAENETDOWN = 10050, WSAEWOULDBLOCK = 10035, WSAENOTSOCK = 10038, WSAEINVAL = 10022,
                       WSA_IO_PENDING = 997, WSA_IO_INCOMPLETE = 996, WSAEADDRINUSE = 10048;
    constexpr uint32_t INVALID_SOCKET = 0xFFFFFFFF, SOCKET_ERROR = 0xFFFFFFFF;
    constexpr uint32_t LOOPBACK = 0x7F000001; // 127.0.0.1
    thread_local uint32_t t_lastError = 0;

    struct Datagram
    {
        std::vector<uint8_t> data;
        uint16_t fromPort;
    };

    // An overlapped WSARecvFrom waiting for data.
    struct PendingRecv
    {
        uint32_t bufs, bufCount, from, fromLen, overlapped;
    };

    struct Socket
    {
        uint32_t type = 2;
        uint16_t port = 0;
        bool nonBlocking = false;
        std::deque<Datagram> queue;
        std::deque<PendingRecv> pending;
    };

    std::mutex g_mutex;
    std::map<uint32_t, Socket> g_sockets;
    uint32_t g_nextSocket = 0x100;
    uint16_t g_nextPort = 49152;
    std::atomic<uint64_t> g_datagrams{ 0 };

    Socket* Find(uint32_t s)
    {
        auto it = g_sockets.find(s);
        return it == g_sockets.end() ? nullptr : &it->second;
    }

    Socket* BoundTo(uint16_t port)
    {
        for (auto& [id, s] : g_sockets)
            if (s.port == port)
                return &s;
        return nullptr;
    }

    uint16_t PortOf(uint32_t sockaddr) { return sockaddr ? Load<uint16_t>(sockaddr + 2) : 0; } // sockaddr_in.sin_port (big-endian = guest order)

    void WriteSockaddr(uint32_t addr, uint32_t lenPtr, uint16_t port)
    {
        if (!addr)
            return;
        memset(GuestPtr(addr), 0, 16);
        Store<uint16_t>(addr + 0, 2); // AF_INET
        Store<uint16_t>(addr + 2, port);
        Store<uint32_t>(addr + 4, LOOPBACK);
        if (lenPtr)
            Store<uint32_t>(lenPtr, 16);
    }

    // Copy a datagram into WSABUF[] (u32 len, u32 buf). Returns bytes copied.
    uint32_t CopyToBufs(const Datagram& d, uint32_t bufs, uint32_t count)
    {
        uint32_t done = 0;
        for (uint32_t i = 0; i < count && done < d.data.size(); i++)
        {
            uint32_t len = Load<uint32_t>(bufs + i * 8), ptr = Load<uint32_t>(bufs + i * 8 + 4);
            uint32_t n = std::min<uint32_t>(len, uint32_t(d.data.size()) - done);
            memcpy(GuestPtr(ptr), d.data.data() + done, n);
            done += n;
        }
        return done;
    }

    std::vector<uint8_t> GatherBufs(uint32_t bufs, uint32_t count)
    {
        std::vector<uint8_t> out;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t len = Load<uint32_t>(bufs + i * 8), ptr = Load<uint32_t>(bufs + i * 8 + 4);
            const uint8_t* p = (const uint8_t*)GuestPtr(ptr);
            out.insert(out.end(), p, p + len);
        }
        return out;
    }

    // WSAOVERLAPPED: Internal (status), InternalHigh (bytes), Offset, OffsetHigh, hEvent.
    void EventSet(uint32_t handle, bool signaled)
    {
        ObjectKind kind;
        uint32_t obj;
        if (!handle || !LookupHandle(handle, &kind, &obj))
            return;
        if (signaled)
            SignalEventObject(obj);
        else
            Store<int32_t>(obj + 4, 0);
    }

    void CompleteOverlapped(uint32_t ov, uint32_t bytes)
    {
        Store<uint32_t>(ov + 0, 0);
        Store<uint32_t>(ov + 4, bytes);
        EventSet(Load<uint32_t>(ov + 16), true);
    }

    // Deliver a datagram from `fromPort` to the socket bound to `toPort` (caller holds g_mutex).
    void Deliver(uint16_t fromPort, uint16_t toPort, std::vector<uint8_t> data)
    {
        Socket* target = BoundTo(toPort);
        if (!target)
        {
            static std::atomic<int> dropped{ 0 };
            if (dropped++ < 10)
                Log("net: datagram %u -> %u (%zu bytes) dropped: no socket bound to that port", fromPort, toPort, data.size());
            return; // nobody listening: dropped, like UDP
        }
        uint64_t n = ++g_datagrams;
        if (n <= 16 || (n & (n - 1)) == 0)
            Log("net: datagram %llu: %u -> %u, %zu bytes (%s)", (unsigned long long)n, fromPort, toPort, data.size(),
                target->pending.empty() ? "queued" : "completes a pending receive");
        Datagram d{ std::move(data), fromPort };
        if (!target->pending.empty())
        {
            PendingRecv r = target->pending.front();
            target->pending.pop_front();
            uint32_t n = CopyToBufs(d, r.bufs, r.bufCount);
            WriteSockaddr(r.from, r.fromLen, d.fromPort);
            CompleteOverlapped(r.overlapped, n);
            return;
        }
        target->queue.push_back(std::move(d));
    }
}

// int WSAStartup(u16 version, WSADATA* data)
GUEST_IMPORT(NetDll_WSAStartup)
{
    uint32_t data = ctx.r5.u32;
    if (data)
    {
        memset(GuestPtr(data), 0, 0x190);
        Store<uint16_t>(data + 0, 0x0202); // wVersion
        Store<uint16_t>(data + 2, 0x0202); // wHighVersion
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_XNetStartup) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = t_lastError; }

// XNET_GET_XNADDR_*: 0 = pending. Report the cable as disconnected.
GUEST_IMPORT(NetDll_XNetGetConnectStatus) { ctx.r3.u64 = 0; }

// SOCKET socket(int af, int type, int protocol): datagram sockets only.
GUEST_IMPORT(NetDll_socket)
{
    uint32_t type = ctx.r5.u32;
    if (type != 2) // SOCK_DGRAM
    {
        t_lastError = WSAENETDOWN;
        ctx.r3.u64 = INVALID_SOCKET;
        return;
    }
    std::lock_guard lock(g_mutex);
    uint32_t id = g_nextSocket++;
    g_sockets[id].type = type;
    static std::atomic<int> n{ 0 };
    if (n++ < 10)
        Log("net: socket(%u, %u, %u) -> %X (loopback datagram socket)", ctx.r4.u32, type, ctx.r6.u32, id);
    ctx.r3.u64 = id;
}

GUEST_IMPORT(NetDll_closesocket)
{
    std::lock_guard lock(g_mutex);
    ctx.r3.u64 = g_sockets.erase(ctx.r4.u32) ? 0 : (t_lastError = WSAENOTSOCK, SOCKET_ERROR);
}

// int bind(SOCKET s, sockaddr* addr, int len)
GUEST_IMPORT(NetDll_bind)
{
    std::lock_guard lock(g_mutex);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { t_lastError = WSAENOTSOCK; ctx.r3.u64 = SOCKET_ERROR; return; }
    uint16_t port = PortOf(ctx.r5.u32);
    if (port && BoundTo(port) && BoundTo(port) != s) { t_lastError = WSAEADDRINUSE; ctx.r3.u64 = SOCKET_ERROR; return; }
    if (!port)
        while (BoundTo(port = g_nextPort++)) {}
    s->port = port;
    Log("net: socket %X bound to port %u", ctx.r4.u32, port);
    ctx.r3.u64 = 0;
}

// int getsockname(SOCKET s, sockaddr* addr, int* len)
GUEST_IMPORT(NetDll_getsockname)
{
    std::lock_guard lock(g_mutex);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { t_lastError = WSAENOTSOCK; ctx.r3.u64 = SOCKET_ERROR; return; }
    WriteSockaddr(ctx.r5.u32, ctx.r6.u32, s->port);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_setsockopt) { ctx.r3.u64 = 0; }
GUEST_IMPORT(NetDll_shutdown) { ctx.r3.u64 = 0; }

// int ioctlsocket(SOCKET s, long cmd, u32* arg): FIONBIO, FIONREAD
GUEST_IMPORT(NetDll_ioctlsocket)
{
    std::lock_guard lock(g_mutex);
    Socket* s = Find(ctx.r4.u32);
    uint32_t cmd = ctx.r5.u32, arg = ctx.r6.u32;
    if (!s) { t_lastError = WSAENOTSOCK; ctx.r3.u64 = SOCKET_ERROR; return; }
    if (cmd == 0x8004667E && arg) // FIONBIO
        s->nonBlocking = Load<uint32_t>(arg) != 0;
    else if (cmd == 0x4004667F && arg) // FIONREAD
        Store<uint32_t>(arg, s->queue.empty() ? 0 : uint32_t(s->queue.front().data.size()));
    ctx.r3.u64 = 0;
}

// int sendto(SOCKET s, const char* buf, int len, int flags, sockaddr* to, int tolen)
GUEST_IMPORT(NetDll_sendto)
{
    std::lock_guard lock(g_mutex);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { t_lastError = WSAENOTSOCK; ctx.r3.u64 = SOCKET_ERROR; return; }
    if (!s->port)
        while (BoundTo(s->port = g_nextPort++) != s) {}
    const uint8_t* p = (const uint8_t*)GuestPtr(ctx.r5.u32);
    Deliver(s->port, PortOf(ctx.r8.u32), std::vector<uint8_t>(p, p + ctx.r6.u32));
    ctx.r3.u64 = ctx.r6.u32;
}

// int WSASendTo(SOCKET s, WSABUF* bufs, u32 count, u32* sent, u32 flags, sockaddr* to, int tolen,
//               WSAOVERLAPPED* ov, completion)
GUEST_IMPORT(NetDll_WSASendTo)
{
    std::lock_guard lock(g_mutex);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { t_lastError = WSAENOTSOCK; ctx.r3.u64 = SOCKET_ERROR; return; }
    if (!s->port)
        while (BoundTo(s->port = g_nextPort++) != s) {}
    std::vector<uint8_t> data = GatherBufs(ctx.r5.u32, ctx.r6.u32);
    uint32_t size = uint32_t(data.size());
    Deliver(s->port, PortOf(ctx.r9.u32), std::move(data));
    if (ctx.r7.u32) Store<uint32_t>(ctx.r7.u32, size);
    uint32_t ov = StackArg32(ctx, 8);
    if (ov) CompleteOverlapped(ov, size);
    ctx.r3.u64 = 0;
}

// int WSARecvFrom(SOCKET s, WSABUF* bufs, u32 count, u32* received, u32* flags, sockaddr* from,
//                 int* fromlen, WSAOVERLAPPED* ov, completion)
GUEST_IMPORT(NetDll_WSARecvFrom)
{
    std::lock_guard lock(g_mutex);
    Socket* s = Find(ctx.r4.u32);
    if (!s) { t_lastError = WSAENOTSOCK; ctx.r3.u64 = SOCKET_ERROR; return; }
    uint32_t bufs = ctx.r5.u32, count = ctx.r6.u32, received = ctx.r7.u32, flags = ctx.r8.u32;
    uint32_t from = ctx.r9.u32, fromLen = ctx.r10.u32, ov = StackArg32(ctx, 8);
    if (flags) Store<uint32_t>(flags, 0);
    if (!s->queue.empty())
    {
        Datagram d = std::move(s->queue.front());
        s->queue.pop_front();
        uint32_t n = CopyToBufs(d, bufs, count);
        WriteSockaddr(from, fromLen, d.fromPort);
        if (received) Store<uint32_t>(received, n);
        if (ov) CompleteOverlapped(ov, n);
        ctx.r3.u64 = 0;
        return;
    }
    if (ov)
    {
        Store<uint32_t>(ov + 0, STATUS_PENDING);
        Store<uint32_t>(ov + 4, 0);
        EventSet(Load<uint32_t>(ov + 16), false);
        s->pending.push_back({ bufs, count, from, fromLen, ov });
        t_lastError = WSA_IO_PENDING;
        ctx.r3.u64 = SOCKET_ERROR;
        return;
    }
    t_lastError = WSAEWOULDBLOCK;
    ctx.r3.u64 = SOCKET_ERROR;
}

// BOOL WSAGetOverlappedResult(SOCKET s, WSAOVERLAPPED* ov, u32* bytes, BOOL wait, u32* flags)
GUEST_IMPORT(NetDll_WSAGetOverlappedResult)
{
    uint32_t ov = ctx.r5.u32, bytes = ctx.r6.u32, flags = ctx.r8.u32;
    bool wait = ctx.r7.u32 != 0;
    for (;;)
    {
        if (Load<uint32_t>(ov) != STATUS_PENDING)
        {
            if (bytes) Store<uint32_t>(bytes, Load<uint32_t>(ov + 4));
            if (flags) Store<uint32_t>(flags, 0);
            ctx.r3.u64 = 1;
            return;
        }
        if (!wait)
        {
            t_lastError = WSA_IO_INCOMPLETE;
            ctx.r3.u64 = 0;
            return;
        }
        PlatformSleepMs(1);
    }
}

// Pending overlapped reads are cancelled when the socket or event goes away; nothing else to do.
GUEST_IMPORT(NetDll_WSACreateEvent)
{
    uint32_t object = CreateDispatcherObject(16, 0, 0); // manual-reset (notification) event
    ctx.r3.u64 = CreateHandle(ObjectKind::Event, object);
}

GUEST_IMPORT(NetDll_WSACloseEvent) { ctx.r3.u64 = CloseHandle(ctx.r4.u32) ? 1 : 0; }

// fd_set: u32 count, SOCKET array[64]
GUEST_IMPORT(NetDll___WSAFDIsSet)
{
    uint32_t s = ctx.r4.u32, set = ctx.r5.u32;
    uint32_t n = set ? std::min<uint32_t>(Load<uint32_t>(set), 64) : 0;
    ctx.r3.u64 = 0;
    for (uint32_t i = 0; i < n; i++)
        if (Load<uint32_t>(set + 4 + i * 4) == s)
            ctx.r3.u64 = 1;
}

// int select(int nfds, fd_set* read, fd_set* write, fd_set* except, timeval* timeout)
GUEST_IMPORT(NetDll_select)
{
    uint32_t readSet = ctx.r5.u32, writeSet = ctx.r6.u32, exceptSet = ctx.r7.u32, timeout = ctx.r8.u32;
    int64_t waitMs = timeout ? int64_t(Load<uint32_t>(timeout)) * 1000 + Load<uint32_t>(timeout + 4) / 1000 : -1;
    auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        uint32_t ready = 0;
        std::vector<uint32_t> readable;
        {
            std::lock_guard lock(g_mutex);
            if (readSet)
            {
                uint32_t n = std::min<uint32_t>(Load<uint32_t>(readSet), 64);
                for (uint32_t i = 0; i < n; i++)
                {
                    uint32_t s = Load<uint32_t>(readSet + 4 + i * 4);
                    Socket* sock = Find(s);
                    if (sock && !sock->queue.empty())
                        readable.push_back(s);
                }
            }
        }
        uint32_t writable = writeSet ? std::min<uint32_t>(Load<uint32_t>(writeSet), 64) : 0;
        ready = uint32_t(readable.size()) + writable;
        int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (ready || (waitMs >= 0 && elapsed >= waitMs))
        {
            if (readSet)
            {
                Store<uint32_t>(readSet, uint32_t(readable.size()));
                for (size_t i = 0; i < readable.size(); i++)
                    Store<uint32_t>(readSet + 4 + uint32_t(i) * 4, readable[i]);
            }
            if (exceptSet)
                Store<uint32_t>(exceptSet, 0);
            ctx.r3.u64 = ready;
            return;
        }
        PlatformSleepMs(1);
    }
}

// Stream sockets (TCP) don't exist offline.
#define NET_FAIL(name) GUEST_IMPORT(name) { t_lastError = WSAENETDOWN; ctx.r3.u64 = SOCKET_ERROR; }
NET_FAIL(NetDll_connect)
NET_FAIL(NetDll_listen)
NET_FAIL(NetDll_accept)
NET_FAIL(NetDll_send)
NET_FAIL(NetDll_recv)
