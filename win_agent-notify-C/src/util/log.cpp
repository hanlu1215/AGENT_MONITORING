#include "util/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

#include "util/paths.h"

namespace cn {
namespace {

std::mutex g_lock;
std::ofstream g_file;
LogLevel g_level = LogLevel::Info;
bool g_ready = false;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARNING";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

}  // namespace

namespace logdetail {

void Write(LogLevel level, const std::string& module, const std::string& message) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (int(level) < int(g_level)) return;
    if (!g_ready) return;

    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[64];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count() %
              1000;
    char millis[8];
    std::snprintf(millis, sizeof(millis), ",%03d", int(ms));

    g_file << stamp << millis << " [" << LevelName(level) << "] codex_notify." << module << ": "
           << message << "\n";
    g_file.flush();
}

}  // namespace logdetail

void LogInit(const std::string& path, bool verbose) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_file.is_open()) g_file.close();
    std::string dir = PathDirName(path);
    if (!dir.empty()) EnsureDir(dir);
    g_file.open(path, std::ios::binary | std::ios::app);
    g_level = verbose ? LogLevel::Debug : LogLevel::Info;
    g_ready = g_file.is_open();
}

void LogSetLevel(LogLevel level) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_level = level;
}

void LogShutdown() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_file.is_open()) g_file.close();
    g_ready = false;
}

void LogDebug(const std::string& module, const std::string& message) {
    logdetail::Write(LogLevel::Debug, module, message);
}

void LogInfo(const std::string& module, const std::string& message) {
    logdetail::Write(LogLevel::Info, module, message);
}

void LogWarn(const std::string& module, const std::string& message) {
    logdetail::Write(LogLevel::Warn, module, message);
}

void LogError(const std::string& module, const std::string& message) {
    logdetail::Write(LogLevel::Error, module, message);
}

}  // namespace cn
