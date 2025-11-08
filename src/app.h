#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <initializer_list>
#include <windows.h>
#include "magnifier_window.h"

class Config;
class MonitorManager;
class CaptureEngine;
class MagnifierWindow;
class TrackingManager;
class InputManager;
class HotkeyManager;
class TrayIcon;
class SettingsDialog;
enum class HotkeyAction;
enum class TrackingMode;
struct MonitorInfo;
struct ViewState;
struct FloatPoint {
    float x;
    float y;
};

struct FloatRect {
    float left;
    float top;
    float right;
    float bottom;
};

class App {
public:
    App();
    ~App();

    int Run();

private:
    bool Initialize();
    void Shutdown();

    bool InitializeComponents();
    bool StartMagnifier();
    void StopMagnifier();
    void Update();
    void UpdateViewState();
    bool SelectMonitors();
    bool ConfigureForCurrentMonitors();
    void ApplyCursorBlocking();
    void ReleaseCursorBlocking();
    void HandleTrayMessage(WPARAM wparam, LPARAM lparam);
    void ShowTrayMenuAt(const POINT& screen_point);
    void CycleTrackingMode();
    void SetTrackingMode(TrackingMode mode);
    void ChangeZoom(float delta);
    void UpdateTray();
    void CheckKeyboardLayout();
    std::wstring LayoutCodeFromHKL(HKL layout) const;
    void ToggleInvertColors();
    void ShowCurrentTimeBadge();
    void ForceRestart();
    void RestartApplication();
    void MarkUserActivity();
    void CheckInactivity();
    void UpdateStatusOverlay();
    void ShowStatusMessage(const std::wstring& text, ULONGLONG duration_ms);
    void QueueStatusMessage(const std::wstring& text, ULONGLONG duration_ms);
    void ShowVersionThenTimeOnStartup();
    std::optional<FloatPoint> ScreenToSource(const POINT& pt) const;
    void OnMouseLeftClick(const POINT& pt);
    void ApplyClickMovementLimit(float& x, float& y, ULONGLONG now);
    bool IsMessengerProcess() const;
    void RestorePreviousCenter();
    void ClearCenterHistory();
    void CenterOnCaretNow();
    void SnapCenterTo(float x, float y, ULONGLONG now, bool ignore_click_limit = false);
    void EnforceMagnifierMonitorExclusivity();
    void EnsureMagnifierTopmost();
    void RequestExit();
    bool WindowMatchesPatterns(HWND hwnd, const std::initializer_list<const wchar_t*>& patterns) const;
    std::optional<RECT> GetForegroundWindowRectIfMatches(const std::initializer_list<const wchar_t*>& patterns) const;
    bool IsPuttyProcess() const;
    void HandleDisplayConfigurationChange(const wchar_t* reason, bool force_restart);
    void OnSystemSuspend();
    void OnSystemResume();

    void RegisterMessageWindow();
    static LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    void OnHotkey(int id);
    void OnSettingsRequested();
    void ToggleMagnifier();
    void SwapMonitors();

    HWND message_window_{};
    HINSTANCE instance_{};

    std::unique_ptr<Config> config_;
    std::unique_ptr<MonitorManager> monitors_;
    std::unique_ptr<CaptureEngine> capture_;
    std::unique_ptr<MagnifierWindow> magnifier_;
    std::unique_ptr<TrackingManager> tracking_;
    std::unique_ptr<InputManager> input_;
    std::unique_ptr<HotkeyManager> hotkeys_;
    std::unique_ptr<TrayIcon> tray_;
    std::unique_ptr<SettingsDialog> settings_;
    HMENU tray_menu_{};

    bool magnifier_active_{false};
    bool ready_{false};

    int source_index_{-1};
    int magnifier_index_{-1};

    ViewState view_state_{};
    POINT mouse_position_{};
    POINT caret_position_{};
    RECT focus_rect_{};

    float zoom_{2.0f};
    TrackingMode tracking_mode_;
    ULONGLONG last_caret_tick_{0};
    ULONGLONG last_caret_target_tick_{0};
    ULONGLONG last_mouse_tick_{0};
    ULONGLONG last_focus_tick_{0};

    bool cursor_block_enabled_{true};
    ULONGLONG cursor_bypass_until_{0};
    bool bypass_active_{false};
    float current_center_x_{0.0f};
    float current_center_y_{0.0f};
    bool has_center_{false};
    float previous_center_x_{0.0f};
    float previous_center_y_{0.0f};
    bool has_previous_center_{false};
    ULONGLONG previous_center_saved_tick_{0};
    float dead_zone_pixels_{16.0f};
    float smoothing_factor_{0.45f};
    ULONGLONG control_press_tick_{0};
    bool control_down_{false};
    bool alt_down_{false};
    bool ctrl_block_active_{false};
    HKL last_keyboard_layout_{nullptr};
    bool invert_colors_{false};
    ULONGLONG last_user_activity_tick_{0};
    bool restart_pending_{false};
    std::wstring status_overlay_text_;
    bool status_overlay_dirty_{true};
    std::optional<std::wstring> pending_status_message_;
    ULONGLONG pending_status_duration_{0};
    // Sequencing for status badges
    ULONGLONG status_overlay_end_tick_{0};
    std::optional<std::wstring> queued_status_message_;
    ULONGLONG queued_status_duration_{0};
    POINT last_click_position_{};
    ULONGLONG last_click_tick_{0};
    bool has_last_click_{false};
    bool click_lock_active_{false};
    std::optional<FloatPoint> last_click_source_;
    bool messenger_zone_active_{false};
    FloatRect messenger_zone_source_{};
    POINT messenger_anchor_{};
    bool end_key_down_{false};
    bool end_alignment_active_{false};
    ULONGLONG end_press_tick_{0};
    ULONGLONG end_ignore_inputs_until_{0};
    bool has_putty_anchor_{false};
    FloatPoint putty_anchor_source_{};
    bool resume_should_start_magnifier_{false};

    const MonitorInfo& SourceMonitor() const;
    const MonitorInfo& MagnifierMonitor() const;
};
