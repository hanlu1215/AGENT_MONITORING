// 主窗口：两个 agent 的状态卡片 + 监控开关 + 推送通道开关 + 隐私屏幕 + 按钮。
#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "appctx.h"

namespace cn {

class MainWindow {
public:
    static void RegisterWindowClass();

    // 返回进程退出码
    int Run(HINSTANCE instance, AppContext* app, const std::string& config_path);

private:
    struct AgentCard {
        HWND state = nullptr;
        HWND detail = nullptr;
    };

    void Build();
    void Refresh();
    void Say();
    void OnStart();
    void OnStop();
    void OnTest();
    void OnForget();
    void OnQuit();
    void OnToggleAgents();
    void OnToggleChannels();
    void OnConfigureFeishu();
    void OnConfigureDevice(size_t visible_index);
    void OnTogglePrivacy();
    void OnPickWallpaper();
    bool OnSetPassword();
    void Persist();

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    AppContext* app_ = nullptr;
    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT font_ = nullptr;
    HFONT font_bold_ = nullptr;
    HFONT font_title_ = nullptr;

    AgentCard cards_[2];
    HWND check_codex_ = nullptr;
    HWND check_claude_ = nullptr;
    HWND check_feishu_ = nullptr;
    std::vector<HWND> check_devices_;
    std::vector<int> device_config_indices_;  // -1 表示旧版 device，其它值是 devices[] 下标
    HWND check_tone_ = nullptr;
    HWND check_speech_ = nullptr;
    HWND check_privacy_ = nullptr;
    HWND label_privacy_ = nullptr;
    HWND button_start_ = nullptr;
    HWND button_stop_ = nullptr;
    HWND status_ = nullptr;

    std::string last_detail_[2];
    std::string pushed_detail_;
    bool pushed_ok_ = false;
    bool ports_ok_ = false;
};

}  // namespace cn
