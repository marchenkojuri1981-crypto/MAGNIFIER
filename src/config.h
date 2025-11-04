#pragma once

#include <filesystem>
#include <string>

#include "tracking_manager.h"

enum class TrackingMode;

struct AppConfig {
    std::wstring source_monitor;
    std::wstring magnifier_monitor;
    float zoom{2.0f};
    TrackingMode mode;
    bool block_cursor{true};
    bool auto_launch{false};
    bool invert_colors{false};
};

class Config {
public:
    Config();
    ~Config();

    bool Load();
    bool Save();

    AppConfig& Data() { return data_; }
    const AppConfig& Data() const { return data_; }

private:
    std::filesystem::path GetConfigPath() const;

    AppConfig data_;
};
