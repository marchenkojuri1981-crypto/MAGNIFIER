#include "hotkey_manager.h"

HotkeyManager::HotkeyManager() = default;
HotkeyManager::~HotkeyManager() = default;

bool HotkeyManager::RegisterDefaults(HWND target) {
    UnregisterAll(target);

    UINT modifiers = MOD_CONTROL | MOD_ALT;
    RegisterCombo(target, modifiers, 'M', HotkeyAction::ToggleMagnifier);

    RegisterCombo(target, modifiers, VK_OEM_PLUS, HotkeyAction::ZoomIn);
    RegisterCombo(target, modifiers | MOD_SHIFT, VK_OEM_PLUS, HotkeyAction::ZoomIn);
    RegisterCombo(target, modifiers, VK_ADD, HotkeyAction::ZoomIn);

    RegisterCombo(target, modifiers, VK_OEM_MINUS, HotkeyAction::ZoomOut);
    RegisterCombo(target, modifiers | MOD_SHIFT, VK_OEM_MINUS, HotkeyAction::ZoomOut);
    RegisterCombo(target, modifiers, VK_SUBTRACT, HotkeyAction::ZoomOut);

    RegisterCombo(target, modifiers, 'T', HotkeyAction::SwitchMode);
    RegisterCombo(target, modifiers, 'I', HotkeyAction::ToggleInvert);
    RegisterCombo(target, modifiers, 'X', HotkeyAction::ShowCurrentTime);
    RegisterCombo(target, modifiers, 'C', HotkeyAction::ShowCurrentTime);
    RegisterCombo(target, modifiers, 'P', HotkeyAction::ToggleMousePassThrough);
    RegisterCombo(target, modifiers, 'O', HotkeyAction::OpenSettings);
    RegisterCombo(target, modifiers, 'R', HotkeyAction::ForceRestart);
    RegisterCombo(target, modifiers, 'Z', HotkeyAction::Quit);

    return true;
}

void HotkeyManager::UnregisterAll(HWND target) {
    for (const auto& [id, _] : hotkeys_) {
        UnregisterHotKey(target, static_cast<int>(id));
    }
    hotkeys_.clear();
    next_id_ = 1;
}

void HotkeyManager::HandleHotkey(WPARAM id) {
    auto it = hotkeys_.find(static_cast<UINT>(id));
    if (it != hotkeys_.end() && handler_) {
        handler_(it->second);
    }
}

void HotkeyManager::RegisterCombo(HWND target, UINT modifiers, UINT key, HotkeyAction action) {
    UINT id = next_id_++;
    if (RegisterHotKey(target, static_cast<int>(id), modifiers, key)) {
        hotkeys_[id] = action;
    }
}
