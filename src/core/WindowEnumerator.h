#pragma once

#include "Types.h"
#include <vector>
#include <string>

namespace pb {

/// Enumerates visible top-level windows suitable for capture.
class WindowEnumerator {
public:
    WindowEnumerator() = default;
    ~WindowEnumerator() = default;

    /// Enumerate all visible top-level windows that are suitable for capture.
    /// Filters out: invisible, minimized, tool windows, no-title windows.
    /// Uses DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS) for accurate rects.
    /// Returns a vector of WindowInfo sorted by window title.
    std::vector<WindowInfo> enumerate();

    /// Find a window by its HWND.
    /// Returns true and fills info if found, false otherwise.
    bool getWindowInfo(HWND hwnd, WindowInfo& info);

    /// Refresh and get windows filtered by process name (case-insensitive partial match).
    std::vector<WindowInfo> findByProcessName(const std::wstring& processName);

    /// Refresh and get windows filtered by title (case-insensitive partial match).
    std::vector<WindowInfo> findByTitle(const std::wstring& title);

private:
    /// Callback for EnumWindows.
    static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam);

    /// Check if a window should be included in capture candidates.
    static bool isCapturableWindow(HWND hwnd);

    /// Get the process executable name for a given PID.
    static std::wstring getProcessName(DWORD pid);

    /// Get accurate window rect using DWM extended frame bounds.
    static RECT getWindowRect(HWND hwnd);

    /// Build a WindowInfo for a given HWND.
    static bool buildWindowInfo(HWND hwnd, WindowInfo& info);

    std::vector<WindowInfo> windows_;
};

} // namespace pb
