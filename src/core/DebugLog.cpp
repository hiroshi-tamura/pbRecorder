#include "core/DebugLog.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace pb {

namespace {
std::mutex g_logMutex;
std::atomic<int> g_enabled{-1};  // -1 = unknown, 0 = off, 1 = on
}

bool debugLogEnabled() {
    int cached = g_enabled.load();
    if (cached >= 0) return cached != 0;
    char buf[8] = {};
    DWORD n = GetEnvironmentVariableA("PBRECORDER_DEBUG", buf, sizeof(buf));
    int on = (n > 0) ? 1 : 0;
    g_enabled.store(on);
    return on != 0;
}

void debugLog(const std::string& message) {
    if (!debugLogEnabled()) return;

    // Timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::string line = ts;
    line += message;

    OutputDebugStringA(line.c_str());
    OutputDebugStringA("\n");

    std::lock_guard<std::mutex> lock(g_logMutex);
    FILE* f = std::fopen("pbRecorder-debug.log", "a");
    if (f) {
        std::fputs(line.c_str(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

} // namespace pb
