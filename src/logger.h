#pragma once

#include <string>

class Logger {
public:
    static void Info(const std::wstring& message);
    static void Error(const std::wstring& message);
};

