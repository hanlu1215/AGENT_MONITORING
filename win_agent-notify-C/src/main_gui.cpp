// 图形界面版入口：**没有任何控制台窗口**（windows 子系统 + 从不碰控制台）。
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <vector>

#include "appctx.h"
#include "gui.h"
#include "util/log.h"
#include "util/paths.h"
#include "util/str.h"

namespace {

struct StartupOptions {
    std::string config_path;
    bool verbose = false;
};

ULONG_PTR g_gdiplus_token = 0;
bool g_gdiplus_started = false;

std::vector<std::string> CommandLineArgs() {
    std::vector<std::string> args;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return args;
    for (int i = 1; i < count; ++i) args.push_back(cn::WideToUtf8(argv[i]));
    LocalFree(argv);
    return args;
}

StartupOptions ParseStartupOptions(const std::vector<std::string>& args) {
    StartupOptions options;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config" && i + 1 < args.size()) {
            options.config_path = args[++i];
        } else if (cn::StartsWith(args[i], "--config=")) {
            options.config_path = args[i].substr(9);
        } else if (args[i] == "--verbose" || args[i] == "-v") {
            options.verbose = true;
        }
    }
    return options;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    StartupOptions options = ParseStartupOptions(CommandLineArgs());

    // 高 DPI：manifest 里已经声明了 PerMonitorV2，这里再兜一次（老系统上 manifest 可能被忽略）
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI * SetDpiFn)(DPI_AWARENESS_CONTEXT);
        auto set_dpi = (SetDpiFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (set_dpi) set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    Gdiplus::GdiplusStartupInput gdiplus_input;
    Gdiplus::GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr);
    g_gdiplus_started = true;

    cn::AppContext app;
    std::string error;
    if (!app.Init(options.config_path, &error)) {
        MessageBoxW(nullptr, cn::Utf8ToWide(error).c_str(), L"配置加载失败", MB_OK | MB_ICONERROR);
        return 1;
    }
    cn::LogInit(cn::ResolvePath(app.config().str("state", "log_path", "codex-notify.log")),
                options.verbose);
    cn::LogInfo("gui", cn::Cat("界面启动（配置：", app.config_path(), "）"));

    cn::MainWindow window;
    int code = window.Run(instance, &app, options.config_path);

    app.Shutdown();
    if (g_gdiplus_started) Gdiplus::GdiplusShutdown(g_gdiplus_token);
    return code;
}
