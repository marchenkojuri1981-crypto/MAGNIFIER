#pragma once

#include <windows.h>
#include <shellapi.h>
#include <string>

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    bool Create(HWND hwnd);
    void Destroy();
    void ShowNotification(const std::wstring& title, const std::wstring& message);
    void SetTooltip(const std::wstring& text);

private:
    NOTIFYICONDATAW nid_{};
    bool created_{false};
};
