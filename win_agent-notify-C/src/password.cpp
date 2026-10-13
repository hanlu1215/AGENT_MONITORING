#include "password.h"

#include <commctrl.h>

#include "util/crypto.h"
#include "util/log.h"
#include "util/str.h"

namespace cn {
namespace {

const char* kSalt = "codex-notify-privacy-v1";
const wchar_t* kPromptClass = L"CodexNotifyPasswordPrompt";

struct PromptState {
    std::wstring title;
    std::wstring label;
    std::wstring text;
    bool mask = true;
    bool done = false;
    HWND edit = nullptr;
};

LRESULT CALLBACK PromptProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    PromptState* state = (PromptState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (message) {
        case WM_CREATE: {
            CREATESTRUCTW* create = (CREATESTRUCTW*)lparam;
            state = (PromptState*)create->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
            HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
            HWND label = CreateWindowExW(0, L"STATIC", state->label.c_str(),
                                         WS_CHILD | WS_VISIBLE, 16, 14, 320, 20, hwnd,
                                         nullptr, nullptr, nullptr);
            state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                              (state->mask ? ES_PASSWORD : 0),
                                          16, 40, 320, 24, hwnd, (HMENU)100, nullptr, nullptr);
            HWND ok = CreateWindowExW(0, L"BUTTON", L"确定",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 176,
                                      76, 76, 26, hwnd, (HMENU)IDOK, nullptr, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP, 260, 76, 76, 26,
                                          hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
            if (font) {
                SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageW(state->edit, WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
            }
            SetFocus(state->edit);
            return 0;
        }
        case WM_COMMAND:
            if (!state) break;
            if (LOWORD(wparam) == IDOK) {
                int length = GetWindowTextLengthW(state->edit);
                std::wstring buffer(size_t(length) + 1, L'\0');
                GetWindowTextW(state->edit, buffer.data(), length + 1);
                buffer.resize(size_t(length));
                state->text = buffer;
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                state->done = false;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (state) state->done = false;
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void EnsurePromptClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = PromptProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    window_class.lpszClassName = kPromptClass;
    RegisterClassExW(&window_class);
    registered = true;
}

}  // namespace

std::string HashPassword(const std::string& text) {
    return Sha256Hex(std::string(kSalt) + text);
}

bool HasPassword(const Config& cfg) {
    const Json& privacy = cfg.get("privacy");
    if (!privacy.is_object()) return false;
    return !Trim(privacy.get_str("password_sha256")).empty();
}

bool CheckPassword(const Config& cfg, const std::string& text) {
    const Json& privacy = cfg.get("privacy");
    if (!privacy.is_object()) return false;
    std::string want = ToLowerAscii(Trim(privacy.get_str("password_sha256")));
    if (want.empty()) return false;
    return ToLowerAscii(HashPassword(text)) == want;
}

bool PromptText(HWND owner, const std::wstring& title, const std::wstring& label, bool mask,
                std::wstring* out) {
    EnsurePromptClass();
    PromptState state;
    state.title = title;
    state.label = label;
    state.mask = mask;

    RECT owner_rect{};
    int width = 368;
    int height = 148;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (owner && GetWindowRect(owner, &owner_rect)) {
        x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
        y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    }

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kPromptClass, title.c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner,
                                nullptr, GetModuleHandleW(nullptr), &state);
    if (!hwnd) return false;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG message;
    while (!state.done && GetMessageW(&message, nullptr, 0, 0)) {
        if (message.hwnd == hwnd || IsChild(hwnd, message.hwnd)) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                state.done = false;
                DestroyWindow(hwnd);
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        } else {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (!state.done) return false;
    if (out) *out = state.text;
    return true;
}

bool PromptSetPassword(HWND owner, Config* cfg) {
    std::wstring first;
    if (!PromptText(owner, L"设置解锁密码", L"输入隐私屏幕的解锁密码：", true, &first)) {
        return false;
    }
    if (first.empty()) {
        MessageBoxW(owner, L"密码不能为空。", L"还没设密码", MB_OK | MB_ICONWARNING);
        return false;
    }
    std::wstring again;
    if (!PromptText(owner, L"再输一遍", L"再输一遍确认：", true, &again)) return false;
    if (first != again) {
        MessageBoxW(owner, L"两次输入不一样，请重新设置。", L"两次不一致",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    Json& privacy = (*cfg)["privacy"];
    if (!privacy.is_object()) privacy = Json::Object();
    privacy.set("password_sha256", Json::Str(HashPassword(WideToUtf8(first))));
    LogInfo("gui", "隐私屏幕密码已更新");
    return true;
}

}  // namespace cn
