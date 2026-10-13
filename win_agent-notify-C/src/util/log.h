// 日志：写 codex-notify.log，格式与 Python 版一致
//   2026-10-04 20:40:00,123 [INFO] codex_notify.monitor: 消息
#pragma once

#include <string>

namespace cn {

enum class LogLevel { Debug = 10, Info = 20, Warn = 30, Error = 40 };

namespace logdetail {
void Write(LogLevel level, const std::string& module, const std::string& message);
}

void LogInit(const std::string& path, bool verbose = false);
void LogSetLevel(LogLevel level);
void LogShutdown();

void LogDebug(const std::string& module, const std::string& message);
void LogInfo(const std::string& module, const std::string& message);
void LogWarn(const std::string& module, const std::string& message);
void LogError(const std::string& module, const std::string& message);

}  // namespace cn
