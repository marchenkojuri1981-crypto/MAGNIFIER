cmake_minimum_required(VERSION 3.20)

project(ElectronicMagnifier LANGUAGES CXX)

if(NOT WIN32)
    message(WARNING "ElectronicMagnifier is intended to be built on Windows with MSVC. Generation will continue, but the resulting project can only be built on Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX)

add_executable(ElectronicMagnifier
    src/main.cpp
    src/app.cpp
    src/monitor_manager.cpp
    src/capture_engine.cpp
    src/magnifier_window.cpp
    src/tracking_manager.cpp
    src/input_manager.cpp
    src/hotkey_manager.cpp
    src/tray_icon.cpp
    src/config.cpp
    src/settings_dialog.cpp
    src/logger.cpp
    resources/app_icon.rc
)

target_include_directories(ElectronicMagnifier PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if (MSVC)
    target_compile_options(ElectronicMagnifier PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_link_options(ElectronicMagnifier PRIVATE /SUBSYSTEM:WINDOWS)
endif()

target_link_libraries(ElectronicMagnifier PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler
    windowscodecs
    shcore
    user32
    gdi32
    shell32
    ole32
    oleaut32
    oleacc
    comctl32
    dwmapi
    uiautomationcore
)

add_executable(StopMagnifier
    src/stop_magnifier.cpp
)

target_link_libraries(StopMagnifier PRIVATE
    user32
)
