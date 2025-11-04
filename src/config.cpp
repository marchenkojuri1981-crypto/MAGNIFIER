#include "config.h"

#include "tracking_manager.h"

#include <fstream>
#include <ShlObj.h>

namespace fs = std::filesystem;

Config::Config() {
    data_.mode = TrackingMode::Auto;
    Load();
}

Config::~Config() = default;

bool Config::Load() {
    auto path = GetConfigPath();
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    auto read_string = [&](const std::string& key) -> std::wstring {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) {
            return L"";
        }
        pos = content.find('"', content.find(':', pos) + 1);
        if (pos == std::string::npos) {
            return L"";
        }
        auto end = content.find('"', pos + 1);
        if (end == std::string::npos) {
            return L"";
        }
        std::string value = content.substr(pos + 1, end - pos - 1);
        return std::wstring(value.begin(), value.end());
    };

    auto read_float = [&](const std::string& key, float fallback) -> float {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) {
            return fallback;
        }
        pos = content.find(':', pos);
        if (pos == std::string::npos) {
            return fallback;
        }
        auto end = content.find_first_of(",}\n", pos + 1);
        std::string number = content.substr(pos + 1, end - pos - 1);
        try {
            return std::stof(number);
        } catch (...) {
            return fallback;
        }
    };

    auto read_bool = [&](const std::string& key, bool fallback) -> bool {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) {
            return fallback;
        }
        pos = content.find(':', pos);
        if (pos == std::string::npos) {
            return fallback;
        }
        auto end = content.find_first_of(",}\n", pos + 1);
        std::string token = content.substr(pos + 1, end - pos - 1);
        if (token.find("true") != std::string::npos) {
            return true;
        }
        if (token.find("false") != std::string::npos) {
            return false;
        }
        return fallback;
    };

    data_.source_monitor = read_string("sourceMonitor");
    data_.magnifier_monitor = read_string("magnifierMonitor");
    data_.zoom = read_float("zoom", data_.zoom);
    data_.block_cursor = read_bool("blockCursor", data_.block_cursor);
    data_.auto_launch = read_bool("autoLaunch", data_.auto_launch);
    data_.invert_colors = read_bool("invertColors", data_.invert_colors);

    std::wstring mode = read_string("trackingMode");
    if (mode == L"Caret") {
        data_.mode = TrackingMode::Caret;
    } else if (mode == L"Mouse") {
        data_.mode = TrackingMode::Mouse;
    } else if (mode == L"Focus") {
        data_.mode = TrackingMode::Focus;
    } else if (mode == L"Manual") {
        data_.mode = TrackingMode::Manual;
    } else {
        data_.mode = TrackingMode::Auto;
    }

    return true;
}

bool Config::Save() {
    auto path = GetConfigPath();
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    auto to_utf8 = [](const std::wstring& value) {
        return std::string(value.begin(), value.end());
    };

    std::string mode;
    switch (data_.mode) {
    case TrackingMode::Caret: mode = "Caret"; break;
    case TrackingMode::Mouse: mode = "Mouse"; break;
    case TrackingMode::Focus: mode = "Focus"; break;
    case TrackingMode::Manual: mode = "Manual"; break;
    default: mode = "Auto"; break;
    }

    out << "{\n";
    out << "  \"sourceMonitor\": \"" << to_utf8(data_.source_monitor) << "\",\n";
    out << "  \"magnifierMonitor\": \"" << to_utf8(data_.magnifier_monitor) << "\",\n";
    out << "  \"zoom\": " << data_.zoom << ",\n";
    out << "  \"trackingMode\": \"" << mode << "\",\n";
    out << "  \"blockCursor\": " << (data_.block_cursor ? "true" : "false") << ",\n";
    out << "  \"autoLaunch\": " << (data_.auto_launch ? "true" : "false") << ",\n";
    out << "  \"invertColors\": " << (data_.invert_colors ? "true" : "false") << "\n";
    out << "}\n";
    return true;
}

std::filesystem::path Config::GetConfigPath() const {
    wchar_t appdata[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    fs::path path = fs::path(appdata) / L"ElectronicMagnifier" / L"config.json";
    return path;
}
