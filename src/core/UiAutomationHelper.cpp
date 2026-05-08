#include "core/UiAutomationHelper.h"

#include <algorithm>
#include <sstream>

#include <oleacc.h>

namespace pb {

namespace {

bool validRect(const RegionRect& rect)
{
    return rect.width > 1 && rect.height > 1;
}

int rectDistance(const RegionRect& a, const RegionRect& b)
{
    return std::abs(a.x - b.x) +
           std::abs(a.y - b.y) +
           std::abs(a.width - b.width) +
           std::abs(a.height - b.height);
}

RegionRect relativeFallbackRect(const UiElementTarget& target, const RegionRect& rootRect)
{
    if (!validRect(target.rootInitialRect)) {
        return target.initialRect;
    }
    return {
        rootRect.x + (target.initialRect.x - target.rootInitialRect.x),
        rootRect.y + (target.initialRect.y - target.rootInitialRect.y),
        target.initialRect.width,
        target.initialRect.height
    };
}

bool rectLooksLikeSameTarget(const RegionRect& resolved,
                             const RegionRect& expected,
                             const RegionRect& initial)
{
    const int sizeTolerance = std::max(48, std::max(initial.width, initial.height) / 3);
    const int positionTolerance = std::max(80, std::max(initial.width, initial.height) / 2);
    const bool sizeOk =
        std::abs(resolved.width - initial.width) <= sizeTolerance &&
        std::abs(resolved.height - initial.height) <= sizeTolerance;
    const bool positionOk =
        std::abs(resolved.x - expected.x) <= positionTolerance &&
        std::abs(resolved.y - expected.y) <= positionTolerance;
    return sizeOk && positionOk;
}

RegionRect fromUiaRect(const RECT& rect)
{
    return {
        static_cast<int>(rect.left),
        static_cast<int>(rect.top),
        static_cast<int>(rect.right - rect.left),
        static_cast<int>(rect.bottom - rect.top)
    };
}

std::string hrText(HRESULT hr)
{
    return hrToString(hr);
}

} // namespace

UiAutomationHelper::UiAutomationHelper() = default;

UiAutomationHelper::~UiAutomationHelper()
{
    walker_.Reset();
    automation_.Reset();
    if (comInitialized_ && GetCurrentThreadId() == comThreadId_) {
        CoUninitialize();
    }
}

bool UiAutomationHelper::initialize()
{
    if (initialized_) {
        return true;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        comInitialized_ = true;
        comThreadId_ = GetCurrentThreadId();
    } else if (hr != RPC_E_CHANGED_MODE) {
        setError("UIAutomation COM initialization failed: " + hrText(hr));
        return false;
    }

    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&automation_));
    if (FAILED(hr)) {
        setError("CoCreateInstance(CUIAutomation) failed: " + hrText(hr));
        return false;
    }

    hr = automation_->get_ControlViewWalker(&walker_);
    if (FAILED(hr) || !walker_) {
        setError("IUIAutomation::get_ControlViewWalker failed: " + hrText(hr));
        return false;
    }

    initialized_ = true;
    return true;
}

std::vector<UiElementCandidate> UiAutomationHelper::candidatesFromPoint(POINT pt)
{
    std::vector<UiElementCandidate> candidates;
    if (!initialize()) {
        return candidates;
    }

    ElementPtr element;
    HRESULT hr = automation_->ElementFromPoint(pt, &element);
    if (FAILED(hr) || !element) {
        setError("ElementFromPoint failed: " + hrText(hr));
        return candidates;
    }

    ElementPtr root = rootFromElement(element.Get());
    if (!root) {
        root = element;
    }

    ElementPtr current = element;
    int depth = 0;
    while (current) {
        auto rect = rectFromElement(current.Get());
        if (rect && validRect(*rect)) {
            UiElementCandidate candidate;
            candidate.target = buildTarget(current.Get(), root.Get(), depth);
            candidate.depth = depth;
            candidates.push_back(std::move(candidate));
        }

        if (sameElement(current.Get(), root.Get())) {
            break;
        }

        ElementPtr parent;
        if (FAILED(walker_->GetParentElement(current.Get(), &parent)) || !parent) {
            break;
        }
        current = parent;
        ++depth;
    }

    return candidates;
}

std::vector<UiElementPreview> UiAutomationHelper::previewCandidatesFromPoint(POINT pt)
{
    std::vector<UiElementPreview> previews;
    if (!initialize()) {
        return previews;
    }

    ElementPtr element;
    HRESULT hr = automation_->ElementFromPoint(pt, &element);
    if (FAILED(hr) || !element) {
        setError("ElementFromPoint failed: " + hrText(hr));
        return previews;
    }

    ElementPtr root = rootFromElement(element.Get());
    if (!root) {
        root = element;
    }

    ElementPtr current = element;
    int depth = 0;
    while (current) {
        auto rect = rectFromElement(current.Get());
        if (rect && validRect(*rect)) {
            UiElementPreview preview;
            preview.rect = *rect;
            preview.name = propertyString(current.Get(), UIA_NamePropertyId);
            preview.automationId = propertyString(current.Get(), UIA_AutomationIdPropertyId);
            preview.className = propertyString(current.Get(), UIA_ClassNamePropertyId);
            preview.controlType = propertyInt(current.Get(), UIA_ControlTypePropertyId);
            preview.rootWindow = nativeWindowHandle(root.Get());
            if (preview.rootWindow) {
                preview.rootWindow = GetAncestor(preview.rootWindow, GA_ROOT);
            }
            preview.depth = depth;
            previews.push_back(std::move(preview));
        }

        if (sameElement(current.Get(), root.Get())) {
            break;
        }

        ElementPtr parent;
        if (FAILED(walker_->GetParentElement(current.Get(), &parent)) || !parent) {
            break;
        }
        current = parent;
        ++depth;
    }

    return previews;
}

std::optional<UiElementTarget> UiAutomationHelper::targetFromPoint(POINT pt, int parentOffset)
{
    auto candidates = candidatesFromPoint(pt);
    if (candidates.empty()) {
        return std::nullopt;
    }
    int index = std::clamp(parentOffset, 0, static_cast<int>(candidates.size()) - 1);
    return candidates[static_cast<size_t>(index)].target;
}

std::optional<RegionRect> UiAutomationHelper::resolveRect(const UiElementTarget& target)
{
    if (!initialize()) {
        return std::nullopt;
    }

    ElementPtr root;
    if (target.rootWindow && IsWindow(target.rootWindow)) {
        HRESULT hr = automation_->ElementFromHandle(target.rootWindow, &root);
        if (FAILED(hr)) {
            setError("ElementFromHandle failed: " + hrText(hr));
        }
    }

    std::optional<RegionRect> rootRect;
    if (root) {
        rootRect = rectFromElement(root.Get());
    }
    if (!rootRect && target.rootWindow && IsWindow(target.rootWindow)) {
        RECT wr{};
        if (GetWindowRect(target.rootWindow, &wr)) {
            rootRect = fromUiaRect(wr);
        }
    }
    std::optional<RegionRect> expectedRect;
    if (rootRect && validRect(*rootRect)) {
        expectedRect = relativeFallbackRect(target, *rootRect);
    }

    if (root) {
        ElementPtr element = elementFromPath(root.Get(), target.childPath);
        if (element) {
            auto rect = rectFromElement(element.Get());
            if (rect && validRect(*rect)) {
                if (!expectedRect ||
                    rectLooksLikeSameTarget(*rect, *expectedRect, target.initialRect)) {
                    return rect;
                }
            }
        }
    }

    if (expectedRect && validRect(*expectedRect)) {
        return expectedRect;
    }

    if (validRect(target.initialRect)) {
        return target.initialRect;
    }
    return std::nullopt;
}

std::wstring UiAutomationHelper::describe(const UiElementTarget& target) const
{
    if (!target.name.empty()) {
        return target.name;
    }
    if (!target.automationId.empty()) {
        return target.automationId;
    }
    if (!target.className.empty()) {
        return target.className;
    }
    return L"UI element";
}

UiAutomationHelper::ElementPtr UiAutomationHelper::rootFromElement(IUIAutomationElement* element)
{
    if (!element) {
        return nullptr;
    }

    ElementPtr current = element;
    while (current) {
        HWND hwnd = nativeWindowHandle(current.Get());
        if (hwnd) {
            HWND rootHwnd = GetAncestor(hwnd, GA_ROOT);
            if (!rootHwnd) {
                rootHwnd = hwnd;
            }

            ElementPtr root;
            if (SUCCEEDED(automation_->ElementFromHandle(rootHwnd, &root)) && root) {
                return root;
            }
            return current;
        }

        ElementPtr parent;
        if (FAILED(walker_->GetParentElement(current.Get(), &parent)) || !parent) {
            break;
        }
        current = parent;
    }

    return element;
}

std::vector<int> UiAutomationHelper::childPathFromRoot(IUIAutomationElement* root,
                                                       IUIAutomationElement* element)
{
    std::vector<ElementPtr> chain;
    ElementPtr current = element;

    while (current) {
        if (sameElement(current.Get(), root)) {
            break;
        }

        chain.push_back(current);

        ElementPtr parent;
        if (FAILED(walker_->GetParentElement(current.Get(), &parent)) || !parent) {
            chain.clear();
            break;
        }
        current = parent;
    }

    std::vector<int> path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        ElementPtr parent;
        if (path.empty()) {
            parent = root;
        } else {
            parent = elementFromPath(root, path);
        }
        if (!parent) {
            path.clear();
            break;
        }

        int index = 0;
        bool found = false;
        ElementPtr child;
        if (SUCCEEDED(walker_->GetFirstChildElement(parent.Get(), &child))) {
            while (child) {
                if (sameElement(child.Get(), it->Get())) {
                    found = true;
                    break;
                }
                ElementPtr next;
                if (FAILED(walker_->GetNextSiblingElement(child.Get(), &next))) {
                    break;
                }
                child = next;
                ++index;
            }
        }

        if (!found) {
            path.clear();
            break;
        }
        path.push_back(index);
    }

    return path;
}

UiAutomationHelper::ElementPtr UiAutomationHelper::elementFromPath(
    IUIAutomationElement* root,
    const std::vector<int>& path)
{
    ElementPtr current = root;
    for (int wantedIndex : path) {
        if (!current || wantedIndex < 0) {
            return nullptr;
        }

        ElementPtr child;
        if (FAILED(walker_->GetFirstChildElement(current.Get(), &child)) || !child) {
            return nullptr;
        }

        int index = 0;
        while (child && index < wantedIndex) {
            ElementPtr next;
            if (FAILED(walker_->GetNextSiblingElement(child.Get(), &next))) {
                return nullptr;
            }
            child = next;
            ++index;
        }

        if (!child) {
            return nullptr;
        }
        current = child;
    }

    return current;
}

std::optional<RegionRect> UiAutomationHelper::rectFromElement(IUIAutomationElement* element) const
{
    if (!element) {
        return std::nullopt;
    }

    RECT rect{};
    HRESULT hr = element->get_CurrentBoundingRectangle(&rect);
    if (FAILED(hr)) {
        return std::nullopt;
    }
    RegionRect region = fromUiaRect(rect);
    if (!validRect(region)) {
        return std::nullopt;
    }
    return region;
}

UiElementTarget UiAutomationHelper::buildTarget(IUIAutomationElement* element,
                                                IUIAutomationElement* root,
                                                int /*depth*/)
{
    UiElementTarget target;
    target.rootWindow = nativeWindowHandle(root);
    if (target.rootWindow) {
        target.rootWindow = GetAncestor(target.rootWindow, GA_ROOT);
    }
    target.childPath = childPathFromRoot(root, element);
    target.name = propertyString(element, UIA_NamePropertyId);
    target.automationId = propertyString(element, UIA_AutomationIdPropertyId);
    target.className = propertyString(element, UIA_ClassNamePropertyId);
    target.controlType = propertyInt(element, UIA_ControlTypePropertyId);
    if (auto rect = rectFromElement(element)) {
        target.initialRect = *rect;
    }
    if (auto rootRect = rectFromElement(root)) {
        target.rootInitialRect = *rootRect;
    } else if (target.rootWindow) {
        RECT wr{};
        if (GetWindowRect(target.rootWindow, &wr)) {
            target.rootInitialRect = fromUiaRect(wr);
        }
    }
    return target;
}

std::wstring UiAutomationHelper::propertyString(IUIAutomationElement* element,
                                                PROPERTYID propertyId) const
{
    if (!element) {
        return {};
    }

    BSTR value = nullptr;
    HRESULT hr = E_FAIL;
    if (propertyId == UIA_NamePropertyId) {
        hr = element->get_CurrentName(&value);
    } else if (propertyId == UIA_AutomationIdPropertyId) {
        hr = element->get_CurrentAutomationId(&value);
    } else if (propertyId == UIA_ClassNamePropertyId) {
        hr = element->get_CurrentClassName(&value);
    }

    std::wstring result;
    if (SUCCEEDED(hr) && value) {
        result.assign(value, SysStringLen(value));
    }
    if (value) {
        SysFreeString(value);
    }
    return result;
}

int UiAutomationHelper::propertyInt(IUIAutomationElement* element, PROPERTYID propertyId) const
{
    if (!element) {
        return 0;
    }

    int value = 0;
    if (propertyId == UIA_ControlTypePropertyId) {
        element->get_CurrentControlType(&value);
    }
    return value;
}

HWND UiAutomationHelper::nativeWindowHandle(IUIAutomationElement* element) const
{
    if (!element) {
        return nullptr;
    }
    UIA_HWND hwnd = 0;
    if (FAILED(element->get_CurrentNativeWindowHandle(&hwnd))) {
        return nullptr;
    }
    return reinterpret_cast<HWND>(hwnd);
}

bool UiAutomationHelper::sameElement(IUIAutomationElement* a, IUIAutomationElement* b) const
{
    if (!a || !b || !automation_) {
        return a == b;
    }

    BOOL same = FALSE;
    if (FAILED(automation_->CompareElements(a, b, &same))) {
        return a == b;
    }
    return same != FALSE;
}

void UiAutomationHelper::setError(const std::string& error)
{
    lastError_ = error;
}

} // namespace pb
