#include "tray_icon.h"

#include "resource.h"

TrayIcon::TrayIcon() = default;
TrayIcon::~TrayIcon() {
    Destroy();
}

bool TrayIcon::Create(HWND hwnd) {
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = WM_APP + 1;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!icon) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    nid_.hIcon = icon;
    lstrcpyW(nid_.szTip, L"Electronic Magnifier");
    nid_.uVersion = NOTIFYICON_VERSION_4;
    created_ = Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE;
    if (created_) {
        Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    }
    return created_;
}

void TrayIcon::Destroy() {
    if (created_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        created_ = false;
    }
}

void TrayIcon::ShowNotification(const std::wstring& title, const std::wstring& message) {
    if (!created_) {
        return;
    }
    nid_.uFlags = NIF_INFO;
    lstrcpynW(nid_.szInfoTitle, title.c_str(), ARRAYSIZE(nid_.szInfoTitle));
    lstrcpynW(nid_.szInfo, message.c_str(), ARRAYSIZE(nid_.szInfo));
    nid_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::SetTooltip(const std::wstring& text) {
    if (!created_) {
        return;
    }
    nid_.uFlags = NIF_TIP;
    lstrcpynW(nid_.szTip, text.c_str(), ARRAYSIZE(nid_.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}
