#include "WindowEnumerator.h"

#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <algorithm>
#include <cctype>
#include <locale>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

namespace pb {

namespace {

/// Case-insensitive wide string contains check.
bool wstringContainsCI(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (haystack.empty()) return false;

    std::wstring h = haystack;
    std::wstring n = needle;
    // Convert to lower case (ASCII range is sufficient for matching).
    std::transform(h.begin(), h.end(), h.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::transform(n.begin(), n.end(), n.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return h.find(n) != std::wstring::npos;
}

} // anonymous namespace

bool WindowEnumerator::isCapturableWindow(HWND hwnd) {
    // Must be visible.
    if (!IsWindowVisible(hwnd)) return false;

    // Must not be minimized.
    if (IsIconic(hwnd)) return false;

    // Skip tool windows (WS_EX_TOOLWINDOW).
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    // Must have a non-empty title.
    int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen <= 0) return false;

    // Skip windows that are cloaked (e.g., UWP apps on other virtual desktops).
    BOOL cloaked = FALSE;
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (SUCCEEDED(hr) && cloaked) return false;

    // Must be a top-level window (no owner, or at least not a child).
    // Windows with WS_CHILD style are child windows, skip them.
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style & WS_CHILD) return false;

    return true;
}

std::wstring WindowEnumerator::getProcessName(DWORD pid) {
    std::wstring name;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                  FALSE, pid);
    if (hProcess) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;

        // Try QueryFullProcessImageNameW first (works for protected processes).
        if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
            name = path;
            // Extract just the filename.
            size_t pos = name.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                name = name.substr(pos + 1);
            }
        } else {
            // Fallback to GetModuleFileNameExW.
            if (GetModuleFileNameExW(hProcess, nullptr, path, MAX_PATH)) {
                name = path;
                size_t pos = name.find_last_of(L"\\/");
                if (pos != std::wstring::npos) {
                    name = name.substr(pos + 1);
                }
            }
        }
        CloseHandle(hProcess);
    }
    return name;
}

RECT WindowEnumerator::getWindowRect(HWND hwnd) {
    RECT rect{};

    // Try DWM extended frame bounds for accurate rect (excludes invisible borders).
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &rect, sizeof(rect));
    if (SUCCEEDED(hr)) {
        return rect;
    }

    // Fallback to standard GetWindowRect.
    ::GetWindowRect(hwnd, &rect);
    return rect;
}

bool WindowEnumerator::buildWindowInfo(HWND hwnd, WindowInfo& info) {
    if (!isCapturableWindow(hwnd)) return false;

    info.hwnd = hwnd;

    // Get title.
    int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen > 0) {
        info.title.resize(titleLen + 1);
        int copied = GetWindowTextW(hwnd, &info.title[0], titleLen + 1);
        info.title.resize(copied);
    }

    // Get class name.
    wchar_t className[256] = {};
    int classLen = GetClassNameW(hwnd, className, 256);
    if (classLen > 0) {
        info.className = std::wstring(className, classLen);
    }

    // Get process ID and name.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    info.processId = pid;
    info.processName = getProcessName(pid);

    // Get accurate window rect.
    info.rect = getWindowRect(hwnd);

    return true;
}

BOOL CALLBACK WindowEnumerator::enumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    WindowInfo info{};
    if (buildWindowInfo(hwnd, info)) {
        windows->push_back(std::move(info));
    }

    return TRUE; // continue enumeration
}

std::vector<WindowInfo> WindowEnumerator::enumerate() {
    windows_.clear();

    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&windows_));

    // Sort by title for consistent ordering.
    std::sort(windows_.begin(), windows_.end(),
        [](const WindowInfo& a, const WindowInfo& b) {
            return a.title < b.title;
        });

    return windows_;
}

bool WindowEnumerator::getWindowInfo(HWND hwnd, WindowInfo& info) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    return buildWindowInfo(hwnd, info);
}

std::vector<WindowInfo> WindowEnumerator::findByProcessName(const std::wstring& processName) {
    enumerate(); // refresh

    std::vector<WindowInfo> results;
    for (const auto& w : windows_) {
        if (wstringContainsCI(w.processName, processName)) {
            results.push_back(w);
        }
    }
    return results;
}

std::vector<WindowInfo> WindowEnumerator::findByTitle(const std::wstring& title) {
    enumerate(); // refresh

    std::vector<WindowInfo> results;
    for (const auto& w : windows_) {
        if (wstringContainsCI(w.title, title)) {
            results.push_back(w);
        }
    }
    return results;
}

} // namespace pb
