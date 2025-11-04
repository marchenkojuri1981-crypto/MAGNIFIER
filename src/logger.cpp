#include "logger.h"

#include <windows.h>

namespace {
void Output(const std::wstring& prefix, const std::wstring& message) {
    std::wstring line = prefix + L": " + message + L"\n";
    OutputDebugStringW(line.c_str());
}
}

void Logger::Info(const std::wstring& message) {
    Output(L"[INFO]", message);
}

void Logger::Error(const std::wstring& message) {
    Output(L"[ERROR]", message);
}

