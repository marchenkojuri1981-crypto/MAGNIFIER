#include "settings_dialog.h"

#include <string>

SettingsDialog::SettingsDialog() = default;
SettingsDialog::~SettingsDialog() = default;

bool SettingsDialog::Show(HINSTANCE, HWND parent) {
    std::wstring message =
        L"Settings UI is not yet implemented.\n"
        L"Edit %APPDATA%/ElectronicMagnifier/config.json to adjust behaviour.";
    MessageBoxW(parent, message.c_str(), L"Electronic Magnifier", MB_OK | MB_ICONINFORMATION);
    return true;
}

INT_PTR CALLBACK SettingsDialog::DlgProc(HWND, UINT, WPARAM, LPARAM) {
    return FALSE;
}
