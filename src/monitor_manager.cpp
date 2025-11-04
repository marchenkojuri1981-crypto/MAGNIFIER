#include "monitor_manager.h"

#include <shellscalingapi.h>
#include <string>

MonitorManager::MonitorManager() {
    Refresh();
}

MonitorManager::~MonitorManager() = default;

void MonitorManager::Refresh() {
    monitors_.clear();
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hmon, HDC hdc, LPRECT rect, LPARAM user) -> BOOL {
        auto manager = reinterpret_cast<MonitorManager*>(user);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(hmon, &info)) {
            return TRUE;
        }

        MonitorInfo monitor{};
        monitor.handle = hmon;
        monitor.bounds = info.rcMonitor;
        monitor.work_area = info.rcWork;
        monitor.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        monitor.device_name = info.szDevice;

        DISPLAY_DEVICEW display_device{};
        display_device.cb = sizeof(display_device);
        if (EnumDisplayDevicesW(info.szDevice, 0, &display_device, 0)) {
            monitor.friendly_name = display_device.DeviceString;
        } else {
            monitor.friendly_name = monitor.device_name;
        }

        UINT dpiX = 96, dpiY = 96;
        if (GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY) == S_OK) {
            monitor.scale = static_cast<double>(dpiX) / 96.0;
        }

        manager->monitors_.push_back(monitor);
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
}

int MonitorManager::FindMonitorIndex(HMONITOR handle) const {
    for (size_t i = 0; i < monitors_.size(); ++i) {
        if (monitors_[i].handle == handle) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
const MonitorInfo* MonitorManager::FindByDeviceName(const std::wstring& device) const {
    for (const auto& monitor : monitors_) {
        if (monitor.device_name == device) {
            return &monitor;
        }
    }
    return nullptr;
}

