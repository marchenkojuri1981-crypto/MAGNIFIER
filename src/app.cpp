#include "app.h"

#include "capture_engine.h"
#include "config.h"
#include "hotkey_manager.h"
#include "input_manager.h"
#include "logger.h"
#include "magnifier_window.h"
#include "monitor_manager.h"
#include "settings_dialog.h"
#include "tracking_manager.h"
#include "tray_icon.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <shellapi.h>
#include <windowsx.h>
#include <dbt.h>

namespace {
constexpr wchar_t kMessageWindowClass[] = L"ElectronicMagnifierMessageWindow";
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kUpdateIntervalMs = 16;
constexpr float kZoomStep = 0.25f;
constexpr float kMinZoom = 1.0f;
constexpr float kMaxZoom = 12.0f;
constexpr ULONGLONG kCaretFollowTimeoutMs = 600;
constexpr ULONGLONG kMouseFollowTimeoutMs = 160;
constexpr ULONGLONG kFocusFollowTimeoutMs = 900;
constexpr ULONGLONG kBypassDurationMs = 5000;
constexpr ULONGLONG kBypassHoldThresholdMs = 500;
constexpr ULONGLONG kInactivityRestartMs = 60000;
constexpr ULONGLONG kStatusBadgeDurationMs = 2000;
constexpr float kPreviousCenterRecordThreshold = 160.0f;
constexpr ULONGLONG kPreviousCenterRecordCooldownMs = 500;
constexpr float kClickLimitPixelsPerSecond = 50.0f;
constexpr ULONGLONG kEndHoldThresholdMs = 1000;
constexpr ULONGLONG kEndIgnoreCursorMs = 500;

enum TrayCommand : UINT {
    kCmdToggleMagnifier = 40001,
    kCmdSwapMonitors,
    kCmdSettings,
    kCmdClose,
};

bool PointInRect(const RECT& rect, const POINT& pt) {
    return pt.x >= rect.left && pt.x < rect.right && pt.y >= rect.top && pt.y < rect.bottom;
}

void CloseOtherInstances() {
    const DWORD current_pid = GetCurrentProcessId();
    std::vector<HWND> other_windows;
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, kMessageWindowClass, nullptr)) != nullptr) {
        DWORD window_pid = 0;
        GetWindowThreadProcessId(hwnd, &window_pid);
        if (window_pid != current_pid) {
            other_windows.push_back(hwnd);
        }
    }

    for (HWND window : other_windows) {
        SendMessageTimeoutW(window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, nullptr);
    }

    if (other_windows.empty()) {
        return;
    }

    const ULONGLONG deadline = GetTickCount64() + 3000;
    while (GetTickCount64() < deadline) {
        bool any_remaining = false;
        HWND check = nullptr;
        while ((check = FindWindowExW(nullptr, check, kMessageWindowClass, nullptr)) != nullptr) {
            DWORD window_pid = 0;
            GetWindowThreadProcessId(check, &window_pid);
            if (window_pid != current_pid) {
                any_remaining = true;
                break;
            }
        }
        if (!any_remaining) {
            break;
        }
        Sleep(50);
    }
}

const wchar_t* TrackingModeLabel(TrackingMode mode) {
    switch (mode) {
    case TrackingMode::Auto: return L"Auto";
    case TrackingMode::Caret: return L"Caret";
    case TrackingMode::Mouse: return L"Mouse";
    case TrackingMode::Focus: return L"Focus";
    case TrackingMode::Manual: return L"Manual";
    }
    return L"";
}
} // namespace

App::App() = default;

App::~App() {
    Shutdown();
}

int App::Run() {
    if (!Initialize()) {
        return -1;
    }

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Shutdown();
    return static_cast<int>(msg.wParam);
}

bool App::Initialize() {
    instance_ = GetModuleHandle(nullptr);

    CloseOtherInstances();

    RegisterMessageWindow();

    config_ = std::make_unique<Config>();
    monitors_ = std::make_unique<MonitorManager>();
    capture_ = std::make_unique<CaptureEngine>();
    magnifier_ = std::make_unique<MagnifierWindow>();
    tracking_ = std::make_unique<TrackingManager>();
    input_ = std::make_unique<InputManager>();
    hotkeys_ = std::make_unique<HotkeyManager>();
    tray_ = std::make_unique<TrayIcon>();
    settings_ = std::make_unique<SettingsDialog>();

    if (!InitializeComponents()) {
        Logger::Error(L"Initialization failed");
        return false;
    }

    ready_ = true;
    return true;
}

void App::Shutdown() {
    if (magnifier_active_) {
        StopMagnifier();
    }

    KillTimer(message_window_, kTimerId);
    ReleaseCursorBlocking();

    if (input_) {
        input_->Stop();
    }
    if (tracking_) {
        tracking_->Stop();
    }
    if (hotkeys_) {
        hotkeys_->UnregisterAll(message_window_);
    }
    if (tray_) {
        tray_->Destroy();
    }
    if (tray_menu_) {
        DestroyMenu(tray_menu_);
        tray_menu_ = nullptr;
    }

    settings_.reset();
    tray_.reset();
    hotkeys_.reset();
    input_.reset();
    tracking_.reset();
    magnifier_.reset();
    capture_.reset();
    monitors_.reset();
    config_.reset();

    if (message_window_) {
        DestroyWindow(message_window_);
        message_window_ = nullptr;
    }
}

bool App::InitializeComponents() {
    zoom_ = std::clamp(config_->Data().zoom, kMinZoom, kMaxZoom);
    tracking_mode_ = config_->Data().mode;
    cursor_block_enabled_ = config_->Data().block_cursor;
    invert_colors_ = config_->Data().invert_colors;
    last_caret_target_tick_ = 0;
    last_user_activity_tick_ = GetTickCount64();
    status_overlay_dirty_ = true;
    has_putty_anchor_ = false;
    putty_anchor_source_ = {};

    if (!SelectMonitors()) {
        return false;
    }

    if (!ConfigureForCurrentMonitors()) {
        return false;
    }

    tracking_->SetMode(tracking_mode_);
    tracking_->SetCaretCallback([this](const POINT& pt) {
        caret_position_ = pt;
        last_caret_tick_ = GetTickCount64();
        MarkUserActivity();
    });
    tracking_->SetMouseCallback([this](const POINT& pt) {
        POINT previous = mouse_position_;
        bool moved = (pt.x != previous.x) || (pt.y != previous.y);
        mouse_position_ = pt;
        last_mouse_tick_ = GetTickCount64();
        if (moved) {
            click_lock_active_ = false;
        }
        if (messenger_zone_active_) {
            int dx = std::abs(pt.x - messenger_anchor_.x);
            int dy = std::abs(pt.y - messenger_anchor_.y);
            if (dx > 10 || dy > 10) {
                messenger_zone_active_ = false;
            }
        }
        MarkUserActivity();
    });
    tracking_->SetFocusCallback([this](const RECT& rect) {
        focus_rect_ = rect;
        last_focus_tick_ = GetTickCount64();
        MarkUserActivity();
    });
    tracking_->SetWheelCallback([this](int delta) -> bool {
        MarkUserActivity();
        if (!control_down_ || !alt_down_) {
            return false;
        }
        float steps = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
        if (std::abs(steps) < std::numeric_limits<float>::epsilon()) {
            return true;
        }
        ChangeZoom(kZoomStep * steps);
        return true;
    });
    tracking_->SetClickCallback([this](const POINT& pt) {
        OnMouseLeftClick(pt);
    });
    tracking_->Start();

    input_->SetKeyCallback([this](WPARAM msg, LPARAM data) -> bool {
        auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        if (!info) {
            return false;
        }
        bool key_down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        bool key_up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
        bool handled = false;
        if (key_down || key_up) {
            MarkUserActivity();
        }

        auto is_pure_modifier = [](DWORD vk) {
            switch (vk) {
            case VK_SHIFT:
            case VK_LSHIFT:
            case VK_RSHIFT:
            case VK_CONTROL:
            case VK_LCONTROL:
            case VK_RCONTROL:
            case VK_MENU:
            case VK_LMENU:
            case VK_RMENU:
            case VK_LWIN:
            case VK_RWIN:
            case VK_CAPITAL:
                return true;
            default:
                return false;
            }
        };

        if (info->vkCode == VK_LCONTROL || info->vkCode == VK_RCONTROL || info->vkCode == VK_CONTROL) {
            if (key_down && !control_down_) {
                control_down_ = true;
                control_press_tick_ = GetTickCount64();
                if (alt_down_) {
                    ctrl_block_active_ = true;
                }
            } else if (key_up && control_down_) {
                control_down_ = false;
                auto now = GetTickCount64();
                if (now - control_press_tick_ >= kBypassHoldThresholdMs) {
                    cursor_bypass_until_ = now + kBypassDurationMs;
                    bypass_active_ = true;
                    ReleaseCursorBlocking();
                }
                ctrl_block_active_ = false;
            }
            if ((alt_down_ || ctrl_block_active_) && (key_down || key_up)) {
                handled = true;
            }
        } else if (info->vkCode == VK_MENU || info->vkCode == VK_LMENU || info->vkCode == VK_RMENU) {
            if (key_down) {
                alt_down_ = true;
                if (control_down_) {
                    ctrl_block_active_ = true;
                }
            } else if (key_up) {
                alt_down_ = false;
                ctrl_block_active_ = false;
            }
        } else if (info->vkCode == VK_END) {
            if (key_down && !end_key_down_) {
                end_key_down_ = true;
                end_press_tick_ = GetTickCount64();
                end_alignment_active_ = false;
                end_ignore_inputs_until_ = 0;
                has_putty_anchor_ = false;
            } else if (key_up && end_key_down_) {
                end_key_down_ = false;
                end_alignment_active_ = false;
                end_ignore_inputs_until_ = GetTickCount64() + kEndIgnoreCursorMs;
            }
        }

        if ((info->vkCode == VK_LWIN || info->vkCode == VK_RWIN) && key_up) {
            if (tracking_ && tracking_->Mode() != TrackingMode::Manual) {
                tracking_->RequestCaretRefresh();
                CenterOnCaretNow();
            }
        }

        if (key_down && !is_pure_modifier(info->vkCode)) {
            if (tracking_ && tracking_->Mode() != TrackingMode::Manual) {
                tracking_->RequestCaretRefresh();
                CenterOnCaretNow();
            }
        }

        if (key_down && control_down_ && alt_down_) {
            if (info->vkCode == 'Z') {
                RequestExit();
                return true;
            }
        }
        return handled;
    });
    input_->Start();

    hotkeys_->SetHandler([this](HotkeyAction action) {
        MarkUserActivity();
        switch (action) {
        case HotkeyAction::ToggleMagnifier:
            ToggleMagnifier();
            break;
        case HotkeyAction::ZoomIn:
            ChangeZoom(kZoomStep);
            break;
        case HotkeyAction::ZoomOut:
            ChangeZoom(-kZoomStep);
            break;
        case HotkeyAction::SwitchMode:
            CycleTrackingMode();
            break;
        case HotkeyAction::SwapMonitors:
            SwapMonitors();
            break;
        case HotkeyAction::ToggleInvert:
            ToggleInvertColors();
            break;
        case HotkeyAction::ToggleMousePassThrough:
            cursor_block_enabled_ = !cursor_block_enabled_;
            config_->Data().block_cursor = cursor_block_enabled_;
            config_->Save();
            if (!cursor_block_enabled_) {
                ReleaseCursorBlocking();
            }
            UpdateTray();
            break;
        case HotkeyAction::OpenSettings:
            OnSettingsRequested();
            break;
        case HotkeyAction::ShowCurrentTime:
            ShowCurrentTimeBadge();
            break;
        case HotkeyAction::ForceRestart:
            ForceRestart();
            break;
        case HotkeyAction::Quit:
            RequestExit();
            break;
        }
    });
    hotkeys_->RegisterDefaults(message_window_);

    if (!tray_->Create(message_window_)) {
        Logger::Error(L"Failed to create tray icon");
    }
    tray_menu_ = CreatePopupMenu();
    if (tray_menu_) {
        AppendMenuW(tray_menu_, MF_STRING, kCmdToggleMagnifier, L"Toggle magnifier");
        AppendMenuW(tray_menu_, MF_STRING, kCmdSwapMonitors, L"Swap monitors");
        AppendMenuW(tray_menu_, MF_STRING, kCmdSettings, L"Settings...");
        AppendMenuW(tray_menu_, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(tray_menu_, MF_STRING, kCmdClose, L"Close");
    }

    SetTimer(message_window_, kTimerId, kUpdateIntervalMs, nullptr);

    HWND foreground = GetForegroundWindow();
    DWORD thread_id = 0;
    if (foreground) {
        thread_id = GetWindowThreadProcessId(foreground, nullptr);
    }
    last_keyboard_layout_ = GetKeyboardLayout(thread_id);

    magnifier_active_ = true;
    ShowVersionThenTimeOnStartup();
    UpdateStatusOverlay();
    UpdateTray();
    return true;
}

bool App::SelectMonitors() {
    monitors_->Refresh();
    const auto& list = monitors_->Monitors();
    if (list.size() < 2) {
        Logger::Error(L"At least two monitors (including DISPLAY2) are required");
        ShowStatusMessage(L"Подключите второй монитор", kStatusBadgeDurationMs);
        return false;
    }

    auto find_by_name = [&](const std::wstring& name) -> int {
        if (name.empty()) {
            return -1;
        }
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].device_name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    const std::wstring display_prefix = L"\\\\.\\DISPLAY";
    auto display_number = [&](const std::wstring& name) -> int {
        if (name.rfind(display_prefix, 0) != 0) {
            return -1;
        }
        const wchar_t* digits = name.c_str() + display_prefix.size();
        if (!*digits || !iswdigit(*digits)) {
            return -1;
        }
        wchar_t* end = nullptr;
        long value = wcstol(digits, &end, 10);
        if (end == digits || value <= 0) {
            return -1;
        }
        return static_cast<int>(value);
    };

    auto find_by_number = [&](int number) -> int {
        for (size_t i = 0; i < list.size(); ++i) {
            if (display_number(list[i].device_name) == number) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    int magnifier = find_by_number(2);
    if (magnifier < 0) {
        Logger::Error(L"Monitor #2 (\\\\.\\DISPLAY2) is not available");
        ShowStatusMessage(L"Монитор №2 недоступен", kStatusBadgeDurationMs);
        return false;
    }

    int source = find_by_name(config_->Data().source_monitor);
    if (source == magnifier) {
        source = -1;
    }

    if (source < 0) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (static_cast<int>(i) == magnifier) {
                continue;
            }
            if (list[i].primary) {
                source = static_cast<int>(i);
                break;
            }
        }
    }

    if (source < 0) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (static_cast<int>(i) != magnifier) {
                source = static_cast<int>(i);
                break;
            }
        }
    }

    if (source < 0) {
        Logger::Error(L"Unable to select a capture monitor different from DISPLAY2");
        ShowStatusMessage(L"Нет монитора для захвата", kStatusBadgeDurationMs);
        return false;
    }

    source_index_ = source;
    magnifier_index_ = magnifier;

    config_->Data().source_monitor = list[source_index_].device_name;
    config_->Data().magnifier_monitor = list[magnifier_index_].device_name;
    config_->Save();
    return true;
}

bool App::ConfigureForCurrentMonitors() {
    if (!capture_->InitializeForMonitor(SourceMonitor())) {
        return false;
    }

    if (magnifier_) {
        magnifier_->Shutdown();
    }
    magnifier_ = std::make_unique<MagnifierWindow>();
    if (!magnifier_->Initialize(nullptr, capture_->Device(), capture_->Context())) {
        return false;
    }
    magnifier_->AttachToMonitor(MagnifierMonitor());
    return true;
}

bool App::StartMagnifier() {
    if (source_index_ < 0 || magnifier_index_ < 0) {
        if (!SelectMonitors()) {
            return false;
        }
    }

    if (!ConfigureForCurrentMonitors()) {
        return false;
    }
    ShowWindow(magnifier_->hwnd(), SW_SHOW);
    magnifier_active_ = true;
    EnsureMagnifierTopmost();
    EnforceMagnifierMonitorExclusivity();
    ClearCenterHistory();
    ApplyCursorBlocking();
    ShowVersionThenTimeOnStartup();
    UpdateTray();
    return true;
}

void App::StopMagnifier() {
    magnifier_active_ = false;
    if (magnifier_) {
        ShowWindow(magnifier_->hwnd(), SW_HIDE);
        magnifier_->ShowLayoutOverlay(L"", 0);
    }
    ReleaseCursorBlocking();
    ClearCenterHistory();
    ShowStatusMessage(L"", 0);
    UpdateTray();
}

void App::Update() {
    CheckInactivity();
    CheckKeyboardLayout();
    EnforceMagnifierMonitorExclusivity();

    if (!magnifier_active_) {
        return;
    }

    // Handle queued status badge sequencing
    {
        ULONGLONG now = GetTickCount64();
        if (queued_status_message_ && status_overlay_end_tick_ != 0 && now >= status_overlay_end_tick_) {
            ShowStatusMessage(*queued_status_message_, queued_status_duration_);
            queued_status_message_.reset();
            queued_status_duration_ = 0;
        }
    }

    auto frame = capture_->AcquireFrame();
    if (!frame.has_value() && capture_->NeedsReinitialize()) {
        if (capture_->Reinitialize()) {
            frame = capture_->AcquireFrame();
        }
    }
    if (!frame.has_value()) {
        ApplyCursorBlocking();
        return;
    }

    UpdateViewState();
    magnifier_->PresentFrame(frame.value(), view_state_);
    ApplyCursorBlocking();
}

void App::UpdateViewState() {
    if (source_index_ < 0) {
        return;
    }

    auto desc = capture_->FrameDesc();
    if (desc.Width == 0 || desc.Height == 0) {
        return;
    }

    auto now = GetTickCount64();

    view_state_.cursor_visible = false;
    view_state_.invert_colors = invert_colors_;
    view_state_.cursor_x = 0.0f;
    view_state_.cursor_y = 0.0f;

    auto screen_to_source = [&](const POINT& pt) -> std::optional<FloatPoint> {
        return ScreenToSource(pt);
    };

    auto rect_center_to_source = [&](const RECT& rect) -> std::optional<FloatPoint> {
        POINT center{};
        center.x = rect.left + (rect.right - rect.left) / 2;
        center.y = rect.top + (rect.bottom - rect.top) / 2;
        return screen_to_source(center);
    };

    if (!end_key_down_ && now >= end_ignore_inputs_until_) {
        has_putty_anchor_ = false;
    }

    bool inputs_suppressed = (now < end_ignore_inputs_until_) || end_alignment_active_;

    if (auto cursor = screen_to_source(mouse_position_)) {
        view_state_.cursor_visible = true;
        view_state_.cursor_x = cursor->x;
        view_state_.cursor_y = cursor->y;
    }

    FloatPoint target{ desc.Width / 2.0f, desc.Height / 2.0f };
    bool have_target = false;
    bool target_is_caret = false;

    auto use_caret = [&]() {
        if (inputs_suppressed) {
            return false;
        }
        target_is_caret = false;
        if (now - last_caret_tick_ > kCaretFollowTimeoutMs) {
            return false;
        }
        auto caret_source = screen_to_source(caret_position_);
        if (!caret_source) {
            return false;
        }
        target = *caret_source;
        // Shift right to include caret width
        target.x += 4.0f;
        have_target = true;
        target_is_caret = true;
        last_caret_target_tick_ = now;
        return true;
    };

    auto use_mouse = [&]() {
        if (inputs_suppressed) {
            return false;
        }
        if (now - last_mouse_tick_ > kMouseFollowTimeoutMs) {
            return false;
        }
        if (tracking_mode_ == TrackingMode::Auto && last_mouse_tick_ <= last_caret_target_tick_) {
            return false;
        }
        auto mouse_source = screen_to_source(mouse_position_);
        if (!mouse_source) {
            return false;
        }
        target = *mouse_source;
        have_target = true;
        if (tracking_mode_ == TrackingMode::Auto) {
            last_caret_target_tick_ = 0;
        }
        return true;
    };

    auto use_focus = [&]() {
        if (inputs_suppressed) {
            return false;
        }
        if (now - last_focus_tick_ > kFocusFollowTimeoutMs) {
            return false;
        }
        auto focus_source = rect_center_to_source(focus_rect_);
        if (focus_source) {
            target = *focus_source;
            have_target = true;
            return true;
        }
        return false;
    };

    switch (tracking_mode_) {
    case TrackingMode::Auto:
        if (!use_caret()) {
            if (!use_mouse()) {
                use_focus();
            }
        }
        break;
    case TrackingMode::Caret:
        use_caret();
        break;
    case TrackingMode::Mouse:
        use_mouse();
        break;
    case TrackingMode::Focus:
        use_focus();
        break;
    case TrackingMode::Manual:
        have_target = false;
        break;
    }

    float frame_width = static_cast<float>(desc.Width);
    float frame_height = static_cast<float>(desc.Height);
    float view_width = frame_width / zoom_;
    float view_height = frame_height / zoom_;
    view_width = std::min(view_width, frame_width);
    view_height = std::min(view_height, frame_height);
    float half_w = view_width / 2.0f;
    float half_h = view_height / 2.0f;

    if (end_key_down_ || end_alignment_active_) {
        if (auto rect = GetForegroundWindowRectIfMatches({ L"putty" })) {
            POINT bottom_left{};
            bottom_left.x = rect->left;
            bottom_left.y = std::max(rect->top, rect->bottom - 1);
            if (auto bottom_left_source = screen_to_source(bottom_left)) {
                putty_anchor_source_ = *bottom_left_source;
                has_putty_anchor_ = true;
            }
        }
    }

    if (end_key_down_) {
        if (!end_alignment_active_ && (now - end_press_tick_) >= kEndHoldThresholdMs && has_putty_anchor_) {
            end_alignment_active_ = true;
        }
    } else if (end_alignment_active_) {
        end_alignment_active_ = false;
    }

    bool putty_alignment_applied = false;
    if (end_alignment_active_ && has_putty_anchor_) {
        float desired_left = std::clamp(putty_anchor_source_.x, 0.0f, frame_width - view_width);
        float desired_bottom = std::clamp(putty_anchor_source_.y, view_height, frame_height);
        current_center_x_ = desired_left + half_w;
        current_center_y_ = desired_bottom - half_h;
        has_center_ = true;
        putty_alignment_applied = true;
        messenger_zone_active_ = false;
    }

    if (!putty_alignment_applied) {
        if (!has_center_) {
            SnapCenterTo(target.x, target.y, now);
        } else if (have_target) {
            if (target_is_caret) {
                SnapCenterTo(target.x, target.y, now, true);
            } else {
                float dx = target.x - current_center_x_;
                float dy = target.y - current_center_y_;
                float distance = std::sqrt(dx * dx + dy * dy);
                if (distance > dead_zone_pixels_) {
                    if (distance >= kPreviousCenterRecordThreshold) {
                        if (!has_previous_center_ || (now - previous_center_saved_tick_) >= kPreviousCenterRecordCooldownMs) {
                            previous_center_x_ = current_center_x_;
                            previous_center_y_ = current_center_y_;
                            has_previous_center_ = true;
                            previous_center_saved_tick_ = now;
                        }
                    }
                    float new_center_x = current_center_x_ + dx * smoothing_factor_;
                    float new_center_y = current_center_y_ + dy * smoothing_factor_;
                    ApplyClickMovementLimit(new_center_x, new_center_y, now);
                    current_center_x_ = new_center_x;
                    current_center_y_ = new_center_y;
                }
            }
        }
    }

    if (messenger_zone_active_) {
        float min_allowed_x = std::max(messenger_zone_source_.left, half_w);
        float max_allowed_x = std::min(messenger_zone_source_.right, frame_width - half_w);
        float min_allowed_y = std::max(messenger_zone_source_.top, half_h);
        float max_allowed_y = std::min(messenger_zone_source_.bottom, frame_height - half_h);
        if (min_allowed_x <= max_allowed_x && min_allowed_y <= max_allowed_y) {
            current_center_x_ = std::clamp(current_center_x_, min_allowed_x, max_allowed_x);
            current_center_y_ = std::clamp(current_center_y_, min_allowed_y, max_allowed_y);
        } else {
            messenger_zone_active_ = false;
        }
    }

    current_center_x_ = std::clamp(current_center_x_, half_w, frame_width - half_w);
    current_center_y_ = std::clamp(current_center_y_, half_h, frame_height - half_h);

    float left = current_center_x_ - half_w;
    float top = current_center_y_ - half_h;

    view_state_.source_region.left = static_cast<LONG>(std::floor(left));
    view_state_.source_region.top = static_cast<LONG>(std::floor(top));
    view_state_.source_region.right = static_cast<LONG>(std::ceil(left + view_width));
    view_state_.source_region.bottom = static_cast<LONG>(std::ceil(top + view_height));
    view_state_.zoom = zoom_;
}

void App::ApplyCursorBlocking() {
    if (!cursor_block_enabled_ || !magnifier_active_ || source_index_ < 0) {
        ClipCursor(nullptr);
        return;
    }

    auto now = GetTickCount64();
    if (bypass_active_) {
        if (now >= cursor_bypass_until_) {
            bypass_active_ = false;
        } else {
            ClipCursor(nullptr);
            return;
        }
    }

    RECT bounds = SourceMonitor().bounds;
    ClipCursor(&bounds);
}

void App::ReleaseCursorBlocking() {
    ClipCursor(nullptr);
}

void App::HandleTrayMessage(WPARAM wparam, LPARAM lparam) {
    if (wparam != 1) {
        return;
    }
    switch (lparam) {
    case WM_CONTEXTMENU: {
        POINT pt{};
        pt.x = GET_X_LPARAM(lparam);
        pt.y = GET_Y_LPARAM(lparam);
        if (pt.x == -1 && pt.y == -1) {
            GetCursorPos(&pt);
        }
        MarkUserActivity();
        ShowTrayMenuAt(pt);
        break;
    }
    case WM_LBUTTONUP: {
        POINT pt{};
        GetCursorPos(&pt);
        MarkUserActivity();
        ShowTrayMenuAt(pt);
        break;
    }
    default:
        break;
    }
}

void App::ShowTrayMenuAt(const POINT& screen_point) {
    SetForegroundWindow(message_window_);
    if (tray_menu_) {
        TrackPopupMenuEx(tray_menu_, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, screen_point.x, screen_point.y, message_window_, nullptr);
    }
    PostMessageW(message_window_, WM_NULL, 0, 0);
}

void App::CycleTrackingMode() {
    TrackingMode next = TrackingMode::Auto;
    switch (tracking_mode_) {
    case TrackingMode::Auto: next = TrackingMode::Caret; break;
    case TrackingMode::Caret: next = TrackingMode::Mouse; break;
    case TrackingMode::Mouse: next = TrackingMode::Focus; break;
    case TrackingMode::Focus: next = TrackingMode::Manual; break;
    case TrackingMode::Manual: next = TrackingMode::Auto; break;
    }
    SetTrackingMode(next);
}

void App::SetTrackingMode(TrackingMode mode) {
    tracking_mode_ = mode;
    tracking_->SetMode(mode);
    config_->Data().mode = mode;
    config_->Save();
    ShowStatusMessage(TrackingModeLabel(mode), kStatusBadgeDurationMs);
    UpdateTray();
}

void App::ChangeZoom(float delta) {
    float new_zoom = std::clamp(zoom_ + delta, kMinZoom, kMaxZoom);
    if (std::abs(new_zoom - zoom_) < 0.001f) {
        return;
    }
    zoom_ = new_zoom;
    config_->Data().zoom = zoom_;
    config_->Save();
    has_center_ = false;
    if (magnifier_ && magnifier_active_) {
        int percent = static_cast<int>(std::round(zoom_ * 100.0f));
        wchar_t text[16]{};
        swprintf_s(text, L"%d%%", percent);
        magnifier_->ShowLayoutOverlay(text, 1000);
    }
    UpdateTray();
}

void App::UpdateTray() {
    if (!tray_) {
        return;
    }
    if (source_index_ < 0 || magnifier_index_ < 0) {
        return;
    }
    std::wstring status = L"Magnifier ";
    status += magnifier_active_ ? L"ON" : L"OFF";
    status += L" | Zoom ";
    wchar_t zoom_buffer[16]{};
    swprintf_s(zoom_buffer, L"%.2f", zoom_);
    status += zoom_buffer;
    status += L" | Mode ";
    switch (tracking_mode_) {
    case TrackingMode::Auto: status += L"Auto"; break;
    case TrackingMode::Caret: status += L"Caret"; break;
    case TrackingMode::Mouse: status += L"Mouse"; break;
    case TrackingMode::Focus: status += L"Focus"; break;
    case TrackingMode::Manual: status += L"Manual"; break;
    }
    if (invert_colors_) {
        status += L" | INV";
    }
    tray_->SetTooltip(status);
}

void App::CheckKeyboardLayout() {
    HWND foreground = GetForegroundWindow();
    DWORD thread_id = 0;
    if (foreground) {
        thread_id = GetWindowThreadProcessId(foreground, nullptr);
    }
    HKL layout = GetKeyboardLayout(thread_id);
    if (!layout) {
        return;
    }
    if (layout == last_keyboard_layout_) {
        return;
    }
    last_keyboard_layout_ = layout;

    std::wstring code = LayoutCodeFromHKL(layout);
    if (code.empty()) {
        return;
    }

    if (magnifier_ && magnifier_active_) {
        magnifier_->ShowLayoutOverlay(code, 2000);
    }
}

std::wstring App::LayoutCodeFromHKL(HKL layout) const {
    if (!layout) {
        return L"";
    }

    LANGID lang = LOWORD(reinterpret_cast<UINT_PTR>(layout));
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
    if (LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), locale_name, LOCALE_NAME_MAX_LENGTH, 0) == 0) {
        wchar_t layout_name[KL_NAMELENGTH]{};
        if (GetKeyboardLayoutNameW(layout_name)) {
            return std::wstring(layout_name, layout_name + 2);
        }
        return L"";
    }

    wchar_t iso639[16]{};
    if (GetLocaleInfoEx(locale_name, LOCALE_SISO639LANGNAME, iso639, ARRAYSIZE(iso639)) > 0) {
        std::wstring result = iso639;
        for (auto& ch : result) {
            ch = static_cast<wchar_t>(std::towupper(ch));
        }
        if (result.size() > 4) {
            result.resize(4);
        }
        return result;
    }

    std::wstring fallback = locale_name;
    for (auto& ch : fallback) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }
    if (fallback.size() > 4) {
        fallback.resize(4);
    }
    return fallback;
}

void App::RegisterMessageWindow() {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = &App::MessageWndProc;
    cls.hInstance = instance_;
    cls.lpszClassName = kMessageWindowClass;
    RegisterClassExW(&cls);

    message_window_ = CreateWindowExW(
        0, kMessageWindowClass, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance_, this);
}

LRESULT CALLBACK App::MessageWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }

    auto* self = reinterpret_cast<App*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_HOTKEY:
        self->OnHotkey(static_cast<int>(wparam));
        return 0;
    case WM_TIMER:
        if (wparam == kTimerId) {
            self->Update();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kCmdToggleMagnifier:
            self->ToggleMagnifier();
            break;
        case kCmdSwapMonitors:
            self->SwapMonitors();
            break;
        case kCmdSettings:
            self->OnSettingsRequested();
            break;
        case kCmdClose:
            self->StopMagnifier();
            PostQuitMessage(0);
            break;
        default:
            break;
        }
        return 0;
    case WM_TRAYICON:
        self->HandleTrayMessage(wparam, lparam);
        return 0;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND) {
            self->OnSystemSuspend();
            return TRUE;
        }
        if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
            self->OnSystemResume();
            return TRUE;
        }
        break;
    case WM_DISPLAYCHANGE:
        self->HandleDisplayConfigurationChange(L"WM_DISPLAYCHANGE", false);
        return 0;
    case WM_DEVICECHANGE:
        if (wparam == DBT_DEVNODES_CHANGED) {
            self->HandleDisplayConfigurationChange(L"WM_DEVICECHANGE", false);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void App::OnHotkey(int id) {
    if (!hotkeys_) {
        return;
    }
    hotkeys_->HandleHotkey(static_cast<WPARAM>(id));
}

void App::OnSettingsRequested() {
    MarkUserActivity();
    settings_->Show(instance_, nullptr);
}

void App::ToggleMagnifier() {
    MarkUserActivity();
    if (magnifier_active_) {
        StopMagnifier();
    } else {
        StartMagnifier();
    }
}

void App::SwapMonitors() {
    MarkUserActivity();
    std::wstring original_source = config_->Data().source_monitor;
    std::wstring original_magnifier = config_->Data().magnifier_monitor;

    bool was_active = magnifier_active_;
    if (was_active) {
        StopMagnifier();
    }

    std::swap(config_->Data().source_monitor, config_->Data().magnifier_monitor);
    config_->Save();

    monitors_->Refresh();
    if (!SelectMonitors()) {
        config_->Data().source_monitor = std::move(original_source);
        config_->Data().magnifier_monitor = std::move(original_magnifier);
        config_->Save();
        monitors_->Refresh();
        SelectMonitors();
        if (was_active) {
            StartMagnifier();
        }
        UpdateTray();
        return;
    }

    ClearCenterHistory();

    bool reconfigured = false;
    if (was_active) {
        reconfigured = StartMagnifier();
    } else {
        reconfigured = ConfigureForCurrentMonitors();
    }

    if (!reconfigured) {
        config_->Data().source_monitor = std::move(original_source);
        config_->Data().magnifier_monitor = std::move(original_magnifier);
        config_->Save();
        monitors_->Refresh();
        if (SelectMonitors()) {
            if (was_active) {
                StartMagnifier();
            } else {
                ConfigureForCurrentMonitors();
            }
        }
        UpdateTray();
        return;
    }

    UpdateTray();
}

void App::ToggleInvertColors() {
    MarkUserActivity();
    invert_colors_ = !invert_colors_;
    config_->Data().invert_colors = invert_colors_;
    config_->Save();
    ShowStatusMessage(invert_colors_ ? L"Invert On" : L"Invert Off", kStatusBadgeDurationMs);
    UpdateTray();
}

void App::ShowCurrentTimeBadge() {
    SYSTEMTIME current_time{};
    GetLocalTime(&current_time);

    wchar_t buffer[6] = {};
    if (swprintf_s(buffer, L"%02u:%02u", current_time.wHour, current_time.wMinute) < 0) {
        return;
    }

    ShowStatusMessage(buffer, kStatusBadgeDurationMs);
}

void App::EnsureMagnifierTopmost() {
    if (!magnifier_ || !magnifier_->hwnd()) {
        return;
    }

    SetWindowPos(magnifier_->hwnd(), HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
}

void App::EnforceMagnifierMonitorExclusivity() {
    if (!magnifier_active_ || !magnifier_ || !magnifier_->hwnd()) {
        return;
    }
    if (source_index_ < 0 || magnifier_index_ < 0) {
        return;
    }

    const MonitorInfo& magnifier_monitor = MagnifierMonitor();
    const MonitorInfo& source_monitor = SourceMonitor();
    if (!magnifier_monitor.handle || !source_monitor.handle) {
        return;
    }

    EnsureMagnifierTopmost();

    struct ExclusivityContext {
        HMONITOR magnifier_monitor;
        RECT source_bounds;
        RECT source_work;
        HWND magnifier_hwnd;
        DWORD process_id;
    } context{
        magnifier_monitor.handle,
        source_monitor.bounds,
        source_monitor.work_area,
        magnifier_->hwnd(),
        GetCurrentProcessId()
    };

    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        const auto* ctx = reinterpret_cast<const ExclusivityContext*>(param);
        if (!IsWindow(hwnd)) {
            return TRUE;
        }
        if (hwnd == ctx->magnifier_hwnd) {
            return TRUE;
        }
        if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
            return TRUE;
        }

        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (style & WS_CHILD) {
            return TRUE;
        }

        LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex_style & WS_EX_TOOLWINDOW) {
            return TRUE;
        }

        if (!IsWindowVisible(hwnd)) {
            return TRUE;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == ctx->process_id) {
            return TRUE;
        }

        HMONITOR window_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        if (window_monitor != ctx->magnifier_monitor) {
            return TRUE;
        }

        RECT window_rect{};
        if (!GetWindowRect(hwnd, &window_rect)) {
            return TRUE;
        }

        RECT target_area = ctx->source_work;
        if (IsRectEmpty(&target_area)) {
            target_area = ctx->source_bounds;
        }

        int width = window_rect.right - window_rect.left;
        int height = window_rect.bottom - window_rect.top;
        int area_width = target_area.right - target_area.left;
        int area_height = target_area.bottom - target_area.top;

        int new_left = target_area.left;
        int new_top = target_area.top;

        if (area_width > 0 && width < area_width) {
            new_left = std::clamp(window_rect.left, target_area.left, target_area.right - width);
        }
        if (area_height > 0 && height < area_height) {
            new_top = std::clamp(window_rect.top, target_area.top, target_area.bottom - height);
        }

        SetWindowPos(hwnd, nullptr, new_left, new_top, 0, 0,
            SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
}

void App::RequestExit() {
    StopMagnifier();
    PostQuitMessage(0);
}

void App::ForceRestart() {
    MarkUserActivity();
    RestartApplication();
}

void App::RestartApplication() {
    if (restart_pending_) {
        return;
    }
    restart_pending_ = true;
    last_user_activity_tick_ = GetTickCount64();

    wchar_t module_path[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, module_path, ARRAYSIZE(module_path));
    if (length == 0 || length >= ARRAYSIZE(module_path)) {
        restart_pending_ = false;
        return;
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};

    if (CreateProcessW(module_path, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup_info, &process_info)) {
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
    } else {
        restart_pending_ = false;
        last_user_activity_tick_ = GetTickCount64();
        return;
    }

    StopMagnifier();
    PostQuitMessage(0);
}

void App::MarkUserActivity() {
    last_user_activity_tick_ = GetTickCount64();
}

void App::CheckInactivity() {
    if (restart_pending_) {
        return;
    }
    if (last_user_activity_tick_ == 0) {
        last_user_activity_tick_ = GetTickCount64();
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (now - last_user_activity_tick_ >= kInactivityRestartMs) {
        RestartApplication();
    }
}

void App::ShowStatusMessage(const std::wstring& text, ULONGLONG duration_ms) {
    pending_status_message_ = text;
    pending_status_duration_ = duration_ms;
    status_overlay_dirty_ = true;
    UpdateStatusOverlay();
}

void App::QueueStatusMessage(const std::wstring& text, ULONGLONG duration_ms) {
    queued_status_message_ = text;
    queued_status_duration_ = duration_ms;
}

void App::ShowVersionThenTimeOnStartup() {
    // Show current time for 2 seconds on startup badge
    SYSTEMTIME current_time{};
    GetLocalTime(&current_time);
    wchar_t time_buf[6]{};
    if (swprintf_s(time_buf, L"%02u:%02u", current_time.wHour, current_time.wMinute) >= 0) {
        queued_status_message_.reset();
        queued_status_duration_ = 0;
        ShowStatusMessage(time_buf, 2000);
    }
}

std::optional<FloatPoint> App::ScreenToSource(const POINT& pt) const {
    if (source_index_ < 0) {
        return std::nullopt;
    }
    const auto& source_monitor = SourceMonitor();
    if (!PointInRect(source_monitor.bounds, pt)) {
        return std::nullopt;
    }
    FloatPoint converted{};
    converted.x = static_cast<float>((pt.x - source_monitor.bounds.left) * source_monitor.scale);
    converted.y = static_cast<float>((pt.y - source_monitor.bounds.top) * source_monitor.scale);
    return converted;
}

void App::OnMouseLeftClick(const POINT& pt) {
    mouse_position_ = pt;
    last_click_position_ = pt;
    last_click_tick_ = GetTickCount64();
    has_last_click_ = true;
    click_lock_active_ = true;
    messenger_zone_active_ = false;
    MarkUserActivity();

    if (auto converted = ScreenToSource(pt)) {
        last_click_source_ = converted;
    } else {
        last_click_source_.reset();
    }

    if (source_index_ >= 0 && IsMessengerProcess()) {
        const auto& monitor = SourceMonitor();
        LONG monitor_width = monitor.bounds.right - monitor.bounds.left;
        LONG monitor_height = monitor.bounds.bottom - monitor.bounds.top;
        if (monitor_width > 0 && monitor_height > 0) {
            LONG strip_height = static_cast<LONG>(std::lround(static_cast<double>(monitor_height) * 0.1));
            strip_height = std::max<LONG>(strip_height, 1);
            LONG strip_top = monitor.bounds.bottom - strip_height;
            strip_top = std::max(strip_top, monitor.bounds.top);
            if (pt.y >= strip_top && pt.y < monitor.bounds.bottom &&
                pt.x >= monitor.bounds.left && pt.x < monitor.bounds.right) {
                LONG restricted_left = monitor.bounds.left + static_cast<LONG>(std::lround(static_cast<double>(monitor_width) * 0.25));
                restricted_left = std::clamp(restricted_left, monitor.bounds.left, monitor.bounds.right - 1);
                RECT zone{};
                zone.left = restricted_left;
                zone.top = strip_top;
                zone.right = monitor.bounds.right;
                zone.bottom = monitor.bounds.bottom;
                if (zone.right > zone.left && zone.bottom > zone.top) {
                    POINT zone_top_left{ zone.left, zone.top };
                    POINT zone_bottom_right{ zone.right - 1, zone.bottom - 1 };
                    auto top_left = ScreenToSource(zone_top_left);
                    auto bottom_right = ScreenToSource(zone_bottom_right);
                    if (top_left && bottom_right) {
                        messenger_zone_source_ = { top_left->x, top_left->y, bottom_right->x, bottom_right->y };
                        messenger_zone_active_ = true;
                        messenger_anchor_ = pt;
                    }
                }
            }
        }
    }
}

bool App::WindowMatchesPatterns(HWND hwnd, const std::initializer_list<const wchar_t*>& patterns) const {
    if (!hwnd || patterns.size() == 0) {
        return false;
    }

    auto normalize = [](std::wstring& text) {
        std::transform(text.begin(), text.end(), text.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    };

    std::vector<std::wstring> lowered_patterns;
    lowered_patterns.reserve(patterns.size());
    for (const wchar_t* pattern : patterns) {
        if (!pattern) {
            continue;
        }
        std::wstring lowered(pattern);
        normalize(lowered);
        if (!lowered.empty()) {
            lowered_patterns.push_back(std::move(lowered));
        }
    }

    if (lowered_patterns.empty()) {
        return false;
    }

    auto contains_pattern = [&](const std::wstring& text) {
        if (text.empty()) {
            return false;
        }
        std::wstring lowered = text;
        normalize(lowered);
        for (const auto& pattern : lowered_patterns) {
            if (lowered.find(pattern) != std::wstring::npos) {
                return true;
            }
        }
        return false;
    };

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) {
            process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        }
        if (process) {
            wchar_t buffer[MAX_PATH * 4]{};
            DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
            if (QueryFullProcessImageNameW(process, 0, buffer, &size) != 0 && size > 0) {
                std::wstring identifier(buffer, size);
                if (contains_pattern(identifier)) {
                    CloseHandle(process);
                    return true;
                }
            }
            CloseHandle(process);
        }
    }

    wchar_t title[256]{};
    int len = GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
    if (len > 0) {
        std::wstring window_title(title, title + len);
        if (contains_pattern(window_title)) {
            return true;
        }
    }

    wchar_t class_name[256]{};
    int class_len = GetClassNameW(hwnd, class_name, static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
    if (class_len > 0) {
        std::wstring window_class(class_name, class_name + class_len);
        if (contains_pattern(window_class)) {
            return true;
        }
    }

    return false;
}

std::optional<RECT> App::GetForegroundWindowRectIfMatches(const std::initializer_list<const wchar_t*>& patterns) const {
    HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return std::nullopt;
    }
    if (!WindowMatchesPatterns(foreground, patterns)) {
        return std::nullopt;
    }
    RECT rect{};
    if (!GetWindowRect(foreground, &rect)) {
        return std::nullopt;
    }
    return rect;
}

bool App::IsPuttyProcess() const {
    HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }
    return WindowMatchesPatterns(foreground, { L"putty" });
}

bool App::IsMessengerProcess() const {
    HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }
    return WindowMatchesPatterns(foreground, { L"whatsapp", L"telegram" });
}

void App::ApplyClickMovementLimit(float& x, float& y, ULONGLONG now) {
    if (!click_lock_active_ || !has_last_click_) {
        return;
    }

    if (!last_click_source_) {
        auto converted = ScreenToSource(last_click_position_);
        if (converted) {
            last_click_source_ = converted;
        } else {
            click_lock_active_ = false;
            return;
        }
    }

    float elapsed_ms = static_cast<float>(now - last_click_tick_);
    if (elapsed_ms < 0.0f) {
        elapsed_ms = 0.0f;
    }

    float limit = kClickLimitPixelsPerSecond * (elapsed_ms / 1000.0f);
    if (limit <= 0.0f) {
        x = last_click_source_->x;
        y = last_click_source_->y;
        return;
    }

    float min_x = last_click_source_->x - limit;
    float max_x = last_click_source_->x + limit;
    float min_y = last_click_source_->y - limit;
    float max_y = last_click_source_->y + limit;

    x = std::clamp(x, min_x, max_x);
    y = std::clamp(y, min_y, max_y);
}

void App::SnapCenterTo(float x, float y, ULONGLONG now, bool ignore_click_limit) {
    if (!ignore_click_limit) {
        ApplyClickMovementLimit(x, y, now);
    }
    if (!has_center_) {
        current_center_x_ = x;
        current_center_y_ = y;
        has_center_ = true;
        return;
    }

    float dx = x - current_center_x_;
    float dy = y - current_center_y_;
    float distance = std::sqrt(dx * dx + dy * dy);
    if (distance >= kPreviousCenterRecordThreshold) {
        if (!has_previous_center_ || (now - previous_center_saved_tick_) >= kPreviousCenterRecordCooldownMs) {
            previous_center_x_ = current_center_x_;
            previous_center_y_ = current_center_y_;
            has_previous_center_ = true;
            previous_center_saved_tick_ = now;
        }
    }

    current_center_x_ = x;
    current_center_y_ = y;
    has_center_ = true;
}

void App::CenterOnCaretNow() {
    if (!magnifier_active_ || tracking_mode_ == TrackingMode::Manual || source_index_ < 0) {
        return;
    }

    auto desc = capture_->FrameDesc();
    if (desc.Width == 0 || desc.Height == 0) {
        return;
    }

    const auto& source_monitor = SourceMonitor();
    if (!PointInRect(source_monitor.bounds, caret_position_)) {
        return;
    }

    FloatPoint caret_source{};
    caret_source.x = static_cast<float>((caret_position_.x - source_monitor.bounds.left) * source_monitor.scale);
    caret_source.y = static_cast<float>((caret_position_.y - source_monitor.bounds.top) * source_monitor.scale);
    caret_source.x += 4.0f;

    float frame_width = static_cast<float>(desc.Width);
    float frame_height = static_cast<float>(desc.Height);
    float view_width = std::min(frame_width / zoom_, frame_width);
    float view_height = std::min(frame_height / zoom_, frame_height);
    float half_w = view_width / 2.0f;
    float half_h = view_height / 2.0f;

    auto now = GetTickCount64();
    SnapCenterTo(caret_source.x, caret_source.y, now, true);
    last_caret_target_tick_ = now;

    current_center_x_ = std::clamp(current_center_x_, half_w, frame_width - half_w);
    current_center_y_ = std::clamp(current_center_y_, half_h, frame_height - half_h);
}

void App::RestorePreviousCenter() {
    if (!has_previous_center_) {
        return;
    }

    if (!has_center_) {
        current_center_x_ = previous_center_x_;
        current_center_y_ = previous_center_y_;
        has_center_ = true;
    } else {
        std::swap(current_center_x_, previous_center_x_);
        std::swap(current_center_y_, previous_center_y_);
    }

    previous_center_saved_tick_ = GetTickCount64();
}

void App::ClearCenterHistory() {
    has_center_ = false;
    has_previous_center_ = false;
    previous_center_saved_tick_ = 0;
    has_last_click_ = false;
    click_lock_active_ = false;
    last_click_tick_ = 0;
    last_click_source_.reset();
    messenger_zone_active_ = false;
}

void App::UpdateStatusOverlay() {
    if (!magnifier_) {
        return;
    }

    std::wstring next_text = status_overlay_text_;
    ULONGLONG duration = 0;
    bool has_new_text = false;

    if (pending_status_message_) {
        next_text = *pending_status_message_;
        duration = pending_status_duration_;
        pending_status_message_.reset();
        pending_status_duration_ = 0;
        has_new_text = true;
    } else if (!magnifier_active_) {
        next_text.clear();
        has_new_text = (status_overlay_text_ != next_text);
    }

    if (!has_new_text) {
        if (!status_overlay_dirty_) {
            return;
        }
        if (next_text == status_overlay_text_) {
            status_overlay_dirty_ = false;
            return;
        }
    }

    status_overlay_text_ = next_text;
    status_overlay_dirty_ = false;

    ULONGLONG effective_duration = 0;
    if (!status_overlay_text_.empty()) {
        if (has_new_text) {
            effective_duration = duration != 0 ? duration : kStatusBadgeDurationMs;
        }
    }

    magnifier_->SetStatusBadge(status_overlay_text_, effective_duration);
    if (effective_duration > 0) {
        status_overlay_end_tick_ = GetTickCount64() + effective_duration;
    } else if (status_overlay_text_.empty()) {
        status_overlay_end_tick_ = 0;
    }
}

const MonitorInfo& App::SourceMonitor() const {
    return monitors_->Monitors().at(static_cast<size_t>(source_index_));
}

const MonitorInfo& App::MagnifierMonitor() const {
    return monitors_->Monitors().at(static_cast<size_t>(magnifier_index_));
}

void App::HandleDisplayConfigurationChange(const wchar_t* reason, bool force_restart) {
    if (!ready_) {
        return;
    }

    std::wstring message = L"Display configuration change detected";
    if (reason && *reason) {
        message.append(L" (");
        message.append(reason);
        message.push_back(L')');
    }
    Logger::Info(message);

    bool restart = force_restart || magnifier_active_;
    if (magnifier_active_) {
        StopMagnifier();
    }

    if (!SelectMonitors()) {
        Logger::Error(L"Unable to refresh monitor selection after configuration change");
        return;
    }

    bool configured = false;
    if (restart) {
        configured = StartMagnifier();
    } else {
        configured = ConfigureForCurrentMonitors();
        if (configured) {
            UpdateTray();
        }
    }

    if (!configured) {
        Logger::Error(L"Failed to reconfigure magnifier after configuration change");
    }
}

void App::OnSystemSuspend() {
    if (!ready_) {
        return;
    }
    resume_should_start_magnifier_ = magnifier_active_;
    if (magnifier_active_) {
        StopMagnifier();
    }
}

void App::OnSystemResume() {
    if (!ready_) {
        return;
    }
    bool restart = resume_should_start_magnifier_;
    resume_should_start_magnifier_ = false;
    HandleDisplayConfigurationChange(L"Resume from sleep", restart);
}

