// 时间：Unix 秒/毫秒、ISO8601 解析、本地时间格式化。
#pragma once

#include <string>

namespace cn {

double NowUnix();
long long NowUnixMs();

// 解析 "2026-10-03T12:31:39.255Z" / "2026-10-03T12:31:39+08:00"；失败返回 0
double ParseIso8601(const std::string& text);

// 本地时间格式化，格式串用 strftime 的写法
std::string FormatLocal(double unix_seconds, const char* format);
std::string NowStamp();        // %Y-%m-%dT%H:%M:%S
std::string NowClockHms();     // %H:%M:%S
std::string NowDate();         // %Y-%m-%d
std::string WeekdayCn(double unix_seconds);   // "星期一"

}  // namespace cn
