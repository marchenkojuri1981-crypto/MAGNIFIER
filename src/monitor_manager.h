#pragma once

#include <vector>
#include <string>
#include <windows.h>

struct MonitorInfo {
    HMONITOR handle{};
    RECT bounds{};
    RECT work_area{};
    double scale{1.0};
    bool primary{false};
    std::wstring device_name;
    std::wstring friendly_name;
};

class MonitorManager {
public:
    MonitorManager();
    ~MonitorManager();

    void Refresh();

    const std::vector<MonitorInfo>& Monitors() const { return monitors_; }
    std::vector<MonitorInfo>& Monitors() { return monitors_; }
    int FindMonitorIndex(HMONITOR handle) const;
    const MonitorInfo* FindByDeviceName(const std::wstring& device) const;

private:
    std::vector<MonitorInfo> monitors_;
};
