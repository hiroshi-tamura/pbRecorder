#include "MonitorEnumerator.h"

#include <windows.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <stdexcept>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "user32.lib")

namespace pb {

// Type for SetProcessDpiAwarenessContext (available Win10 1703+).
using SetProcessDpiAwarenessContextFunc = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);

void MonitorEnumerator::ensureDpiAwareness() {
    static bool done = false;
    if (done) return;

    // Try the modern API first (Win10 1703+).
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFunc>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext")
        );
        if (fn) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            done = true;
            return;
        }
    }

    // Fallback to SetProcessDpiAwareness (Win8.1+).
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using SetProcessDpiAwarenessFunc = HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);
        auto fn = reinterpret_cast<SetProcessDpiAwarenessFunc>(
            GetProcAddress(shcore, "SetProcessDpiAwareness")
        );
        if (fn) {
            fn(PROCESS_PER_MONITOR_DPI_AWARE);
        }
    }

    done = true;
}

BOOL CALLBACK MonitorEnumerator::monitorEnumProc(
    HMONITOR hMonitor, HDC /*hdc*/, LPRECT /*lpRect*/, LPARAM lParam)
{
    auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi)) {
        return TRUE; // skip this monitor but continue enumeration
    }

    MonitorInfo info{};
    info.hMonitor = hMonitor;
    info.deviceName = mi.szDevice;
    info.index = static_cast<int>(monitors->size());

    // Physical rect from MONITORINFOEX.
    info.x = mi.rcMonitor.left;
    info.y = mi.rcMonitor.top;
    info.width = mi.rcMonitor.right - mi.rcMonitor.left;
    info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // Build a friendly name: "Display N" or use device name.
    // We can also try to get the friendly name from DISPLAY_DEVICEW.
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) {
        info.name = dd.DeviceString;
    }
    if (info.name.empty()) {
        info.name = mi.szDevice;
    }

    // Get per-monitor DPI using GetDpiForMonitor (shcore).
    UINT dpiX = 96, dpiY = 96;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using GetDpiForMonitorFunc = HRESULT(WINAPI*)(
            HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
        auto getDpi = reinterpret_cast<GetDpiForMonitorFunc>(
            GetProcAddress(shcore, "GetDpiForMonitor")
        );
        if (getDpi) {
            UINT dx = 96, dy = 96;
            HRESULT hr = getDpi(hMonitor, MDT_EFFECTIVE_DPI, &dx, &dy);
            if (SUCCEEDED(hr)) {
                dpiX = dx;
                dpiY = dy;
            }
        }
        // Don't FreeLibrary -- shcore stays loaded.
    }

    info.dpiScaleX = static_cast<float>(dpiX) / 96.0f;
    info.dpiScaleY = static_cast<float>(dpiY) / 96.0f;

    monitors->push_back(std::move(info));
    return TRUE;
}

std::vector<MonitorInfo> MonitorEnumerator::enumerate() {
    // Ensure DPI awareness is set before enumeration so we get
    // physical (unscaled) coordinates.
    ensureDpiAwareness();

    monitors_.clear();

    EnumDisplayMonitors(nullptr, nullptr, monitorEnumProc,
                        reinterpret_cast<LPARAM>(&monitors_));

    // Sort by position: primary first, then left-to-right.
    std::sort(monitors_.begin(), monitors_.end(),
        [](const MonitorInfo& a, const MonitorInfo& b) {
            // Primary monitor (0,0 origin) should come first.
            if (a.x == 0 && a.y == 0 && !(b.x == 0 && b.y == 0))
                return true;
            if (b.x == 0 && b.y == 0 && !(a.x == 0 && a.y == 0))
                return false;
            if (a.x != b.x) return a.x < b.x;
            return a.y < b.y;
        });

    // Reassign indices after sorting.
    for (int i = 0; i < static_cast<int>(monitors_.size()); ++i) {
        monitors_[i].index = i;
    }

    return monitors_;
}

MonitorInfo MonitorEnumerator::getMonitor(int index) {
    if (monitors_.empty()) {
        enumerate();
    }
    for (const auto& m : monitors_) {
        if (m.index == index) return m;
    }
    throw std::out_of_range("Monitor index " + std::to_string(index) + " not found");
}

MonitorInfo MonitorEnumerator::getPrimaryMonitor() {
    if (monitors_.empty()) {
        enumerate();
    }
    // After sorting, primary is typically index 0.
    // But verify by checking MONITORINFOF_PRIMARY.
    for (const auto& m : monitors_) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m.hMonitor, &mi)) {
            if (mi.dwFlags & MONITORINFOF_PRIMARY) {
                return m;
            }
        }
    }
    // Fallback: return first monitor.
    if (!monitors_.empty()) {
        return monitors_[0];
    }
    throw std::runtime_error("No monitors found");
}

int MonitorEnumerator::getMonitorCount() {
    if (monitors_.empty()) {
        enumerate();
    }
    return static_cast<int>(monitors_.size());
}

} // namespace pb
