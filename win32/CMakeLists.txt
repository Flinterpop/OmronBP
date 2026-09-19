cmake_minimum_required(VERSION 3.25)
project(OmronBP VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: a single portable exe with no runtime dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_compile_options(/W4 /WX /permissive- /EHsc /utf-8 /Zc:__cplusplus)

# Core: no UI, no Bluetooth - what the tests exercise.
add_library(omron_core STATIC
    src/protocol.cpp
    src/models.cpp
    src/storage.cpp
    src/pdf.cpp
    src/report.cpp
)
target_include_directories(omron_core PUBLIC src)

add_executable(core_tests tests/core_tests.cpp src/workflow.cpp src/ble.cpp)
target_link_libraries(core_tests PRIVATE omron_core windowsapp)
enable_testing()
add_test(NAME core_tests COMMAND core_tests)

# The app (the manifest listed as a source is merged by CMake into the linker manifest).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
add_executable(OmronBP WIN32
    src/main.cpp
    src/ble.cpp
    src/workflow.cpp
    src/chart.cpp
    src/pair_dialog.cpp
    src/export_dialog.cpp
    src/app.rc
    src/app.ico
    src/app.manifest
)
target_link_libraries(OmronBP PRIVATE omron_core windowsapp comctl32 d2d1 dwrite shell32 shlwapi ole32 uxtheme)
target_compile_definitions(OmronBP PRIVATE APP_VERSION_STRING="${PROJECT_VERSION}")
endif()
