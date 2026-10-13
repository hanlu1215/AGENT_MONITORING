#include "privacy.h"

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <objidl.h>     // GDI+ 需要 PROPID / IStream 这些声明
#include <gdiplus.h>

#include <algorithm>
#include <mutex>

#include "password.h"
#include "util/crypto.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

const wchar_t* kWindowClass = L"CodexNotifyPrivacyScreen";
constexpr int kTickMs = 500;
constexpr int kTickTimer = 1;

const std::map<std::string, std::string>& AgentLabels() {
    static const std::map<std::string, std::string> kLabels = {{"codex", "Codex"},
                                                               {"claude", "Claude Code"}};
    return kLabels;
}

// ---- GDI+ ----

std::once_flag g_gdiplus_once;
ULONG_PTR g_gdiplus_token = 0;

void EnsureGdiplus() {
    std::call_once(g_gdiplus_once, [] {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr);
    });
}

bool PngEncoderClsid(CLSID* out) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;
    std::vector<BYTE> buffer(bytes);
    Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)buffer.data();
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *out = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

// ---- 绘制 ----

// 白字背后八个方向各描一层黑边（参考那份 GTK 时钟壁纸的做法），叠在照片上也读得清
void DrawOutlined(HDC hdc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
                  UINT flags) {
    if (text.empty()) return;
    HGDIOBJ previous = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    const int offsets[8][2] = {{-2, -2}, {0, -2}, {2, -2}, {-2, 0},
                               {2, 0},   {-2, 2}, {0, 2},  {2, 2}};
    SetTextColor(hdc, RGB(0, 0, 0));
    for (const auto& offset : offsets) {
        RECT shifted = rect;
        shifted.left += offset[0];
        shifted.right += offset[0];
        shifted.top += offset[1];
        shifted.bottom += offset[1];
        DrawTextW(hdc, text.c_str(), -1, &shifted, flags);
    }
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), -1, &rect, flags);
    SelectObject(hdc, previous);
}

HFONT MakeFont(int pixel_height, const wchar_t* face, bool bold) {
    return CreateFontW(-(pixel_height > 0 ? pixel_height : 1), 0, 0, 0,
                       bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       face);
}

RECT CenterRect(int center_x, int top, int width, int height) {
    RECT rect;
    rect.left = center_x - width / 2;
    rect.right = rect.left + width;
    rect.top = top;
    rect.bottom = top + height;
    return rect;
}

bool PointIn(const RECT& rect, int x, int y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

}  // namespace

// 状态字用什么颜色：跟 ESP32 板载灯一个色（原值见 ESP32S3_WebLED/ESP32S3_WebLED.ino
// 的 stateColor()）。这里直接取灯的原始 RGB —— 屏幕上要的是"亮"，
// 不像 GUI 那边要考虑浅色背景把颜色压暗。认不出的状态就留白字。
COLORREF StatusLedColor(const std::string& status) {
    std::string key = ToLowerAscii(status);
    if (key == "working") return RGB(255, 150, 0);           // 黄
    if (key == "waiting_approval") return RGB(255, 0, 0);    // 红
    if (key == "completed") return RGB(0, 90, 255);          // 蓝
    if (key == "interrupted") return RGB(255, 0, 160);       // 品红
    if (key == "idle") return RGB(0, 200, 0);                // 绿
    if (key == "info") return RGB(0, 180, 255);              // 青
    return RGB(255, 255, 255);
}

std::vector<ScreenRect> ListMonitors() {
    std::vector<ScreenRect> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT rect, LPARAM data) -> BOOL {
            auto* list = reinterpret_cast<std::vector<ScreenRect>*>(data);
            if (rect && rect->right > rect->left && rect->bottom > rect->top) {
                list->push_back({rect->left, rect->top, rect->right - rect->left,
                                 rect->bottom - rect->top});
            }
            return TRUE;
        },
        (LPARAM)&out);
    return out;
}

int PrimaryMonitorIndex(const std::vector<ScreenRect>& rects) {
    for (size_t i = 0; i < rects.size(); ++i) {
        const ScreenRect& rect = rects[i];
        if (rect.x <= 0 && 0 < rect.x + rect.width && rect.y <= 0 && 0 < rect.y + rect.height) {
            return int(i);   // 含坐标原点那块就是主屏
        }
    }
    return 0;
}

double DimAlphaOf(const Config& cfg) {
    const Json& section = cfg.get("privacy");
    if (!section.is_object()) return 0.35;
    const Json& raw = section.has("dim_alpha") ? section.get("dim_alpha") : section.get("dim");
    if (raw.is_null() || raw.is_bool()) return 0.35;
    if (raw.is_num()) return (std::max)(0.0, (std::min)(0.9, raw.as_double()));
    std::string text = ToLowerAscii(Trim(raw.as_string()));
    if (text.empty() || text == "none" || text == "false" || text == "off") return 0.0;
    static const std::map<std::string, double> kTable = {
        {"gray12", 0.25}, {"gray25", 0.35}, {"gray50", 0.45}, {"gray75", 0.6}};
    auto found = kTable.find(text);
    return found == kTable.end() ? 0.35 : found->second;
}

std::vector<std::string> WallpapersFrom(const Config& cfg) {
    std::vector<std::string> found;
    const Json& section = cfg.get("privacy");
    const Json& raw = section.is_object() ? section.get("wallpaper") : Json();
    std::vector<std::string> items;
    if (raw.is_array()) {
        for (size_t i = 0; i < raw.size(); ++i) {
            std::string value = raw.at(i).as_string();
            if (!value.empty()) items.push_back(value);
        }
    } else if (raw.is_string() && !raw.as_string().empty()) {
        items.push_back(raw.as_string());
    }

    for (const std::string& item : items) {
        std::string path = ExpandPath(item);
        if (DirExists(path)) {
            for (std::string& child :
                 ListFilesSorted(path, {".png", ".gif", ".jpg", ".jpeg", ".bmp"})) {
                found.push_back(child);
            }
        } else if (FileExists(path)) {
            found.push_back(path);
        }
    }
    std::vector<std::string> unique;
    for (const std::string& path : found) {
        if (std::find(unique.begin(), unique.end(), path) == unique.end()) unique.push_back(path);
    }
    return unique;
}

std::string PrepareWallpaper(const std::string& source, int width, int height, double dim_alpha) {
    EnsureGdiplus();
    if (source.empty() || width <= 0 || height <= 0) return std::string();
    std::string src = ExpandPath(source);
    if (!FileExists(src)) return std::string();

    dim_alpha = (std::max)(0.0, (std::min)(0.9, dim_alpha));
    std::string stamp = PathFileName(src) + "-" + FormatDouble(FileMTime(src), 3) + "-" +
                        std::to_string(width) + "x" + std::to_string(height) + "-" +
                        FormatDouble(dim_alpha, 3);
    std::string cache_dir = PathJoin(TempDir(), "codex-notify-wallpaper");
    std::string cache = PathJoin(cache_dir, "wall-" + Sha1Hex(stamp).substr(0, 16) + ".png");
    if (FileExists(cache) && FileSize(cache) > 0) return cache;

    Gdiplus::Bitmap* image = Gdiplus::Bitmap::FromFile(Utf8ToWide(src).c_str());
    if (!image || image->GetLastStatus() != Gdiplus::Ok) {
        delete image;
        return std::string();
    }
    int src_w = int(image->GetWidth());
    int src_h = int(image->GetHeight());
    if (src_w <= 0 || src_h <= 0) {
        delete image;
        return std::string();
    }

    Gdiplus::Bitmap target(width, height, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&target);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.Clear(Gdiplus::Color(255, 0, 0, 0));

    // 等比放大到铺满（cover）再居中裁切：不管原图多大，出来一定刚好是这块屏的分辨率
    double scale = (std::max)(double(width) / src_w, double(height) / src_h);
    int dest_w = int(src_w * scale);
    int dest_h = int(src_h * scale);
    graphics.DrawImage(image, (width - dest_w) / 2, (height - dest_h) / 2, dest_w, dest_h);
    if (dim_alpha > 0.0) {
        // 压暗是**平滑烘焙进图片**的，不是盖网点 —— 照片上不会满屏噪点
        BYTE alpha = (BYTE)int(255 * dim_alpha + 0.5);
        Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, 0, 0, 0));
        graphics.FillRectangle(&brush, 0, 0, width, height);
    }
    graphics.Flush();
    delete image;

    EnsureDir(cache_dir);
    CLSID encoder;
    bool saved = false;
    if (PngEncoderClsid(&encoder)) {
        std::string part = cache + ".part";
        saved = target.Save(Utf8ToWide(part).c_str(), &encoder, nullptr) == Gdiplus::Ok;
        if (saved) {
            MoveFileExW(Utf8ToWide(part).c_str(), Utf8ToWide(cache).c_str(),
                        MOVEFILE_REPLACE_EXISTING);
        } else {
            RemoveFile(part);
        }
    }
    return saved ? cache : std::string();
}

std::pair<std::string, std::string> AgentHeadline(const std::map<std::string, Json>& states,
                                                  const Config& cfg,
                                                  std::string* status_out) {
    const Json& section = cfg.get("privacy");
    const char* order[] = {"waiting_approval", "working", "completed"};
    std::map<std::string, std::vector<std::string>> named;
    std::vector<std::string> idle;
    for (const char* key : {"codex", "claude"}) {
        auto label = AgentLabels().find(key);
        std::string name = label == AgentLabels().end() ? key : label->second;
        auto found = states.find(key);
        std::string status;
        if (found != states.end()) status = ToLowerAscii(found->second.get_str("status"));
        bool matched = false;
        for (const char* candidate : order) {
            if (status == candidate) {
                named[status].push_back(name);
                matched = true;
                break;
            }
        }
        if (!matched && found != states.end()) idle.push_back(name);
    }

    std::string headline;
    std::string headline_status;
    for (const char* status : order) {
        auto found = named.find(status);
        if (found == named.end() || found->second.empty()) continue;
        std::string short_name = Split(status, '_').front();
        std::string text;
        if (section.is_object()) {
            text = Trim(section.get_str(std::string("headline_") + status));
            if (text.empty()) text = Trim(section.get_str(std::string("headline_") + short_name));
        }
        if (text.empty()) continue;   // 留空 = 这一条不显示
        headline = text;
        headline_status = status;
        break;
    }
    if (status_out) *status_out = headline_status;

    auto short_label = [](const std::string& status) {
        if (status == "working") return std::string("工作中");
        if (status == "waiting_approval") return std::string("等待审批");
        if (status == "completed") return std::string("已完成");
        return status;
    };
    std::vector<std::string> parts;
    for (const char* status : order) {
        auto found = named.find(status);
        if (found == named.end()) continue;
        for (const std::string& name : found->second) {
            parts.push_back(name + " " + short_label(status));
        }
    }
    bool show_idle = section.is_object() && section.get_bool("show_idle_agents", false);
    if (show_idle) {
        for (const std::string& name : idle) parts.push_back(name + " 空闲");
    }
    if (headline.empty() && !show_idle) return {"", ""};
    return {headline, Join(parts, " · ")};
}

// ---------------------------------------------------------------------------
//  窗口
// ---------------------------------------------------------------------------

LRESULT CALLBACK PrivacyProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    PrivacyScreen* screen = (PrivacyScreen*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        CREATESTRUCTW* create = (CREATESTRUCTW*)lparam;
        screen = (PrivacyScreen*)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)screen);
    }
    if (screen) return screen->OnMessage(hwnd, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void PrivacyScreen::RegisterWindowClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = PrivacyProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = CreateSolidBrush(RGB(11, 11, 11));
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);
    registered = true;
}

HWND PrivacyScreen::PrimaryWindow() const {
    for (const Screen& screen : windows_) {
        if (screen.primary && screen.hwnd) return screen.hwnd;
    }
    return windows_.empty() ? nullptr : windows_.front().hwnd;
}

void PrivacyScreen::Show() { BuildWindows(); }

void PrivacyScreen::ShowForTest(const ScreenRect& rect, bool visible) {
    testing_ = true;
    test_visible_ = visible;
    monitor_rects_.clear();
    monitor_rects_.push_back(rect);
    BuildWindows();
}

// 主屏的竖排布局：时钟 → 日期 → 状态 → 解锁控件，四格从上到下排，
// 相邻两格之间留一样的空隙，整组再垂直居中 —— 任何分辨率下间距都是均匀的。
// 状态格和解锁格永远占位（哪怕这会儿没人干活），这样任务一起来一变，
// 别的元素不会跟着上下跳。
PrivacyScreen::StackRects PrivacyScreen::ComputeStack(int width, int height) const {
    StackRects out;
    static const Json kEmpty;
    const Json& section = cfg_ ? cfg_->get("privacy") : kEmpty;
    bool show_clock = !section.is_object() || section.get_bool("show_clock", true);
    bool show_date = !section.is_object() || section.get_bool("show_date", true);

    // 字号全按屏高算，换显示器 / 改分辨率自动适配
    out.clock_px = (std::max)(48, int(height * 0.16));
    out.date_px = (std::max)(14, int(height * 0.028));
    out.head_px = (std::max)(24, int(height * 0.045));
    out.detail_px = (std::max)(14, int(height * 0.022));
    out.button_font_px = (std::max)(13, int(height * 0.0185));
    out.small_font_px = (std::max)(11, int(height * 0.0155));

    int clock_h = int(out.clock_px * 1.20);
    int date_h = int(out.date_px * 1.70);
    int head_h = int(out.head_px * 1.70);
    // 状态下面那行小字的空地算在状态格里：按两行留，1 行时在格子里垂直居中，
    // 2 行也不会顶到下面的解锁按钮
    int detail_h = int(out.detail_px * 3.10);
    int button_h = (std::max)(30, int(height * 0.046));
    int gap = (std::max)(16, int(height * 0.042));

    const int box_h[4] = {clock_h, date_h, head_h + detail_h, button_h};
    const bool shown[4] = {show_clock, show_date, true, true};
    int sum = 0;
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        if (!shown[i]) continue;
        sum += box_h[i];
        ++count;
    }
    int top = (height - (sum + gap * (count > 0 ? count - 1 : 0))) / 2 + drift_y_;
    int center_x = width / 2 + drift_x_;
    int wrap = int(width * 0.8);
    int y = top;

    if (show_clock) {
        out.clock = CenterRect(center_x, y, out.clock_px * 5, clock_h);
        y += clock_h + gap;
    }
    if (show_date) {
        out.date = CenterRect(center_x, y, wrap, date_h);
        y += date_h + gap;
    }
    out.head = CenterRect(center_x, y, wrap, head_h);
    out.detail = CenterRect(center_x, y + head_h, wrap, detail_h);
    y += head_h + detail_h + gap;

    // 解锁控件那一行：框和按钮都随屏高缩放，不然 4K 上按钮小得点不着
    int ask_w = button_h * 4;
    int entry_w = button_h * 8;
    int ok_w = button_h * 5 / 2;
    int row_gap = (std::max)(8, button_h / 3);
    out.ask = CenterRect(center_x, y, ask_w, button_h);

    int row_w = entry_w + row_gap + ok_w + row_gap + ok_w;
    int left = center_x - row_w / 2;
    auto place = [&](int part_w) {
        RECT rect{};
        rect.left = left;
        rect.top = y;
        rect.right = left + part_w;
        rect.bottom = y + button_h;
        left += part_w + row_gap;
        return rect;
    };
    out.entry = place(entry_w);
    out.ok = place(ok_w);
    out.reveal = place(ok_w);
    return out;
}

PrivacyScreen::StackRects PrivacyScreen::LayoutForTest() const {
    HWND hwnd = PrimaryWindow();
    if (!hwnd) return StackRects{};
    RECT client{};
    GetClientRect(hwnd, &client);
    return ComputeStack(client.right, client.bottom);
}

void PrivacyScreen::PaintForTest(HDC dc) {
    for (const Screen& screen : windows_) {
        if (screen.primary) {
            PaintScreen(screen, dc);
            return;
        }
    }
}

void PrivacyScreen::BuildWindows() {
    if (!windows_.empty() || !cfg_) return;
    RegisterWindowClass();

    asking_ = false;
    reveal_ = false;
    password_.clear();
    hint_.clear();
    drift_x_ = drift_y_ = 0;
    last_drift_ = NowUnix();
    wallpapers_ = WallpapersFrom(*cfg_);
    wall_index_ = 0;

    std::vector<ScreenRect> rects = monitor_rects_;
    if (rects.empty()) {
        rects = ListMonitors();
        if (rects.empty()) {
            rects.push_back({0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)});
        }
    }
    int main_index = testing_ ? 0 : PrimaryMonitorIndex(rects);

    for (size_t i = 0; i < rects.size(); ++i) {
        Screen screen;
        screen.rect = rects[i];
        screen.primary = (int(i) == main_index);
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"",
                                    WS_POPUP, screen.rect.x, screen.rect.y, screen.rect.width,
                                    screen.rect.height, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), this);
        if (!hwnd) continue;
        screen.hwnd = hwnd;
        windows_.push_back(screen);
    }

    if (wallpapers_.size() > 1) {
        LogInfo("privacy", Cat("隐私屏幕：", (long long)wallpapers_.size(),
                               " 张壁纸，每次换位置轮换一张"));
    }
    SetWallpaperIndex(0);

    if (!testing_ || test_visible_) {
        for (Screen& screen : windows_) {
            ShowWindow(screen.hwnd, SW_SHOW);
            UpdateWindow(screen.hwnd);
            SetTimer(screen.hwnd, kTickTimer, kTickMs, nullptr);
        }
        // 只在打开时抢一次键盘焦点：敲的键全落在隐私屏上，不会漏到别的程序里
        HWND primary = PrimaryWindow();
        if (primary) {
            SetForegroundWindow(primary);
            SetFocus(primary);
        }
    }
    LogInfo("privacy", Cat("隐私屏幕：", (long long)windows_.size(), " 块屏幕"));
}

void PrivacyScreen::DestroyWindows() {
    for (Screen& screen : windows_) {
        if (screen.hwnd) {
            KillTimer(screen.hwnd, kTickTimer);
            DestroyWindow(screen.hwnd);
        }
        if (screen.bitmap) {
            DeleteObject(screen.bitmap);
            screen.bitmap = nullptr;
        }
    }
    DropCaret();
    caret_height_ = 0;
    windows_.clear();
    asking_ = false;
    password_.clear();
}

void PrivacyScreen::Hide() {
    DestroyWindows();
    testing_ = false;
    test_visible_ = false;
    monitor_rects_.clear();
}

void PrivacyScreen::SetWallpaperIndex(int index) {
    if (wallpapers_.empty()) return;
    int count = int(wallpapers_.size());
    wall_index_ = ((index % count) + count) % count;
    const std::string& source = wallpapers_[wall_index_];
    double alpha = DimAlphaOf(*cfg_);
    for (Screen& screen : windows_) {
        std::string prepared =
            PrepareWallpaper(source, screen.rect.width, screen.rect.height, alpha);
        if (prepared.empty()) continue;
        if (screen.bitmap) {
            DeleteObject(screen.bitmap);
            screen.bitmap = nullptr;
        }
        Gdiplus::Bitmap* image = Gdiplus::Bitmap::FromFile(Utf8ToWide(prepared).c_str());
        if (image && image->GetLastStatus() == Gdiplus::Ok) {
            HBITMAP handle = nullptr;
            if (image->GetHBITMAP(Gdiplus::Color(255, 11, 11, 11), &handle) == Gdiplus::Ok) {
                screen.bitmap = handle;
            }
        }
        delete image;
        screen.bitmap_source = prepared;
    }
}

void PrivacyScreen::PaintScreen(const Screen& screen, HDC hdc) {
    if (!cfg_) return;
    RECT client{};
    GetClientRect(screen.hwnd, &client);
    int width = client.right;
    int height = client.bottom;
    if (width <= 0 || height <= 0) return;

    HDC memory = CreateCompatibleDC(hdc);
    HBITMAP buffer = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ old_bitmap = SelectObject(memory, buffer);

    HBRUSH black = CreateSolidBrush(RGB(11, 11, 11));
    FillRect(memory, &client, black);
    DeleteObject(black);
    if (screen.bitmap) {
        HDC source = CreateCompatibleDC(memory);
        HGDIOBJ old_source = SelectObject(source, screen.bitmap);
        BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY);
        SelectObject(source, old_source);
        DeleteDC(source);
    }

    if (screen.primary) {
        const Json& section = cfg_->get("privacy");
        StackRects rects = ComputeStack(width, height);
        int center_x = width / 2 + drift_x_;
        int wrap = int(width * 0.8);
        // show_clock / show_date 关掉时 ComputeStack 不填那个矩形，留作零值
        auto has_box = [](const RECT& rect) { return rect.right > rect.left; };

        HFONT clock_font = MakeFont(rects.clock_px, L"Consolas", true);
        HFONT date_font = MakeFont(rects.date_px, L"Microsoft YaHei UI", false);
        HFONT head_font = MakeFont(rects.head_px, L"Microsoft YaHei UI", true);
        HFONT detail_font = MakeFont(rects.detail_px, L"Microsoft YaHei UI", false);

        if (has_box(rects.clock)) {
            DrawOutlined(memory, Utf8ToWide(FormatLocal(NowUnix(), "%H:%M:%S")), rects.clock,
                         clock_font, RGB(255, 255, 255), DT_CENTER | DT_SINGLELINE);
        }
        if (has_box(rects.date)) {
            std::string date = FormatLocal(NowUnix(), "%Y-%m-%d") + "  " + WeekdayCn(NowUnix());
            DrawOutlined(memory, Utf8ToWide(date), rects.date, date_font, RGB(242, 242, 242),
                         DT_CENTER | DT_SINGLELINE);
        }
        if (!headline_.empty()) {
            // 颜色跟着状态走，和板载灯一致
            DrawOutlined(memory, Utf8ToWide(headline_), rects.head, head_font,
                         StatusLedColor(headline_status_), DT_CENTER | DT_WORDBREAK);
        }
        if (!detail_.empty()) {
            // 先量出小字实际有多高，再在预留的空地里垂直居中（1 行 2 行都不会压到按钮）
            std::wstring text = Utf8ToWide(detail_);
            RECT measure = rects.detail;
            HGDIOBJ previous = SelectObject(memory, detail_font);
            DrawTextW(memory, text.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            SelectObject(memory, previous);
            int real_h = measure.bottom - measure.top;
            RECT rect = rects.detail;
            rect.top += (rects.detail.bottom - rects.detail.top - real_h) / 2;
            rect.bottom = rect.top + real_h;
            DrawOutlined(memory, text, rect, detail_font, RGB(232, 232, 232),
                         DT_CENTER | DT_WORDBREAK);
        }

        HFONT button_font = MakeFont(rects.button_font_px, L"Microsoft YaHei UI", false);
        HFONT small_font = MakeFont(rects.small_font_px, L"Microsoft YaHei UI", false);
        auto draw_button = [&](const RECT& rect, const std::wstring& text, HFONT font) {
            HBRUSH brush = CreateSolidBrush(RGB(43, 43, 43));
            FillRect(memory, &rect, brush);
            DeleteObject(brush);
            FrameRect(memory, &rect, (HBRUSH)GetStockObject(GRAY_BRUSH));
            HGDIOBJ previous = SelectObject(memory, font);
            SetBkMode(memory, TRANSPARENT);
            SetTextColor(memory, RGB(255, 255, 255));
            RECT text_rect = rect;
            DrawTextW(memory, text.c_str(), -1, &text_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(memory, previous);
        };

        if (!asking_) {
            draw_button(rects.ask, L"解锁屏幕", button_font);
        } else {
            HBRUSH white = CreateSolidBrush(RGB(242, 242, 242));
            FillRect(memory, &rects.entry, white);
            DeleteObject(white);
            HGDIOBJ previous = SelectObject(memory, small_font);
            SetBkMode(memory, TRANSPARENT);
            SetTextColor(memory, RGB(17, 17, 17));
            RECT text_rect = rects.entry;
            text_rect.left += (std::max)(4, rects.small_font_px / 2);
            text_rect.right -= (std::max)(4, rects.small_font_px / 2);
            std::wstring shown = reveal_ ? password_ : std::wstring(password_.size(), L'●');
            DrawTextW(memory, shown.c_str(), -1, &text_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(memory, previous);
            draw_button(rects.ok, L"确定", small_font);
            draw_button(rects.reveal, reveal_ ? L"隐藏" : L"显示", small_font);

            if (!hint_.empty()) {
                RECT hint = CenterRect(center_x, rects.entry.bottom + 10, wrap, 30);
                HGDIOBJ previous_font = SelectObject(memory, small_font);
                SetBkMode(memory, TRANSPARENT);
                SetTextColor(memory, RGB(255, 154, 154));
                DrawTextW(memory, Utf8ToWide(hint_).c_str(), -1, &hint,
                          DT_CENTER | DT_WORDBREAK);
                SelectObject(memory, previous_font);
            }
        }
        if (section.is_object() && section.get_bool("show_hint", false)) {
            RECT hint = CenterRect(center_x, rects.entry.bottom + 40, wrap, 24);
            HGDIOBJ previous = SelectObject(memory, small_font);
            SetBkMode(memory, TRANSPARENT);
            SetTextColor(memory, RGB(154, 154, 154));
            DrawTextW(memory, L"（忘了密码就在任务管理器里结束 win_agent-notify-C.exe）", -1,
                      &hint, DT_CENTER | DT_SINGLELINE);
            SelectObject(memory, previous);
        }

        DeleteObject(clock_font);
        DeleteObject(date_font);
        DeleteObject(head_font);
        DeleteObject(detail_font);
        DeleteObject(button_font);
        DeleteObject(small_font);
    }

    BitBlt(hdc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(buffer);
    DeleteDC(memory);
}

void PrivacyScreen::PaintAll() {
    for (const Screen& screen : windows_) {
        if (screen.hwnd) InvalidateRect(screen.hwnd, nullptr, FALSE);
    }
}

void PrivacyScreen::SetText(const std::string& headline, const std::string& detail) {
    SetText(headline, detail, std::string());
}

void PrivacyScreen::SetText(const std::string& headline, const std::string& detail,
                            const std::string& status) {
    if (headline_ == headline && detail_ == detail && headline_status_ == status) return;
    headline_ = headline;
    detail_ = detail;
    headline_status_ = status;
    PaintAll();
}

void PrivacyScreen::UpdateFromStates(const std::map<std::string, Json>& states) {
    if (!visible() || !cfg_) return;
    std::string status;
    auto [headline, detail] = AgentHeadline(states, *cfg_, &status);
    SetText(headline, detail, status);
}

void PrivacyScreen::Drift(bool force) {
    if (windows_.empty() || !cfg_) return;
    const Json& section = cfg_->get("privacy");
    if (!force && section.is_object() && !section.get_bool("anti_burn_in", true)) return;
    int span = section.is_object() ? int(section.get_int("drift_px", 40)) : 40;
    if (span <= 0) return;
    drift_x_ = RandomInt(-span, span);
    drift_y_ = RandomInt(-span, span);
    // 换位置的同时换壁纸：整块屏的像素都换一遍，防烧屏更彻底
    if (wallpapers_.size() > 1) SetWallpaperIndex(wall_index_ + 1);
    PaintAll();
}

const PrivacyScreen::Screen* PrivacyScreen::FindScreen(HWND hwnd) const {
    for (const Screen& screen : windows_) {
        if (screen.hwnd == hwnd) return &screen;
    }
    return nullptr;
}

void PrivacyScreen::DropCaret() { DestroyCaret(); }

void PrivacyScreen::RefreshCaret() {
    HWND hwnd = PrimaryWindow();
    if (!hwnd || !asking_) {
        DropCaret();
        return;
    }
    const Screen* screen = FindScreen(hwnd);
    if (!screen) return;
    RECT client{};
    GetClientRect(hwnd, &client);
    int width = client.right > 0 ? client.right : screen->rect.width;
    int height = client.bottom > 0 ? client.bottom : screen->rect.height;
    StackRects rects = ComputeStack(width, height);
    int box_height = rects.entry.bottom - rects.entry.top;
    int caret_height = (std::max)(10, box_height * 5 / 8);

    // 光标高度变了就得重建（CreateCaret 的高度是固定的）
    if (caret_height != caret_height_) {
        DropCaret();
        if (!CreateCaret(hwnd, nullptr, 2, caret_height)) return;
        caret_height_ = caret_height;
    }

    // 用画密码框的同一个字体量一下文字宽度，光标跟在最后一个字符后面
    HDC dc = GetDC(hwnd);
    HFONT font = MakeFont(rects.small_font_px, L"Microsoft YaHei UI", false);
    HGDIOBJ previous = SelectObject(dc, font);
    std::wstring shown = reveal_ ? password_ : std::wstring(password_.size(), L'●');
    SIZE size{};
    GetTextExtentPoint32W(dc, shown.c_str(), (int)shown.size(), &size);
    SelectObject(dc, previous);
    DeleteObject(font);
    ReleaseDC(hwnd, dc);

    // 左边距和画字时用的那一份保持一致，光标才不会偏
    int pad = (std::max)(4, rects.small_font_px / 2);
    int x = rects.entry.left + pad + size.cx;
    if (x > rects.entry.right - pad) x = rects.entry.right - pad;   // 别跑出框外
    caret_x_ = x;
    caret_y_ = rects.entry.top + (box_height - caret_height) / 2;
    SetCaretPos(caret_x_, caret_y_);
    ShowCaret(hwnd);
}

void PrivacyScreen::RequestUnlock() {
    asking_ = true;
    reveal_ = false;
    password_.clear();
    hint_.clear();
    PaintAll();
    RefreshCaret();
}

void PrivacyScreen::TryUnlock() {
    DropCaret();
    if (cfg_ && CheckPassword(*cfg_, WideToUtf8(password_))) {
        LogInfo("privacy", "隐私屏幕已解锁");
        Hide();
        if (on_unlock) on_unlock();
        return;
    }
    hint_ = "密码不对。检查一下输入法是不是切到了中文；看不清就点「显示」";
    password_.clear();
    LogInfo("privacy", "隐私屏幕：密码错误");
    PaintAll();
    RefreshCaret();
}

void PrivacyScreen::HandleKey(UINT message, WPARAM wparam) {
    if (!asking_) return;
    if (message == WM_CHAR) {
        wchar_t ch = wchar_t(wparam);
        if (ch == L'\r' || ch == L'\n') {
            TryUnlock();
            return;
        }
        if (ch == L'\b') {
            if (!password_.empty()) password_.pop_back();
            PaintAll();
            RefreshCaret();
            return;
        }
        if (ch == 0x1B) return;                 // Esc：不放行
        if (ch < 0x20 || ch == 0x7F) return;    // 其它控制字符丢掉
        password_.push_back(ch);
        PaintAll();
        RefreshCaret();
        return;
    }
    if (message == WM_KEYDOWN) {
        if (wparam == VK_RETURN) {
            TryUnlock();
            return;
        }
        if (wparam == VK_BACK) {
            if (!password_.empty()) password_.pop_back();
            PaintAll();
            RefreshCaret();
        }
    }
}

LRESULT PrivacyScreen::OnMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto find_screen = [&]() -> const Screen* {
        for (const Screen& screen : windows_) {
            if (screen.hwnd == hwnd) return &screen;
        }
        return nullptr;
    };

    switch (message) {
        case WM_PAINT: {
            // 光标是系统直接画在屏幕上的，整屏 BitBlt 会把它盖掉 —— 先藏后显
            if (asking_) HideCaret(hwnd);
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            const Screen* screen = find_screen();
            if (screen) PaintScreen(*screen, dc);
            EndPaint(hwnd, &paint);
            if (asking_) ShowCaret(hwnd);
            return 0;
        }
        case WM_SETFOCUS:
            if (asking_) RefreshCaret();
            return 0;
        case WM_KILLFOCUS:
            if (asking_) HideCaret(hwnd);
            break;
        case WM_ERASEBKGND:
            return 1;   // 自己在 WM_PAINT 里铺满，不再擦一遍（避免闪）
        case WM_TIMER:
            if (wparam == kTickTimer) {
                const Screen* screen = find_screen();
                if (screen && screen->primary && cfg_) {
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    const Json& section = cfg_->get("privacy");
                    bool anti = !section.is_object() || section.get_bool("anti_burn_in", true);
                    double interval =
                        section.is_object() ? section.get_double("drift_sec", 60.0) : 60.0;
                    double now = NowUnix();
                    if (anti && interval > 0 && now - last_drift_ >= interval) {
                        last_drift_ = now;
                        Drift(true);
                    } else {
                        InvalidateRect(hwnd, nullptr, FALSE);   // 时钟每秒都要变
                    }
                }
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const Screen* screen = find_screen();
            if (!screen || !screen->primary) return 0;
            int x = GET_X_LPARAM(lparam);
            int y = GET_Y_LPARAM(lparam);
            RECT click_client{};
            GetClientRect(hwnd, &click_client);
            StackRects rects = ComputeStack(click_client.right, click_client.bottom);
            if (!asking_) {
                if (PointIn(rects.ask, x, y)) RequestUnlock();
                return 0;
            }
            if (PointIn(rects.ok, x, y)) {
                TryUnlock();
            } else if (PointIn(rects.reveal, x, y)) {
                reveal_ = !reveal_;
                PaintAll();
                RefreshCaret();   // 掩码换成明文，字宽变了，光标要跟着挪
            } else if (PointIn(rects.entry, x, y)) {
                SetFocus(hwnd);
            }
            return 0;
        }
        case WM_CHAR:
            HandleKey(message, wparam);
            return 0;
        case WM_KEYDOWN: {
            if (wparam == VK_ESCAPE) return 0;                                  // 不响应
            if (wparam == 'W' && (GetKeyState(VK_CONTROL) & 0x8000)) return 0;   // Ctrl+W
            HandleKey(message, wparam);
            return 0;
        }
        case WM_SYSKEYDOWN:
            if (wparam == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) return 0;   // Alt+F4
            if (wparam == VK_SPACE && (GetKeyState(VK_MENU) & 0x8000)) return 0;  // Alt+Space
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_CLOSE) return 0;   // 关闭按钮 / Alt+F4 走这里
            if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            return 0;                                      // 关不掉
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace cn
