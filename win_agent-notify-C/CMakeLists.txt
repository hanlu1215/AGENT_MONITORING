cmake_minimum_required(VERSION 3.20)
project(win_agent_notify_c CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# exe 直接落在工程根目录，和 config.json / state.json 同级
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

set(CN_SOURCES
    src/json.cpp
    src/util/str.cpp
    src/util/pinyin.cpp
    src/util/pinyin_data.cpp
    src/util/paths.cpp
    src/util/times.cpp
    src/util/log.cpp
    src/util/crypto.cpp
    src/config.cpp
    src/store.cpp
    src/events.cpp
    src/queue.cpp
    src/http.cpp
    src/feishu.cpp
    src/device.cpp
    src/push.cpp
    src/monitor.cpp
    src/claude_monitor.cpp
    src/localsound.cpp
    src/api.cpp
    src/privacy.cpp
    src/password.cpp
    src/gui.cpp
    src/appctx.cpp
)

add_library(core STATIC ${CN_SOURCES})
target_include_directories(core PUBLIC src)

target_compile_definitions(core PUBLIC
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

target_compile_options(core PUBLIC
    -finput-charset=UTF-8 -fexec-charset=UTF-8
    -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(core PUBLIC -O2 -DNDEBUG)
endif()

# GDI+ / COM / WinHTTP / CNG / Winsock / 多媒体
target_link_libraries(core PUBLIC
    winhttp bcrypt ws2_32 winmm gdiplus gdi32
    user32 comdlg32 shell32 comctl32 ole32 oleaut32 uuid
)
# 单文件 exe：不依赖 libstdc++-6.dll / libgcc_s_seh-1.dll
target_link_options(core INTERFACE -static -static-libgcc -static-libstdc++)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only.")
endif()

# 图形界面版（无控制台）
add_executable(win_agent-notify-C WIN32 src/main_gui.cpp res/app.rc)
target_link_libraries(win_agent-notify-C PRIVATE core)
target_link_options(win_agent-notify-C PRIVATE -municode)
set_target_properties(win_agent-notify-C PROPERTIES OUTPUT_NAME "win_agent-notify-C")
