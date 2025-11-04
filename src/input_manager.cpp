#include "input_manager.h"

InputManager* InputManager::instance_ = nullptr;

InputManager::InputManager() = default;
InputManager::~InputManager() {
    Stop();
}

void InputManager::Start() {
    instance_ = this;
    keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &InputManager::KeyboardProc, nullptr, 0);
}

void InputManager::Stop() {
    if (keyboard_hook_) {
        UnhookWindowsHookEx(keyboard_hook_);
        keyboard_hook_ = nullptr;
    }
    instance_ = nullptr;
}

LRESULT CALLBACK InputManager::KeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && instance_ && instance_->key_callback_) {
        if (instance_->key_callback_(wparam, lparam)) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
