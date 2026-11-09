// IMPORT_TRACE=<guest thread id>: count that thread's import calls and log the
// busiest ones every 5 seconds (to see what a thread is waiting for).
#include "guest.h"
#include "platform.h"

#include <algorithm>
#include <unordered_map>

uint32_t CurrentThreadId();

bool g_importTrace = getenv("IMPORT_TRACE") != nullptr;
thread_local bool g_importSample = false;

void ImportTraceHit(const char* name)
{
    static const uint32_t thread = getenv("IMPORT_TRACE") ? uint32_t(atoi(getenv("IMPORT_TRACE"))) : 0;
    g_importSample = false;
    if (CurrentThreadId() != thread)
        return;
    static std::unordered_map<const char*, uint64_t> counts;
    static uint64_t windowStart = PlatformCounterNs();
    g_importSample = ++counts[name] <= 4; // the first calls of each window log their arguments
    uint64_t now = PlatformCounterNs();
    if (now - windowStart < 5000000000ull)
        return;
    std::vector<std::pair<uint64_t, const char*>> top;
    for (auto& [n, c] : counts) top.push_back({ c, n });
    std::sort(top.rbegin(), top.rend());
    std::string line;
    for (size_t i = 0; i < top.size() && i < 14; i++)
        line += std::string(" ") + top[i].second + "=" + std::to_string(top[i].first);
    Log("IMPORT_TRACE t%u (5 s):%s", thread, line.c_str());
    counts.clear();
    windowStart = now;
}
