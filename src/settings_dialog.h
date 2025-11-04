#pragma once

#include <windows.h>

class SettingsDialog {
public:
    SettingsDialog();
    ~SettingsDialog();

    bool Show(HINSTANCE instance, HWND parent);

private:
    static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

