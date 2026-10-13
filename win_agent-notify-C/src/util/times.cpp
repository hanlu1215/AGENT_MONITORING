#include "util/times.h"

#include <chrono>
#include <cstdio>
#include <ctime>

#include "util/str.h"

namespace cn {
namespace {

const char* kWeekdaysCn[7] = {"星期一", "星期二", "星期三", "星期四",
                              "星期五", "星期六", "星期日"};

int ParseInt(const std::string& text, size_t pos, size_t len, bool* ok) {
    if (pos + len > text.size()) {
        *ok = false;
        return 0;
    }
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        char ch = text[pos + i];
        if (ch < '0' || ch > '9') {
            *ok = false;
            return 0;
        }
        value = value * 10 + (ch - '0');
    }
    return value;
}

}  // namespace

double NowUnix() {
    return double(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count()) /
           1000.0;
}

long long NowUnixMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double ParseIso8601(const std::string& text) {
    // 期望形状：YYYY-MM-DDTHH:MM:SS[.fff][Z|+HH:MM|-HH:MM]
    if (text.size() < 19) return 0.0;
    bool ok = true;
    int year = ParseInt(text, 0, 4, &ok);
    int month = text[4] == '-' ? ParseInt(text, 5, 2, &ok) : 0;
    int day = text[7] == '-' ? ParseInt(text, 8, 2, &ok) : 0;
    int hour = ParseInt(text, 11, 2, &ok);
    int minute = text[13] == ':' ? ParseInt(text, 14, 2, &ok) : 0;
    int second = text[16] == ':' ? ParseInt(text, 17, 2, &ok) : 0;
    if (!ok || (text[10] != 'T' && text[10] != 't' && text[10] != ' ')) return 0.0;

    std::tm tm_value{};
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month - 1;
    tm_value.tm_mday = day;
    tm_value.tm_hour = hour;
    tm_value.tm_min = minute;
    tm_value.tm_sec = second;

    double fraction = 0.0;
    size_t pos = 19;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        double scale = 0.1;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            fraction += (text[pos] - '0') * scale;
            scale *= 0.1;
            ++pos;
        }
    }

    int offset_seconds = 0;
    if (pos < text.size()) {
        char zone = text[pos];
        if (zone == 'Z' || zone == 'z') {
            offset_seconds = 0;
        } else if (zone == '+' || zone == '-') {
            bool zone_ok = true;
            int zone_hour = ParseInt(text, pos + 1, 2, &zone_ok);
            int zone_min = (pos + 4 <= text.size() && text[pos + 3] == ':')
                               ? ParseInt(text, pos + 4, 2, &zone_ok)
                               : 0;
            if (zone_ok) {
                offset_seconds = zone_hour * 3600 + zone_min * 60;
                if (zone == '-') offset_seconds = -offset_seconds;
            }
        }
    }
    std::time_t utc = _mkgmtime(&tm_value);
    if (utc == (std::time_t)-1) return 0.0;
    return double(utc) - double(offset_seconds) + fraction;
}

std::string FormatLocal(double unix_seconds, const char* format) {
    std::time_t when = (std::time_t)unix_seconds;
    std::tm local{};
    if (localtime_s(&local, &when) != 0) return std::string();
    char buf[128];
    if (std::strftime(buf, sizeof(buf), format, &local) == 0) return std::string();
    return buf;
}

std::string NowStamp() { return FormatLocal(NowUnix(), "%Y-%m-%dT%H:%M:%S"); }
std::string NowClockHms() { return FormatLocal(NowUnix(), "%H:%M:%S"); }
std::string NowDate() { return FormatLocal(NowUnix(), "%Y-%m-%d"); }

std::string WeekdayCn(double unix_seconds) {
    std::time_t when = (std::time_t)unix_seconds;
    std::tm local{};
    if (localtime_s(&local, &when) != 0) return std::string();
    int index = local.tm_wday;
    if (index < 0 || index > 6) return std::string();
    return kWeekdaysCn[index];
}

}  // namespace cn
