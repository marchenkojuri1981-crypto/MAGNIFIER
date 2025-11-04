#pragma once

#include <functional>
#include <map>
#include <windows.h>

enum class HotkeyAction {
    ToggleMagnifier = 1,
    ZoomIn,
    ZoomOut,
    SwitchMode,
    ToggleInvert,
    SwapMonitors,
    ToggleMousePassThrough,
    OpenSettings,
    ForceRestart,
    ShowCurrentTime,
    Quit,
};

class HotkeyManager {
public:
    using Handler = std::function<void(HotkeyAction)>;

    HotkeyManager();
    ~HotkeyManager();

    bool RegisterDefaults(HWND target);
    void UnregisterAll(HWND target);

    void SetHandler(Handler handler) { handler_ = std::move(handler); }
    void HandleHotkey(WPARAM id);

private:
    void RegisterCombo(HWND target, UINT modifiers, UINT key, HotkeyAction action);

    std::map<UINT, HotkeyAction> hotkeys_;
    Handler handler_;
    UINT next_id_{1};
};
