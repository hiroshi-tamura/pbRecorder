#pragma once

#include "core/Types.h"

#include <optional>
#include <string>
#include <vector>

#include <uiautomationclient.h>
#include <wrl/client.h>

namespace pb {

struct UiElementCandidate {
    UiElementTarget target;
    int depth = 0;
};

struct UiElementPreview {
    RegionRect rect = {};
    std::wstring name;
    std::wstring automationId;
    std::wstring className;
    int controlType = 0;
    HWND rootWindow = nullptr;
    int depth = 0;
};

class UiAutomationHelper {
public:
    UiAutomationHelper();
    ~UiAutomationHelper();

    UiAutomationHelper(const UiAutomationHelper&) = delete;
    UiAutomationHelper& operator=(const UiAutomationHelper&) = delete;

    bool initialize();
    std::vector<UiElementPreview> previewCandidatesFromPoint(POINT pt);
    std::vector<UiElementCandidate> candidatesFromPoint(POINT pt);
    std::optional<UiElementTarget> targetFromPoint(POINT pt, int parentOffset = 0);
    std::optional<RegionRect> resolveRect(const UiElementTarget& target);
    std::wstring describe(const UiElementTarget& target) const;
    std::string lastError() const { return lastError_; }

private:
    using ElementPtr = Microsoft::WRL::ComPtr<IUIAutomationElement>;

    ElementPtr rootFromElement(IUIAutomationElement* element);
    std::vector<int> childPathFromRoot(IUIAutomationElement* root, IUIAutomationElement* element);
    ElementPtr elementFromPath(IUIAutomationElement* root, const std::vector<int>& path);
    std::optional<RegionRect> rectFromElement(IUIAutomationElement* element) const;
    UiElementTarget buildTarget(IUIAutomationElement* element, IUIAutomationElement* root, int depth);
    std::wstring propertyString(IUIAutomationElement* element, PROPERTYID propertyId) const;
    int propertyInt(IUIAutomationElement* element, PROPERTYID propertyId) const;
    HWND nativeWindowHandle(IUIAutomationElement* element) const;
    bool sameElement(IUIAutomationElement* a, IUIAutomationElement* b) const;
    void setError(const std::string& error);

    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
    Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> walker_;
    bool initialized_ = false;
    bool comInitialized_ = false;
    std::string lastError_;
};

} // namespace pb
