#include "tracking_manager.h"

#include <windowsx.h>
#include <OleAuto.h>
#include <OleAcc.h>
#include <cmath>
#include <algorithm>

TrackingManager* TrackingManager::instance_ = nullptr;

TrackingManager::TrackingManager() = default;
TrackingManager::~TrackingManager() {
    Stop();
}

void TrackingManager::Start() {
    instance_ = this;

    if (!com_initialized_) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr != RPC_E_CHANGED_MODE) {
            if (SUCCEEDED(hr)) {
                com_initialized_ = true;
            }
        }
    }

    if (!automation_) {
        Microsoft::WRL::ComPtr<IUIAutomation> automation;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
            automation_ = automation;
        }
    }

    caret_hook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        &TrackingManager::WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    focus_hook_ = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr,
        &TrackingManager::WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    text_selection_hook_ = SetWinEventHook(EVENT_OBJECT_TEXTSELECTIONCHANGED, EVENT_OBJECT_TEXTSELECTIONCHANGED, nullptr,
        &TrackingManager::WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    value_change_hook_ = SetWinEventHook(EVENT_OBJECT_VALUECHANGE, EVENT_OBJECT_VALUECHANGE, nullptr,
        &TrackingManager::WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    mouse_hook_ = SetWindowsHookExW(WH_MOUSE_LL, &TrackingManager::MouseProc, nullptr, 0);
}

void TrackingManager::Stop() {
    if (caret_hook_) {
        UnhookWinEvent(caret_hook_);
        caret_hook_ = nullptr;
    }
    if (focus_hook_) {
        UnhookWinEvent(focus_hook_);
        focus_hook_ = nullptr;
    }
    if (text_selection_hook_) {
        UnhookWinEvent(text_selection_hook_);
        text_selection_hook_ = nullptr;
    }
    if (value_change_hook_) {
        UnhookWinEvent(value_change_hook_);
        value_change_hook_ = nullptr;
    }
    if (mouse_hook_) {
        UnhookWindowsHookEx(mouse_hook_);
        mouse_hook_ = nullptr;
    }
    automation_.Reset();
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
    instance_ = nullptr;
}

bool TrackingManager::TryUpdateCaretFromThread(DWORD event_thread) {
    GUITHREADINFO gi{};
    gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(event_thread, &gi) || !gi.hwndCaret) {
        return false;
    }

    POINT caret{};
    caret.x = gi.rcCaret.left;
    caret.y = gi.rcCaret.top;
    ClientToScreen(gi.hwndCaret, &caret);
    if (caret_callback_) {
        caret_callback_(caret);
    }
    return true;
}

bool TrackingManager::TryUpdateCaretFromAccessible(HWND hwnd, LONG id_object, LONG id_child) {
    if (!caret_callback_) {
        return false;
    }

    Microsoft::WRL::ComPtr<IAccessible> accessible;
    VARIANT child{};
    IAccessible* raw_accessible = nullptr;
    HRESULT hr = AccessibleObjectFromEvent(hwnd, id_object, id_child, &raw_accessible, &child);
    if (FAILED(hr) || !raw_accessible) {
        VariantClear(&child);
        return false;
    }
    accessible.Attach(raw_accessible);

    auto resolve_child_accessible = [&](const VARIANT& variant) -> Microsoft::WRL::ComPtr<IAccessible> {
        Microsoft::WRL::ComPtr<IAccessible> target = accessible;
        if (!target) {
            return target;
        }
        if (variant.vt == VT_DISPATCH && variant.pdispVal) {
            Microsoft::WRL::ComPtr<IAccessible> child_accessible;
            if (SUCCEEDED(variant.pdispVal->QueryInterface(IID_PPV_ARGS(&child_accessible)))) {
                target = child_accessible;
            }
        } else if (variant.vt == VT_I4 && variant.lVal != CHILDID_SELF) {
            Microsoft::WRL::ComPtr<IDispatch> dispatch_child;
            if (SUCCEEDED(target->get_accChild(const_cast<VARIANT&>(variant), dispatch_child.GetAddressOf())) && dispatch_child) {
                Microsoft::WRL::ComPtr<IAccessible> child_accessible;
                if (SUCCEEDED(dispatch_child.As(&child_accessible))) {
                    target = child_accessible;
                }
            }
        }
        return target;
    };

    Microsoft::WRL::ComPtr<IAccessible> target = resolve_child_accessible(child);

    LONG left = 0;
    LONG top = 0;
    LONG width = 0;
    LONG height = 0;

    VARIANT child_for_call = child;
    Microsoft::WRL::ComPtr<IAccessible> location_source = accessible;
    if (target && target.Get() != accessible.Get()) {
        location_source = target;
        child_for_call.vt = VT_I4;
        child_for_call.lVal = CHILDID_SELF;
    }

    hr = location_source->accLocation(&left, &top, &width, &height, child_for_call);
    VariantClear(&child);
    if (FAILED(hr)) {
        return false;
    }

    POINT caret{};
    // Use the reported top-left corner because some apps (e.g. Telegram)
    // expose selection rectangles instead of narrow caret bounds.
    caret.x = left;
    caret.y = top;
    caret_callback_(caret);
    return true;
}

void TrackingManager::UpdateCaretFromUIA() {
    if (!automation_ || !caret_callback_) {
        return;
    }

    struct CoGuard {
        bool active{false};
        ~CoGuard() {
            if (active) {
                CoUninitialize();
            }
        }
    } guard;

    HRESULT init_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (init_hr == RPC_E_CHANGED_MODE) {
        return;
    }
    if (SUCCEEDED(init_hr)) {
        guard.active = true;
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> focused;
    if (FAILED(automation_->GetFocusedElement(&focused)) || !focused) {
        return;
    }

    auto rect_from_range = [](IUIAutomationTextRange* range, POINT& caret) -> bool {
        if (!range) {
            return false;
        }

        SAFEARRAY* rects = nullptr;
        if (FAILED(range->GetBoundingRectangles(&rects)) || !rects) {
            return false;
        }

        double* data = nullptr;
        HRESULT hr = SafeArrayAccessData(rects, reinterpret_cast<void**>(&data));
        if (FAILED(hr) || !data) {
            SafeArrayDestroy(rects);
            return false;
        }

        const LONG count = rects->rgsabound[0].cElements;
        bool emitted = false;
        if (count >= 4) {
            const LONG rect_count = count / 4;
            LONG index = rect_count - 1;
            double left = data[index * 4 + 0];
            double top = data[index * 4 + 1];
            double width = data[index * 4 + 2];
            caret.x = static_cast<LONG>(std::lround(left + std::max(width, 0.0)));
            caret.y = static_cast<LONG>(std::lround(top));
            if (width <= 0.0) {
                caret.x = static_cast<LONG>(std::lround(left));
            }
            emitted = true;
        }

        SafeArrayUnaccessData(rects);
        SafeArrayDestroy(rects);
        return emitted;
    };

    auto emit_from_range = [this, &rect_from_range](IUIAutomationTextRange* range) -> bool {
        if (!range) {
            return false;
        }

        POINT caret{};
        if (!rect_from_range(range, caret)) {
            return false;
        }

        caret_callback_(caret);
        return true;
    };

    auto supports_text_pattern = [](IUIAutomationElement* element) -> bool {
        if (!element) {
            return false;
        }
        VARIANT value;
        VariantInit(&value);
        bool supported = false;
        if (SUCCEEDED(element->GetCurrentPropertyValue(UIA_IsTextPatternAvailablePropertyId, &value)) && value.vt == VT_BOOL) {
            supported = (value.boolVal == VARIANT_TRUE);
        }
        VariantClear(&value);
        if (supported) {
            return true;
        }
        VariantInit(&value);
        if (SUCCEEDED(element->GetCurrentPropertyValue(UIA_IsTextPattern2AvailablePropertyId, &value)) && value.vt == VT_BOOL) {
            supported = (value.boolVal == VARIANT_TRUE);
        } else {
            supported = false;
        }
        VariantClear(&value);
        return supported;
    };

    auto find_text_provider = [this](IUIAutomationElement* root) -> Microsoft::WRL::ComPtr<IUIAutomationElement> {
        Microsoft::WRL::ComPtr<IUIAutomationElement> result;
        if (!root) {
            return result;
        }

        VARIANT bool_variant;
        VariantInit(&bool_variant);
        bool_variant.vt = VT_BOOL;
        bool_variant.boolVal = VARIANT_TRUE;
        Microsoft::WRL::ComPtr<IUIAutomationCondition> text_available_condition;
        if (SUCCEEDED(automation_->CreatePropertyCondition(UIA_IsTextPatternAvailablePropertyId, bool_variant, &text_available_condition)) && text_available_condition) {
            root->FindFirst(TreeScope_Subtree, text_available_condition.Get(), &result);
        }
        VariantClear(&bool_variant);
        if (result) {
            return result;
        }

        VARIANT control_variant;
        VariantInit(&control_variant);
        control_variant.vt = VT_I4;
        control_variant.lVal = UIA_EditControlTypeId;
        Microsoft::WRL::ComPtr<IUIAutomationCondition> edit_condition;
        if (SUCCEEDED(automation_->CreatePropertyCondition(UIA_ControlTypePropertyId, control_variant, &edit_condition)) && edit_condition) {
            root->FindFirst(TreeScope_Subtree, edit_condition.Get(), &result);
        }
        VariantClear(&control_variant);
        if (result) {
            return result;
        }

        VARIANT doc_variant;
        VariantInit(&doc_variant);
        doc_variant.vt = VT_I4;
        doc_variant.lVal = UIA_DocumentControlTypeId;
        Microsoft::WRL::ComPtr<IUIAutomationCondition> doc_condition;
        if (SUCCEEDED(automation_->CreatePropertyCondition(UIA_ControlTypePropertyId, doc_variant, &doc_condition)) && doc_condition) {
            root->FindFirst(TreeScope_Subtree, doc_condition.Get(), &result);
        }
        VariantClear(&doc_variant);
        if (result) {
            return result;
        }

        VARIANT text_variant;
        VariantInit(&text_variant);
        text_variant.vt = VT_I4;
        text_variant.lVal = UIA_TextControlTypeId;
        Microsoft::WRL::ComPtr<IUIAutomationCondition> text_condition;
        if (SUCCEEDED(automation_->CreatePropertyCondition(UIA_ControlTypePropertyId, text_variant, &text_condition)) && text_condition) {
            root->FindFirst(TreeScope_Subtree, text_condition.Get(), &result);
        }
        VariantClear(&text_variant);
        return result;
    };

    Microsoft::WRL::ComPtr<IUIAutomationElement> text_element = focused;
    if (!supports_text_pattern(text_element.Get())) {
        auto candidate = find_text_provider(text_element.Get());
        if (candidate) {
            text_element = candidate;
        }
    }
    if (!supports_text_pattern(text_element.Get())) {
        auto nested = find_text_provider(text_element.Get());
        if (nested && nested.Get() != text_element.Get()) {
            text_element = nested;
        }
    }

    if (!text_element) {
        return;
    }

    Microsoft::WRL::ComPtr<IUIAutomationTextPattern2> text_pattern2;
    if (SUCCEEDED(text_element->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&text_pattern2))) && text_pattern2) {
        BOOL active = FALSE;
        Microsoft::WRL::ComPtr<IUIAutomationTextRange> range;
        if (SUCCEEDED(text_pattern2->GetCaretRange(&active, &range)) && range) {
            if (emit_from_range(range.Get())) {
                return;
            }
        }
    }

    Microsoft::WRL::ComPtr<IUIAutomationTextPattern> text_pattern;
    if (SUCCEEDED(text_element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text_pattern))) && text_pattern) {
        Microsoft::WRL::ComPtr<IUIAutomationTextRangeArray> selections;
        if (SUCCEEDED(text_pattern->GetSelection(&selections)) && selections) {
            int length = 0;
            selections->get_Length(&length);
            for (int i = 0; i < length; ++i) {
                Microsoft::WRL::ComPtr<IUIAutomationTextRange> range;
                if (SUCCEEDED(selections->GetElement(i, &range)) && range) {
                    if (emit_from_range(range.Get())) {
                        return;
                    }
                }
            }
        }
    }
}

std::wstring TrackingManager::GetSelectedText() const {
    if (!automation_) {
        return L"";
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> focused;
    if (FAILED(automation_->GetFocusedElement(&focused)) || !focused) {
        return L"";
    }

    constexpr size_t kMaxSelectionLength = 4096;
    std::wstring collected;

    Microsoft::WRL::ComPtr<IUIAutomationTextPattern> text_pattern;
    if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text_pattern))) && text_pattern) {
        Microsoft::WRL::ComPtr<IUIAutomationTextRangeArray> selections;
        if (SUCCEEDED(text_pattern->GetSelection(&selections)) && selections) {
            int length = 0;
            selections->get_Length(&length);
            for (int i = 0; i < length; ++i) {
                Microsoft::WRL::ComPtr<IUIAutomationTextRange> range;
                if (SUCCEEDED(selections->GetElement(i, &range)) && range) {
                    BSTR text = nullptr;
                    if (SUCCEEDED(range->GetText(-1, &text)) && text) {
                        if (!collected.empty()) {
                            collected.push_back(L'\n');
                        }
                        collected.append(text, SysStringLen(text));
                        SysFreeString(text);
                        if (collected.size() >= kMaxSelectionLength) {
                            collected.resize(kMaxSelectionLength);
                            return collected;
                        }
                    }
                }
            }
            if (!collected.empty()) {
                return collected;
            }
        }
    }

    Microsoft::WRL::ComPtr<IUIAutomationValuePattern> value_pattern;
    if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&value_pattern))) && value_pattern) {
        BSTR value = nullptr;
        if (SUCCEEDED(value_pattern->get_CurrentValue(&value)) && value) {
            std::wstring text(value, SysStringLen(value));
            SysFreeString(value);
            if (text.size() > kMaxSelectionLength) {
                text.resize(kMaxSelectionLength);
            }
            return text;
        }
    }

    return L"";
}

void TrackingManager::RequestCaretRefresh() {
    UpdateCaretFromUIA();
}

void CALLBACK TrackingManager::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD eventThread, DWORD) {
    if (!instance_) {
        return;
    }

    if (event == EVENT_OBJECT_LOCATIONCHANGE && idObject == OBJID_CARET) {
        if (!instance_->TryUpdateCaretFromThread(eventThread)) {
            if (!instance_->TryUpdateCaretFromAccessible(hwnd, idObject, idChild)) {
                instance_->UpdateCaretFromUIA();
            }
        }
    } else if (event == EVENT_OBJECT_FOCUS) {
        if (instance_->focus_callback_) {
            RECT rect{};
            if (GetWindowRect(hwnd, &rect)) {
                instance_->focus_callback_(rect);
            }
        }
        instance_->UpdateCaretFromUIA();
    } else if (event == EVENT_OBJECT_TEXTSELECTIONCHANGED || event == EVENT_OBJECT_VALUECHANGE) {
        instance_->UpdateCaretFromUIA();
    }
}

LRESULT CALLBACK TrackingManager::MouseProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && instance_) {
        auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
        if (!mouse) {
            return CallNextHookEx(nullptr, code, wparam, lparam);
        }

        if (wparam == WM_MOUSEMOVE && instance_->mouse_callback_) {
            instance_->mouse_callback_(mouse->pt);
        } else if (wparam == WM_LBUTTONDOWN && instance_->click_callback_) {
            instance_->click_callback_(mouse->pt);
        } else if (wparam == WM_MOUSEWHEEL && instance_->wheel_callback_) {
            int delta = static_cast<short>(HIWORD(mouse->mouseData));
            if (delta != 0) {
                if (instance_->wheel_callback_(delta)) {
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
