#include "queue.h"

#include <windows.h>

#include <mutex>

#include "util/log.h"
#include "util/paths.h"
#include "util/str.h"
#include "util/times.h"

// 只借用 sqlite3.h 的声明和常量；实现从 winsqlite3.dll 动态取，不链接任何 sqlite 库。
#include <sqlite3.h>

namespace cn {
namespace {

struct SqliteApi {
    HMODULE module = nullptr;
    decltype(&sqlite3_open_v2) open_v2 = nullptr;
    decltype(&sqlite3_close) close = nullptr;
    decltype(&sqlite3_prepare_v2) prepare_v2 = nullptr;
    decltype(&sqlite3_step) step = nullptr;
    decltype(&sqlite3_finalize) finalize = nullptr;
    decltype(&sqlite3_column_text) column_text = nullptr;
    decltype(&sqlite3_column_int64) column_int64 = nullptr;
    decltype(&sqlite3_busy_timeout) busy_timeout = nullptr;
    bool ready = false;
};

SqliteApi& Api() {
    static SqliteApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        api.module = LoadLibraryExW(L"winsqlite3.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!api.module) {
            LogWarn("queue", "找不到 winsqlite3.dll，跳过排队检查");
            return;
        }
        auto load = [&](const char* name) { return GetProcAddress(api.module, name); };
        api.open_v2 = (decltype(api.open_v2))load("sqlite3_open_v2");
        api.close = (decltype(api.close))load("sqlite3_close");
        api.prepare_v2 = (decltype(api.prepare_v2))load("sqlite3_prepare_v2");
        api.step = (decltype(api.step))load("sqlite3_step");
        api.finalize = (decltype(api.finalize))load("sqlite3_finalize");
        api.column_text = (decltype(api.column_text))load("sqlite3_column_text");
        api.column_int64 = (decltype(api.column_int64))load("sqlite3_column_int64");
        api.busy_timeout = (decltype(api.busy_timeout))load("sqlite3_busy_timeout");
        api.ready = api.open_v2 && api.close && api.prepare_v2 && api.step && api.finalize &&
                    api.column_text && api.column_int64;
        if (!api.ready) LogWarn("queue", "winsqlite3.dll 缺少需要的函数，跳过排队检查");
    });
    return api;
}

}  // namespace

std::set<std::string> PendingThreadIds(const std::string& db_path, double max_age_sec) {
    std::set<std::string> out;
    if (db_path.empty()) return out;
    std::string path = ResolvePath(db_path);
    if (!FileExists(path)) return out;

    SqliteApi& api = Api();
    if (!api.ready) return out;

    sqlite3* db = nullptr;
    std::string utf8_path = path;
    if (api.open_v2(utf8_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) api.close(db);   // 打开失败也可能返回句柄，必须关掉
        return out;
    }
    if (api.busy_timeout) api.busy_timeout(db, 2000);   // 对应 Python 的 timeout=2

    sqlite3_stmt* statement = nullptr;
    if (api.prepare_v2(db, "select thread_id, created_at_ms from queued_items", -1, &statement,
                       nullptr) != SQLITE_OK) {
        api.close(db);
        return out;
    }

    long long now_ms = NowUnixMs();
    while (api.step(statement) == SQLITE_ROW) {
        const unsigned char* text = api.column_text(statement, 0);
        if (!text || !*text) continue;
        long long created = api.column_int64(statement, 1);
        if (max_age_sec > 0 && created > 0 &&
            double(now_ms - created) > max_age_sec * 1000.0) {
            continue;   // 挂太久了，当作已失效
        }
        out.insert(reinterpret_cast<const char*>(text));
    }
    api.finalize(statement);
    api.close(db);
    return out;
}

bool HasQueuedWork(const std::string& thread_id, const std::string& db_path, double max_age_sec) {
    if (thread_id.empty()) return false;
    std::set<std::string> pending = PendingThreadIds(db_path, max_age_sec);
    return pending.count(thread_id) > 0;
}

}  // namespace cn
