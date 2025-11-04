#pragma once

#include <functional>
#include <string>
#include <optional>
#include <windows.h>
#include <wrl/client.h>
#include <UIAutomation.h>

enum class TrackingMode {
    Auto,
    Caret,
    Mouse,
    Focus,
    Manual,
};

struct TrackingState {
    POINT caret{};
    POINT mouse{};
    RECT focus{};
    TrackingMode mode{TrackingMode::Auto};
};

class TrackingManager {
public:
    using CaretCallback = std::function<void(const POINT&)>;
    using MouseCallback = std::function<void(const POINT&)>;
    using FocusCallback = std::function<void(const RECT&)>;
    using WheelCallback = std::function<bool(int)>;
    using ClickCallback = std::function<void(const POINT&)>;

    TrackingManager();
    ~TrackingManager();

    void Start();
    void Stop();

    void SetCaretCallback(CaretCallback cb) { caret_callback_ = std::move(cb); }
    void SetMouseCallback(MouseCallback cb) { mouse_callback_ = std::move(cb); }
    void SetFocusCallback(FocusCallback cb) { focus_callback_ = std::move(cb); }
    void SetWheelCallback(WheelCallback cb) { wheel_callback_ = std::move(cb); }
    void SetClickCallback(ClickCallback cb) { click_callback_ = std::move(cb); }

    void SetMode(TrackingMode mode) { mode_ = mode; }
    TrackingMode Mode() const { return mode_; }
    std::wstring GetSelectedText() const;
    void RequestCaretRefresh();

private:
    bool TryUpdateCaretFromThread(DWORD event_thread);
    bool TryUpdateCaretFromAccessible(HWND hwnd, LONG id_object, LONG id_child);
    void UpdateCaretFromUIA();
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD eventThread, DWORD eventTime);
    static LRESULT CALLBACK MouseProc(int code, WPARAM wparam, LPARAM lparam);

    static TrackingManager* instance_;

    HWINEVENTHOOK caret_hook_{};
    HWINEVENTHOOK focus_hook_{};
    HWINEVENTHOOK text_selection_hook_{};
    HWINEVENTHOOK value_change_hook_{};
    HHOOK mouse_hook_{};

    CaretCallback caret_callback_;
    MouseCallback mouse_callback_;
    FocusCallback focus_callback_;
    WheelCallback wheel_callback_;
    ClickCallback click_callback_;

    TrackingMode mode_{TrackingMode::Auto};
    bool com_initialized_{false};
    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
};
