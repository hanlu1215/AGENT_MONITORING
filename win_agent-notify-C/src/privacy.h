// 隐私屏幕：盖住屏幕内容，但不是真锁屏（agent 照常干活）。
// 每块显示器一个无边框置顶窗；主屏显示时钟/日期/状态字/解锁控件，副屏只铺壁纸。
#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "config.h"
#include "json.h"

namespace cn {

struct ScreenRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

std::vector<ScreenRect> ListMonitors();
int PrimaryMonitorIndex(const std::vector<ScreenRect>& rects);

// 壁纸列表（一张 / 多张 / 一个文件夹）
std::vector<std::string> WallpapersFrom(const Config& cfg);
double DimAlphaOf(const Config& cfg);

// 状态字/状态灯用的颜色：和 ESP32 板载灯同一套 RGB（见 ESP32S3_WebLED.ino 的 stateColor）
COLORREF StatusLedColor(const std::string& status);

// 把壁纸处理成"正好铺满这块屏"的缓存 PNG（cover 缩放 + 压暗烘焙）。失败返回空。
std::string PrepareWallpaper(const std::string& source, int width, int height, double dim_alpha);

// 大字/小字：等待审批 > 工作中 > 已完成。
// status_out 回填"这行大字是哪个状态"，屏上据此取色（没人在干活时是空的）
std::pair<std::string, std::string> AgentHeadline(const std::map<std::string, Json>& states,
                                                  const Config& cfg,
                                                  std::string* status_out = nullptr);

class PrivacyScreen {
public:
    void Init(const Config* cfg) { cfg_ = cfg; }
    void SetConfig(const Config* cfg) { cfg_ = cfg; }

    bool visible() const { return !windows_.empty(); }
    void Show();
    void Hide();

    void SetText(const std::string& headline, const std::string& detail);
    void SetText(const std::string& headline, const std::string& detail,
                 const std::string& status);
    void UpdateFromStates(const std::map<std::string, Json>& states);

    // 防烧屏位移（自测会直接调）
    void Drift(bool force = false);
    int DriftOffsetX() const { return drift_x_; }
    int DriftOffsetY() const { return drift_y_; }

    // 解锁时回调（GUI 用来把勾选框复位）
    std::function<void()> on_unlock;

    // 主屏上「时钟 / 日期 / 状态 / 解锁控件」的盒子。
    // 画、算输入光标、判断点没点中，全都用这一份 —— 改排版只改 ComputeStack。
    struct StackRects {
        RECT clock{};
        RECT date{};
        RECT head{};
        RECT detail{};   // 状态下面那行小字（"Codex 等待审批 · Claude 工作中"）
        RECT ask{};      // 「解锁屏幕」
        RECT entry{};    // 密码框
        RECT ok{};
        RECT reveal{};
        int clock_px = 0;
        int date_px = 0;
        int head_px = 0;
        int detail_px = 0;
        int button_font_px = 15;
        int small_font_px = 12;
    };

    // 自测用：只开一块指定范围的屏。visible=false 时完全不显示、不抢焦点
    void ShowForTest(const ScreenRect& rect, bool visible = false);
    void HideForTest() { testing_ = false; Hide(); }
    HWND PrimaryWindow() const;
    const std::vector<ScreenRect>& Monitors() const { return monitor_rects_; }
    // 自测用：当前主屏尺寸下算出来的排版（用来断言间距均匀、按钮点得中）
    StackRects LayoutForTest() const;
    // 自测用：把主屏画到给定的 DC 上（抓像素验证状态字颜色）
    void PaintForTest(HDC dc);

    // 窗口消息（窗口过程用）
    LRESULT OnMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    static void RegisterWindowClass();

private:
    struct Screen {
        HWND hwnd = nullptr;
        ScreenRect rect;
        bool primary = false;
        HBITMAP bitmap = nullptr;       // 当前贴的壁纸（已按本屏分辨率烘焙）
        std::string bitmap_source;
        int drift_x = 0;
        int drift_y = 0;
    };

    void BuildWindows();
    void DestroyWindows();
    void PaintAll();
    void PaintScreen(const Screen& screen, HDC hdc);
    void SetWallpaperIndex(int index);
    void RequestUnlock();
    void TryUnlock();
    void HandleKey(UINT message, WPARAM wparam);
    StackRects ComputeStack(int width, int height) const;
    // 密码框里的输入光标（自己画的框没有 EDIT 控件，光标要手动管）
    void RefreshCaret();
    void DropCaret();
    const Screen* FindScreen(HWND hwnd) const;

    const Config* cfg_ = nullptr;
    std::vector<Screen> windows_;
    std::vector<ScreenRect> monitor_rects_;
    std::string headline_;
    std::string detail_;
    std::string headline_status_;   // 大字对应的状态，决定它的颜色
    std::vector<std::string> wallpapers_;
    int wall_index_ = 0;
    double last_drift_ = 0.0;
    int drift_x_ = 0;
    int drift_y_ = 0;
    bool asking_ = false;
    bool reveal_ = false;
    bool testing_ = false;
    bool test_visible_ = false;
    std::wstring password_;   // 直接存 UTF-16，输入法/emoji 都不会被截坏
    std::string hint_;
    int caret_x_ = 0;
    int caret_y_ = 0;
    int caret_height_ = 0;
};

}  // namespace cn
