#pragma once

#include <functional>
#include <windows.h>

using KeyCallback = std::function<bool(WPARAM, LPARAM)>;

class InputManager {
public:
    InputManager();
    ~InputManager();

    void Start();
    void Stop();

    void SetKeyCallback(KeyCallback cb) { key_callback_ = std::move(cb); }

private:
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam);

    static InputManager* instance_;
    HHOOK keyboard_hook_{};
    KeyCallback key_callback_;
};
