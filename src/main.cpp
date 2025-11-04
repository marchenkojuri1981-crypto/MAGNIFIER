#include "app.h"

#include <windows.h>
#include <shellscalingapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    App app;
    int result = app.Run();

    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
    return result;
}
