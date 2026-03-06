#pragma once

#include "Types.h"
#include <vector>

namespace pb {

/// Enumerates all connected monitors with physical resolution and DPI info.
class MonitorEnumerator {
public:
    MonitorEnumerator() = default;
    ~MonitorEnumerator() = default;

    /// Enumerate all monitors. Sets per-monitor DPI awareness before
    /// enumeration so that physical (unscaled) resolutions are returned.
    /// Returns a vector of MonitorInfo sorted by monitor index.
    std::vector<MonitorInfo> enumerate();

    /// Get info for a specific monitor by index (0-based).
    /// Returns empty optional-style: throws std::out_of_range if not found.
    MonitorInfo getMonitor(int index);

    /// Get the primary monitor.
    MonitorInfo getPrimaryMonitor();

    /// Get the number of monitors.
    int getMonitorCount();

    /// Ensure process DPI awareness is set to per-monitor V2.
    /// Safe to call multiple times; only the first call takes effect.
    static void ensureDpiAwareness();

private:
    /// Callback for EnumDisplayMonitors.
    static BOOL CALLBACK monitorEnumProc(HMONITOR hMonitor, HDC hdc,
                                          LPRECT lpRect, LPARAM lParam);

    std::vector<MonitorInfo> monitors_;
};

} // namespace pb
