#include "gui.h"

#include <commdlg.h>
#include <commctrl.h>

#include <algorithm>
#include <thread>

#include "feishu.h"
#include "device.h"
#include "password.h"
#include "push.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/str.h"
#include "util/times.h"

namespace cn {
namespace {

const wchar_t* kWindowClass = L"CodexNotifyMainWindow";
constexpr int kRefreshTimer = 1;
constexpr int kRefreshMs = 700;
constexpr int kAutoStartTimer = 2;

// 窗口内容宽度（客户区逻辑像素）。界面刻意收窄，够用就行。
constexpr int kContentWidth = 640;
constexpr int kMargin = 16;
constexpr int kInner = kContentWidth - kMargin * 2;   // 608
constexpr int kIconId = 1;                            // res/app.rc 里的图标 id

constexpr int kIdCodex = 100;
constexpr int kIdClaude = 101;
constexpr int kIdFeishu = 102;
constexpr int kIdTone = 103;
constexpr int kIdSpeech = 104;
constexpr int kIdPrivacy = 105;
constexpr int kIdFeishuConfig = 106;
constexpr int kIdStart = 110;
constexpr int kIdStop = 111;
constexpr int kIdTest = 112;
constexpr int kIdForget = 113;
constexpr int kIdQuit = 114;
constexpr int kIdWallpaper = 120;
constexpr int kIdPassword = 121;
constexpr int kIdDeviceBase = 200;
constexpr int kIdDeviceConfigBase = 1000;
constexpr int kDeviceAddressEdit = 1100;
constexpr int kFeishuWebhookEdit = 1200;
constexpr int kFeishuSecretEdit = 1201;

struct AddressDialogState {
    HWND edit = nullptr;
    bool done = false;
    bool accepted = false;
    std::string value;
};

LRESULT CALLBACK AddressDialogProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = (AddressDialogState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        auto* create = (CREATESTRUCTW*)lparam;
        state = (AddressDialogState*)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    }
    if (!state) return DefWindowProcW(hwnd, message, wparam, lparam);
    if (message == WM_CREATE) {
        HWND label = CreateWindowExW(0, L"STATIC",
            L"完整推送地址（可使用域名或 IP），例如：",
            WS_CHILD | WS_VISIBLE, 16, 16, 420, 22, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        (void)label;
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(state->value).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 16, 42, 430, 26,
            hwnd, (HMENU)kDeviceAddressEdit, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            282, 82, 76, 28, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            370, 82, 76, 28, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
        SetFocus(state->edit);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
        if (LOWORD(wparam) == IDOK) {
            int length = GetWindowTextLengthW(state->edit);
            std::vector<wchar_t> text((size_t)length + 1);
            GetWindowTextW(state->edit, text.data(), length + 1);
            state->value = Trim(WideToUtf8(text.data()));
            state->accepted = !state->value.empty();
        }
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_CLOSE) {
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool EditAddress(HWND owner, const std::string& current, std::string* updated) {
    static bool registered = false;
    const wchar_t* class_name = L"CodexNotifyAddressDialog";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AddressDialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = class_name;
        RegisterClassExW(&wc);
        registered = true;
    }
    AddressDialogState state;
    state.value = current;
    RECT parent{};
    GetWindowRect(owner, &parent);
    int x = parent.left + ((parent.right - parent.left) - 480) / 2;
    int y = parent.top + ((parent.bottom - parent.top) - 160) / 2;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name, L"设置推送地址",
        WS_CAPTION | WS_SYSMENU, x, y, 480, 160, owner, nullptr,
        GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (state.accepted) *updated = state.value;
    return state.accepted;
}

struct FeishuDialogState {
    HWND webhook = nullptr;
    HWND secret = nullptr;
    bool done = false;
    bool accepted = false;
    std::string webhook_value;
    std::string secret_value;
};

LRESULT CALLBACK FeishuDialogProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = (FeishuDialogState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        auto* create = (CREATESTRUCTW*)lparam;
        state = (FeishuDialogState*)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    }
    if (!state) return DefWindowProcW(hwnd, message, wparam, lparam);
    if (message == WM_CREATE) {
        CreateWindowExW(0, L"STATIC", L"机器人 Webhook 地址：", WS_CHILD | WS_VISIBLE,
            16, 16, 440, 22, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        state->webhook = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            Utf8ToWide(state->webhook_value).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 16, 40, 476, 25,
            hwnd, (HMENU)kFeishuWebhookEdit, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"STATIC", L"签名密钥（留空即不使用密钥）：", WS_CHILD | WS_VISIBLE,
            16, 76, 440, 22, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        state->secret = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
            16, 100, 476, 25, hwnd, (HMENU)kFeishuSecretEdit,
            GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            328, 136, 76, 28, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            416, 136, 76, 28, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
        SetFocus(state->webhook);
        SendMessageW(state->webhook, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
        if (LOWORD(wparam) == IDOK) {
            int url_length = GetWindowTextLengthW(state->webhook);
            std::vector<wchar_t> url((size_t)url_length + 1);
            GetWindowTextW(state->webhook, url.data(), url_length + 1);
            int secret_length = GetWindowTextLengthW(state->secret);
            std::vector<wchar_t> secret((size_t)secret_length + 1);
            GetWindowTextW(state->secret, secret.data(), secret_length + 1);
            state->webhook_value = Trim(WideToUtf8(url.data()));
            state->secret_value = WideToUtf8(secret.data());
            state->accepted = true;
        }
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_CLOSE) {
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool EditFeishu(HWND owner, FeishuDialogState* state) {
    static bool registered = false;
    const wchar_t* class_name = L"CodexNotifyFeishuDialog";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = FeishuDialogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = class_name;
        RegisterClassExW(&wc);
        registered = true;
    }
    RECT parent{};
    GetWindowRect(owner, &parent);
    int x = parent.left + ((parent.right - parent.left) - 530) / 2;
    int y = parent.top + ((parent.bottom - parent.top) - 210) / 2;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name, L"飞书机器人配置",
        WS_CAPTION | WS_SYSMENU, x, y, 530, 210, owner, nullptr,
        GetModuleHandleW(nullptr), state);
    if (!dialog) return false;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG message{};
    while (!state->done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    return state->accepted;
}

// 和 ESP32 固件 / 飞书卡片保持一致的颜色
COLORREF StatusColor(const std::string& status) {
    std::string key = ToLowerAscii(status);
    if (key == "working") return RGB(0xFF, 0x96, 0x00);
    if (key == "waiting_approval") return RGB(0xE0, 0x1B, 0x24);
    if (key == "completed") return RGB(0x00, 0x5A, 0xFF);
    if (key == "interrupted") return RGB(0x9A, 0xA0, 0xA6);
    if (key == "idle") return RGB(0x00, 0xC8, 0x00);
    if (key == "info") return RGB(0x1E, 0x90, 0xFF);
    return RGB(0x9A, 0xA0, 0xA6);
}

std::wstring StatusText(const std::string& status) {
    std::string key = ToLowerAscii(status);
    if (key == "working") return L"工作中";
    if (key == "waiting_approval") return L"等待审批";
    if (key == "completed") return L"已完成";
    if (key == "interrupted") return L"已中断";
    if (key == "idle") return L"空闲";
    if (key == "info") return L"通知";
    return L"未开始";
}

void SetFont(HWND control, HFONT font) {
    if (control && font) SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

}  // namespace

void MainWindow::RegisterWindowClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = MainWindow::WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    window_class.lpszClassName = kWindowClass;
    // 类里也挂上图标：这样任务栏在 WM_SETICON 之前就显示对了
    HINSTANCE module = GetModuleHandleW(nullptr);
    window_class.hIcon = (HICON)LoadImageW(module, MAKEINTRESOURCEW(kIconId), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXICON),
                                           GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    window_class.hIconSm = (HICON)LoadImageW(module, MAKEINTRESOURCEW(kIconId), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassExW(&window_class);
    registered = true;
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* window = (MainWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        CREATESTRUCTW* create = (CREATESTRUCTW*)lparam;
        window = (MainWindow*)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)window);
    }
    if (window) return window->HandleMessage(hwnd, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void MainWindow::Build() {
    const int card_width = (kInner - 8) / 2;         // 两张卡片 + 中间 8 像素缝

    // 两张 agent 卡片
    const wchar_t* agent_names[2] = {L"Codex", L"Claude Code"};
    for (int i = 0; i < 2; ++i) {
        int left = kMargin + i * (card_width + 8);
        HWND group = CreateWindowExW(0, L"BUTTON", agent_names[i],
                                     WS_CHILD | WS_VISIBLE | BS_GROUPBOX, left, 12, card_width,
                                     84, hwnd_, nullptr, instance_, nullptr);
        cards_[i].state = CreateWindowExW(0, L"STATIC", L"未开始",
                                          WS_CHILD | WS_VISIBLE | SS_LEFT, left + 16, 38,
                                          card_width - 32, 24, hwnd_, nullptr, instance_, nullptr);
        cards_[i].detail = CreateWindowExW(0, L"STATIC", L"—",
                                           WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                           left + 16, 66, card_width - 32, 20, hwnd_, nullptr,
                                           instance_, nullptr);
        SetFont(group, font_);
        SetFont(cards_[i].state, font_title_);
        SetFont(cards_[i].detail, font_);
    }

    HWND agents_group = CreateWindowExW(0, L"BUTTON", L"监控哪些 agent",
                                        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, kMargin, 104, kInner,
                                        56, hwnd_, nullptr, instance_, nullptr);
    check_codex_ = CreateWindowExW(0, L"BUTTON", L"Codex 会话日志",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   kMargin + 18, 126, 168, 24, hwnd_, (HMENU)kIdCodex, instance_,
                                   nullptr);
    check_claude_ = CreateWindowExW(0, L"BUTTON", L"Claude Code（只读）",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    kMargin + 198, 126, 200, 24, hwnd_, (HMENU)kIdClaude,
                                    instance_, nullptr);
    CheckDlgButton(hwnd_, kIdCodex, BST_CHECKED);
    SendMessageW(check_claude_, BM_SETCHECK,
                 app_->config().flag("claude", "enabled", true) ? BST_CHECKED : BST_UNCHECKED, 0);

    // 推送通道
    HWND channel_group = CreateWindowExW(0, L"BUTTON", L"推送到哪些通道",
                                         WS_CHILD | WS_VISIBLE | BS_GROUPBOX, kMargin, 168,
                                         kInner, 76, hwnd_, nullptr, instance_, nullptr);
    check_feishu_ = CreateWindowExW(0, L"BUTTON", L"飞书",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    kMargin + 18, 190, 62, 24, hwnd_, (HMENU)kIdFeishu, instance_,
                                    nullptr);
    SendMessageW(check_feishu_, BM_SETCHECK,
                 app_->config().flag("feishu", "enabled", true) ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND feishu_config = CreateWindowExW(0, L"BUTTON", L"设置…",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kMargin + 82, 190, 54, 24,
        hwnd_, (HMENU)kIdFeishuConfig, instance_, nullptr);
    SetFont(feishu_config, font_);

    int x = kMargin + 144;
    const int right_edge = kMargin + kInner - 8;
    const Json& legacy = app_->config().get("device");
    if (legacy.is_object() &&
        !Trim(legacy.get_str("url")).empty()) {
        std::string name = Trim(legacy.get_str("name"));
        if (name.empty()) name = "device";
        HWND box = CreateWindowExW(0, L"BUTTON", Utf8ToWide(name).c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, x, 190,
                                   102, 24, hwnd_, (HMENU)(INT_PTR)(kIdDeviceBase + 0), instance_,
                                   nullptr);
        SendMessageW(box, BM_SETCHECK,
                     legacy.get_bool("enabled", true) ? BST_CHECKED : BST_UNCHECKED, 0);
        check_devices_.push_back(box);
        device_config_indices_.push_back(-1);
        HWND config = CreateWindowExW(0, L"BUTTON", L"设置…",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x + 104, 190, 54, 24,
            hwnd_, (HMENU)(INT_PTR)(kIdDeviceConfigBase + (int)check_devices_.size() - 1),
            instance_, nullptr);
        SetFont(config, font_);
        x += 164;
    }
    const Json& devices = app_->config().get("devices");
    for (size_t i = 0; i < devices.size(); ++i) {
        const Json& item = devices.at(i);
        if (!item.is_object()) continue;
        std::string name = Trim(item.get_str("name"));
        if (name.empty()) name = "device" + std::to_string(i + 1);
        int width = (std::min)(102, right_edge - x - 56);  // 为地址设置按钮留位置
        if (width < 52) break;
        HWND box = CreateWindowExW(0, L"BUTTON", Utf8ToWide(name).c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, x, 190,
                                   width, 24, hwnd_, (HMENU)(INT_PTR)(kIdDeviceBase + 1 + (int)i),
                                   instance_, nullptr);
        SendMessageW(box, BM_SETCHECK,
                     item.get_bool("enabled", true) ? BST_CHECKED : BST_UNCHECKED, 0);
        check_devices_.push_back(box);
        device_config_indices_.push_back((int)i);
        HWND config = CreateWindowExW(0, L"BUTTON", L"设置…",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x + width + 2, 190, 54, 24,
            hwnd_, (HMENU)(INT_PTR)(kIdDeviceConfigBase + (int)check_devices_.size() - 1),
            instance_, nullptr);
        SetFont(config, font_);
        x += width + 62;
    }
    if (check_devices_.empty()) {
        HWND label = CreateWindowExW(0, L"STATIC", L"（还没配局域网设备）",
                                     WS_CHILD | WS_VISIBLE, x, 193, 160, 20, hwnd_, nullptr,
                                     instance_, nullptr);
        SetFont(label, font_);
    }

    const Json& sound = app_->config().get("local_sound");
    check_tone_ = CreateWindowExW(0, L"BUTTON", L"本机提示音",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                  kMargin + 18, 216, 112, 24, hwnd_, (HMENU)kIdTone, instance_,
                                  nullptr);
    check_speech_ = CreateWindowExW(0, L"BUTTON", L"本机朗读",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    kMargin + 140, 216, 112, 24, hwnd_, (HMENU)kIdSpeech,
                                    instance_, nullptr);
    SendMessageW(check_tone_, BM_SETCHECK,
                 sound.get_bool("enabled", true) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(check_speech_, BM_SETCHECK,
                 sound.get_bool("speech", false) ? BST_CHECKED : BST_UNCHECKED, 0);

    // 隐私屏幕
    HWND privacy_group = CreateWindowExW(
        0, L"BUTTON", L"隐私屏幕（不是真锁屏：agent 照常干活，只是别人看不到内容）",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, kMargin, 252, kInner, 62, hwnd_, nullptr, instance_,
        nullptr);
    check_privacy_ = CreateWindowExW(0, L"BUTTON", L"开启隐私屏幕",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     kMargin + 18, 274, 116, 24, hwnd_, (HMENU)kIdPrivacy,
                                     instance_, nullptr);
    HWND wallpaper = CreateWindowExW(0, L"BUTTON", L"选择壁纸…",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     kMargin + 142, 272, 100, 26, hwnd_, (HMENU)kIdWallpaper,
                                     instance_, nullptr);
    HWND setpwd = CreateWindowExW(0, L"BUTTON", L"设置解锁密码…",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  kMargin + 250, 272, 122, 26, hwnd_, (HMENU)kIdPassword,
                                  instance_, nullptr);
    label_privacy_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     kMargin + 380, 276, kInner - 380, 22, hwnd_, nullptr,
                                     instance_, nullptr);

    // 按钮
    button_start_ = CreateWindowExW(0, L"BUTTON", L"启动监控",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kMargin,
                                    328, 100, 30, hwnd_, (HMENU)kIdStart, instance_, nullptr);
    button_stop_ = CreateWindowExW(0, L"BUTTON", L"停止监控",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   kMargin + 108, 328, 100, 30, hwnd_, (HMENU)kIdStop, instance_,
                                   nullptr);
    HWND test = CreateWindowExW(0, L"BUTTON", L"测试推送",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                kMargin + 216, 328, 100, 30, hwnd_, (HMENU)kIdTest, instance_,
                                nullptr);
    HWND forget = CreateWindowExW(0, L"BUTTON", L"重置状态",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  kMargin + 324, 328, 100, 30, hwnd_, (HMENU)kIdForget,
                                  instance_, nullptr);
    HWND quit = CreateWindowExW(0, L"BUTTON", L"退出程序",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                kMargin + kInner - 110, 328, 110, 30, hwnd_, (HMENU)kIdQuit,
                                instance_, nullptr);
    EnableWindow(button_stop_, FALSE);

    status_ = CreateWindowExW(0, L"STATIC", L"未启动", WS_CHILD | WS_VISIBLE | SS_LEFT, kMargin,
                              372, kInner, 22, hwnd_, nullptr, instance_, nullptr);
    HWND tip = CreateWindowExW(
        0, L"STATIC",
        L"关掉窗口就是退出程序；想开机自动跑，把快捷方式丢进「启动」文件夹。",
        WS_CHILD | WS_VISIBLE | SS_LEFT, kMargin, 396, kInner, 22, hwnd_, nullptr, instance_,
        nullptr);

    for (HWND control : {agents_group, channel_group, privacy_group, check_codex_, check_claude_,
                         check_feishu_, check_tone_, check_speech_, check_privacy_, wallpaper,
                         setpwd, label_privacy_, button_start_, button_stop_, test, forget, quit,
                         status_, tip}) {
        SetFont(control, font_);
    }
    for (HWND device : check_devices_) SetFont(device, font_);
    SetFont(status_, font_bold_);
    if (HasPassword(app_->config())) {
        SetWindowTextW(label_privacy_, L"密码已设置");
    }
}

void MainWindow::Refresh() {
    auto latest = app_->monitor().LatestByAgent();
    const char* keys[2] = {"codex", "claude"};
    for (int i = 0; i < 2; ++i) {
        auto found = latest.find(keys[i]);
        std::string status;
        std::string name;
        std::string when;
        if (found != latest.end()) {
            status = found->second.get_str("status");
            name = found->second.get_str("name");
            if (name.empty()) name = found->second.get_str("thread_id");
            when = found->second.get_str("updated_at");
        }
        std::wstring text = StatusText(status);
        SetWindowTextW(cards_[i].state, text.c_str());
        std::wstring detail = name.empty()
                                  ? L"—"
                                  : Utf8ToWide(name + "  " + (when.size() > 11 ? when.substr(11)
                                                                               : when));
        SetWindowTextW(cards_[i].detail, detail.c_str());
        // 卡片颜色跟着状态走
        HDC dc = GetDC(cards_[i].state);
        SetTextColor(dc, status.empty() ? RGB(0x66, 0x66, 0x66) : StatusColor(status));
        ReleaseDC(cards_[i].state, dc);
        last_detail_[i] = status;
    }
    app_->privacy().UpdateFromStates(latest);
    Say();
}

void MainWindow::Say() {
    if (!app_->running()) {
        SetWindowTextW(status_, L"未启动（点「启动监控」开始）");
        return;
    }
    std::vector<std::string> what;
    if (app_->codex_on()) what.push_back("Codex");
    if (app_->claude_on()) what.push_back("Claude Code");
    std::string who = what.empty() ? "没有勾选任何 agent" : Join(what, " + ");
    std::string text = Cat("监控运行中（", who, "）· 接口 http://", app_->api_host(), ":",
                           app_->api_port());
    SetWindowTextW(status_, Utf8ToWide(text).c_str());
}

void MainWindow::OnStart() {
    if (app_->running()) return;
    app_->SetCodexOn(IsDlgButtonChecked(hwnd_, kIdCodex) == BST_CHECKED);
    app_->SetClaudeOn(IsDlgButtonChecked(hwnd_, kIdClaude) == BST_CHECKED);
    std::string error = app_->Start();
    if (!error.empty()) {
        std::wstring message = Utf8ToWide(
            error + "\n\n多半是你已经开了另一个监控窗口（端口被占用）。");
        MessageBoxW(hwnd_, message.c_str(), L"启动失败", MB_OK | MB_ICONERROR);
        Say();
        return;
    }
    EnableWindow(button_start_, FALSE);
    EnableWindow(button_stop_, TRUE);
    Say();
}

void MainWindow::OnStop() {
    app_->Stop();
    app_->store().Load();
    EnableWindow(button_start_, TRUE);
    EnableWindow(button_stop_, FALSE);
    Say();
}

void MainWindow::OnTest() {
    OnToggleChannels();
    // 结果回到主线程再弹框（别让联网把窗口卡住）
    HWND target = hwnd_;
    std::thread([this, target] {
        PushOptions options;
        options.text = "这是一条测试消息：听到提示音 / 收到卡片 / 灯变红，就说明通道通了。";
        options.title = "界面测试";
        options.source = "界面测试";
        options.agent = "codex";
        options.force_feishu = true;   // 点「测试推送」就是要把每一条通道都试一遍
        auto [ok, detail] = PushStatus(app_->config(), "waiting_approval", options);
        (void)ok;
        PostMessageW(target, WM_APP + 1, 0, (LPARAM)new std::wstring(Utf8ToWide(detail)));
    }).detach();
}

void MainWindow::OnForget() {
    std::thread([this] {
        for (const char* agent : {"codex", "claude"}) {
            PushOptions options;
            options.source = "界面重置";
            options.agent = agent;
            PushStatus(app_->config(), "idle", options);
        }
    }).detach();
}

void MainWindow::OnQuit() { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }

void MainWindow::OnToggleAgents() {
    app_->SetCodexOn(IsDlgButtonChecked(hwnd_, kIdCodex) == BST_CHECKED);
    app_->SetClaudeOn(IsDlgButtonChecked(hwnd_, kIdClaude) == BST_CHECKED);
    Persist();
    Say();
}

void MainWindow::OnToggleChannels() {
    Json& cfg = app_->config().data;
    Json& feishu = cfg["feishu"];
    if (!feishu.is_object()) feishu = Json::Object();
    feishu.set("enabled", Json::Bool(IsDlgButtonChecked(hwnd_, kIdFeishu) == BST_CHECKED));

    Json& legacy = cfg["device"];
    size_t visible_index = 0;
    if (legacy.is_object() && !Trim(legacy.get_str("url")).empty() &&
        visible_index < check_devices_.size()) {
        legacy.set("enabled", Json::Bool(SendMessageW(check_devices_[visible_index], BM_GETCHECK,
                                                       0, 0) == BST_CHECKED));
        ++visible_index;
    }
    Json& devices = cfg["devices"];
    for (size_t i = 0; i < devices.size() && visible_index < check_devices_.size(); ++i) {
        if (devices.at(i).is_object()) {
            devices.at(i).set(
                "enabled", Json::Bool(SendMessageW(check_devices_[visible_index], BM_GETCHECK,
                                                    0, 0) == BST_CHECKED));
            ++visible_index;
        }
    }

    bool tone = IsDlgButtonChecked(hwnd_, kIdTone) == BST_CHECKED;
    Json& sound = cfg["local_sound"];
    if (!sound.is_object()) sound = Json::Object();
    sound.set("enabled", Json::Bool(tone));
    sound.set("tone", Json::Bool(tone));
    sound.set("speech", Json::Bool(IsDlgButtonChecked(hwnd_, kIdSpeech) == BST_CHECKED));

    Persist();
}

void MainWindow::OnConfigureDevice(size_t visible_index) {
    if (visible_index >= device_config_indices_.size()) return;
    int config_index = device_config_indices_[visible_index];
    Json& target = config_index < 0
                       ? app_->config().data["device"]
                       : app_->config().data["devices"].at((size_t)config_index);
    std::string current = Trim(target.get_str("url"));
    if (current.empty()) current = DeviceResolveUrl(target);
    std::string updated;
    if (!EditAddress(hwnd_, current, &updated)) return;
    target.set("url", Json::Str(updated));
    target.remove("ip");
    target.remove("port");
    target.remove("path");
    Persist();
    if (!SaveConfig(app_->config().data, app_->config_path())) {
        MessageBoxW(hwnd_, L"保存推送地址失败，请检查配置文件权限。", L"保存失败",
                    MB_OK | MB_ICONERROR);
        return;
    }
    LogInfo("gui", Cat("已更新设备推送地址：", target.get_str("name"), " -> ", updated));
}

void MainWindow::OnConfigureFeishu() {
    FeishuDialogState state;
    state.webhook_value = app_->config().str("feishu", "webhook_url");
    if (!EditFeishu(hwnd_, &state)) return;
    if (!state.webhook_value.empty() && !StartsWith(state.webhook_value, "https://") &&
        !StartsWith(state.webhook_value, "http://")) {
        MessageBoxW(hwnd_, L"Webhook 地址应以 http:// 或 https:// 开头。", L"地址格式错误",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    Json& feishu = app_->config().data["feishu"];
    feishu.set("webhook_url", Json::Str(state.webhook_value));
    feishu.set("secret", Json::Str(state.secret_value));
    Persist();
    if (!SaveConfig(app_->config().data, app_->config_path())) {
        MessageBoxW(hwnd_, L"保存飞书配置失败，请检查配置文件权限。", L"保存失败",
                    MB_OK | MB_ICONERROR);
        return;
    }
    LogInfo("gui", "已更新飞书机器人配置");
}

void MainWindow::OnTogglePrivacy() {
    bool want = IsDlgButtonChecked(hwnd_, kIdPrivacy) == BST_CHECKED;
    if (want) {
        if (!HasPassword(app_->config())) {
            if (!OnSetPassword()) {
                CheckDlgButton(hwnd_, kIdPrivacy, BST_UNCHECKED);
                return;
            }
        }
        app_->privacy().Show();
    } else {
        app_->privacy().Hide();
    }
    app_->PersistPrivacyEnabled(want);
    Say();
}

bool MainWindow::OnSetPassword() {
    if (!PromptSetPassword(hwnd_, &app_->config())) return false;
    SetWindowTextW(label_privacy_, L"密码已设置");
    app_->PersistPrivacyEnabled(IsDlgButtonChecked(hwnd_, kIdPrivacy) == BST_CHECKED);
    return true;
}

void MainWindow::OnPickWallpaper() {
    // OFN_ALLOWMULTISELECT：可以一次框选多张
    std::vector<wchar_t> buffer(64 * 1024, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"图片\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0所有文件\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = DWORD(buffer.size());
    dialog.lpstrTitle = L"选壁纸（可以按住 Ctrl / Shift 一次选多张）";
    dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return;

    std::vector<std::string> paths;
    const wchar_t* first = buffer.data();
    const wchar_t* second = first + wcslen(first) + 1;
    if (*second == L'\0') {
        paths.push_back(WideToUtf8(first));   // 只选了一个
    } else {
        std::wstring directory = first;
        for (const wchar_t* name = second; *name; name += wcslen(name) + 1) {
            paths.push_back(WideToUtf8(directory + L"\\" + name));
        }
    }
    if (paths.empty()) return;

    Json& privacy = app_->config().data["privacy"];
    if (!privacy.is_object()) privacy = Json::Object();
    Json list = Json::Array();
    for (const std::string& path : paths) list.push_back(Json::Str(path));
    privacy.set("wallpaper", list);

    std::string first_name = PathFileName(paths[0]);
    std::string text = paths.size() == 1
                           ? "壁纸：" + first_name
                           : Cat("壁纸：", first_name, " 等 ", (long long)paths.size(), " 张");
    SetWindowTextW(label_privacy_, Utf8ToWide(text).c_str());

    app_->privacy().SetConfig(&app_->config());
    if (app_->privacy().visible()) {
        app_->privacy().Hide();     // 立刻换上新壁纸
        app_->privacy().Show();
    }
    Persist();
}

void MainWindow::Persist() {
    std::vector<bool> devices;
    for (HWND box : check_devices_) {
        devices.push_back(SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    app_->PersistToggles(IsDlgButtonChecked(hwnd_, kIdFeishu) == BST_CHECKED, devices,
                         IsDlgButtonChecked(hwnd_, kIdTone) == BST_CHECKED,
                         IsDlgButtonChecked(hwnd_, kIdSpeech) == BST_CHECKED,
                         IsDlgButtonChecked(hwnd_, kIdClaude) == BST_CHECKED,
                         IsDlgButtonChecked(hwnd_, kIdPrivacy) == BST_CHECKED, {});
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_COMMAND: {
            int id = LOWORD(wparam);
            switch (id) {
                case kIdStart: OnStart(); return 0;
                case kIdStop: OnStop(); return 0;
                case kIdTest: OnTest(); return 0;
                case kIdForget: OnForget(); return 0;
                case kIdQuit: OnQuit(); return 0;
                case kIdWallpaper: OnPickWallpaper(); return 0;
                case kIdPassword: OnSetPassword(); return 0;
                case kIdFeishuConfig: OnConfigureFeishu(); return 0;
                case kIdCodex:
                case kIdClaude: OnToggleAgents(); return 0;
                case kIdFeishu:
                case kIdTone:
                case kIdSpeech: OnToggleChannels(); return 0;
                case kIdPrivacy: OnTogglePrivacy(); return 0;
                default:
                    if (id >= kIdDeviceConfigBase &&
                        id < kIdDeviceConfigBase + (int)device_config_indices_.size()) {
                        OnConfigureDevice((size_t)(id - kIdDeviceConfigBase));
                        return 0;
                    }
                    if (id > kIdDeviceBase) OnToggleChannels();
                    return 0;
            }
        }
        case WM_TIMER:
            if (wparam == kRefreshTimer) {
                Refresh();
            } else if (wparam == kAutoStartTimer) {
                KillTimer(hwnd, kAutoStartTimer);
                OnStart();
            }
            return 0;
        case WM_CTLCOLORSTATIC: {
            // 状态字用卡片颜色；其它静态控件保持默认
            HWND control = (HWND)lparam;
            for (int i = 0; i < 2; ++i) {
                if (control == cards_[i].state) {
                    HDC dc = (HDC)wparam;
                    SetTextColor(dc, last_detail_[i].empty() ? RGB(0x66, 0x66, 0x66)
                                                             : StatusColor(last_detail_[i]));
                    SetBkMode(dc, TRANSPARENT);
                    return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
                }
            }
            break;
        }
        case WM_APP + 1: {
            std::wstring* detail = (std::wstring*)lparam;
            if (detail) {
                MessageBoxW(hwnd, detail->c_str(), L"测试推送", MB_OK | MB_ICONINFORMATION);
                delete detail;
            }
            return 0;
        }
        case WM_CLOSE:
            app_->Shutdown();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int MainWindow::Run(HINSTANCE instance, AppContext* app, const std::string& config_path) {
    (void)config_path;
    app_ = app;
    instance_ = instance;
    RegisterWindowClass();

    int dpi = 96;
    HDC screen = GetDC(nullptr);
    if (screen) {
        dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
    }
    font_ = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Microsoft YaHei UI");
    font_bold_ = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    font_title_ = CreateFontW(-MulDiv(14, dpi, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

    RECT rect{0, 0, MulDiv(kContentWidth, dpi, 96), MulDiv(430, dpi, 96)};
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    hwnd_ = CreateWindowExW(0, kWindowClass, L"Codex / Claude 状态监控",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, width,
                            height, nullptr, nullptr, instance, this);
    if (!hwnd_) return 1;
    // 标题栏和任务栏用 exe 里的图标（res/app.rc 里那个）
    HICON icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(kIconId), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (icon) {
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    }
    HICON big = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(kIconId), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                  LR_DEFAULTCOLOR);
    if (big) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)big);

    Build();
    // 每次启动都把"锁屏"复位：上次关程序时开着是上次的事
    if (app_->config().flag("privacy", "enabled", false)) {
        app_->config().data["privacy"].set("enabled", Json::Bool(false));
        CheckDlgButton(hwnd_, kIdPrivacy, BST_UNCHECKED);
        app_->PersistPrivacyEnabled(false);
        LogInfo("gui", "上次退出时隐私屏幕还开着，已复位为关闭");
    }
    app_->privacy().on_unlock = [this] {
        CheckDlgButton(hwnd_, kIdPrivacy, BST_UNCHECKED);
        app_->config().data["privacy"].set("enabled", Json::Bool(false));
        app_->PersistPrivacyEnabled(false);
        Say();
    };

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    Refresh();
    SetTimer(hwnd_, kRefreshTimer, kRefreshMs, nullptr);
    if (app_->config().flag("gui", "auto_start", true)) {
        SetTimer(hwnd_, kAutoStartTimer, 500, nullptr);   // 打开界面就开始干活
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    KillTimer(hwnd_, kRefreshTimer);
    if (font_) DeleteObject(font_);
    if (font_bold_) DeleteObject(font_bold_);
    if (font_title_) DeleteObject(font_title_);
    return 0;
}

}  // namespace cn
