#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int RunTaskkill(const std::string& arguments) {
    return std::system(arguments.c_str());
}
} // namespace

int main() {
    int result = RunTaskkill("taskkill /IM ElectronicMagnifier.exe /T /F >NUL 2>&1");
    if (result != 0) {
        std::cerr << "Failed to stop ElectronicMagnifier.exe (taskkill exit code " << result << ").\n";
        return 1;
    }

    return 0;
}
