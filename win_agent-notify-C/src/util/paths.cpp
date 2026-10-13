#include "util/paths.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "util/str.h"

namespace fs = std::filesystem;

namespace cn {
namespace {

std::string g_exe_path;
std::string g_exe_dir;
bool g_ready = false;

void InitExeDirOnce() {
    if (g_ready) return;
    wchar_t buf[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    g_exe_path = WideToUtf8(std::wstring(buf, len));
    g_exe_dir = PathDirName(g_exe_path);
    g_ready = true;
}

std::string GetEnvUtf8(const wchar_t* name) {
    wchar_t buf[32767];
    DWORD len = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return std::string();
    return WideToUtf8(std::wstring(buf, len));
}

bool IsVarChar(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '_';
}

}  // namespace

const std::string& ExeDir() {
    InitExeDirOnce();
    return g_exe_dir;
}

const std::string& ExePath() {
    InitExeDirOnce();
    return g_exe_path;
}

std::string TempDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD len = GetTempPathW(DWORD(std::size(buf)), buf);
    if (len == 0) return ".";
    std::string dir = WideToUtf8(std::wstring(buf, len));
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    return dir;
}

std::string ExpandPath(const std::string& value) {
    std::string text = value;
    if (!text.empty() && text[0] == '~') {
        std::string home = GetEnvUtf8(L"USERPROFILE");
        if (home.empty()) {
            std::string drive = GetEnvUtf8(L"HOMEDRIVE");
            std::string path = GetEnvUtf8(L"HOMEPATH");
            home = drive + path;
        }
        if (!home.empty()) {
            text = home + text.substr(1);
        }
    }

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '%') {
            size_t end = text.find('%', i + 1);
            if (end != std::string::npos && end > i + 1) {
                std::string name = text.substr(i + 1, end - i - 1);
                std::string expanded = GetEnvUtf8(Utf8ToWide(name).c_str());
                if (!expanded.empty()) {
                    out += expanded;
                    i = end + 1;
                    continue;
                }
            }
        } else if (text[i] == '$' && i + 1 < text.size()) {
            size_t begin = i + 1;
            size_t end = begin;
            if (text[begin] == '{') {
                end = text.find('}', begin);
                if (end != std::string::npos) {
                    std::string name = text.substr(begin + 1, end - begin - 1);
                    std::string expanded = GetEnvUtf8(Utf8ToWide(name).c_str());
                    if (!expanded.empty()) {
                        out += expanded;
                        i = end + 1;
                        continue;
                    }
                }
            } else {
                while (end < text.size() && IsVarChar(text[end])) ++end;
                if (end > begin) {
                    std::string name = text.substr(begin, end - begin);
                    std::string expanded = GetEnvUtf8(Utf8ToWide(name).c_str());
                    if (!expanded.empty()) {
                        out += expanded;
                        i = end;
                        continue;
                    }
                }
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

std::string ResolvePath(const std::string& value) {
    std::string text = ExpandPath(value);
    if (text.empty()) return text;
    fs::path path = fs::u8path(text);
    std::error_code ec;
    if (path.is_relative()) {
        path = fs::u8path(ExeDir()) / path;
    }
    fs::path normalized = path.lexically_normal();
    return normalized.u8string();
}

bool FileExists(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(path), ec) && !ec;
}

bool DirExists(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(path), ec) && !ec;
}

long long FileSize(const std::string& path) {
    std::error_code ec;
    auto size = fs::file_size(fs::u8path(path), ec);
    if (ec) return -1;
    return (long long)size;
}

double FileMTime(const std::string& path) {
    std::error_code ec;
    fs::file_time_type time = fs::last_write_time(fs::u8path(path), ec);
    if (ec) return 0.0;
    // file_clock 的纪元在不同编译器上不同，这里统一换算成 Unix 秒：
    // 取 now() 的两个钟的差值来对齐。
    auto sys_now = std::chrono::system_clock::now();
    auto file_now = fs::file_time_type::clock::now();
    auto delta = std::chrono::duration_cast<std::chrono::system_clock::duration>(time - file_now);
    double seconds = std::chrono::duration<double>(sys_now.time_since_epoch() + delta).count();
    (void)ec;
    return seconds;
}

bool EnsureDir(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::u8path(path), ec);
    return !ec;
}

bool ReadFile(const std::string& path, std::string* out) {
    std::ifstream file(fs::u8path(path), std::ios::binary);
    if (!file) return false;
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    *out = std::move(data);
    return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& data) {
    fs::path target = fs::u8path(path);
    std::string tmp = path + ".tmp";
    {
        std::ofstream file(fs::u8path(tmp), std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(data.data(), std::streamsize(data.size()));
        file.flush();
        if (!file) return false;
    }
    std::error_code ec;
    fs::rename(fs::u8path(tmp), target, ec);
    if (!ec) return true;
    // 目标已存在时 rename 会失败（Windows），改用 MoveFileEx 覆盖
    if (MoveFileExW(fs::u8path(tmp).wstring().c_str(), target.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    fs::remove(fs::u8path(tmp), ec);
    return false;
}

bool RemoveFile(const std::string& path) {
    std::error_code ec;
    return fs::remove(fs::u8path(path), ec) && !ec;
}

std::string PathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string left = a;
    if (left.back() != '\\' && left.back() != '/') left.push_back('\\');
    return left + b;
}

std::string PathFileName(const std::string& path) {
    size_t at = path.find_last_of("\\/");
    return at == std::string::npos ? path : path.substr(at + 1);
}

std::string PathDirName(const std::string& path) {
    size_t at = path.find_last_of("\\/");
    if (at == std::string::npos) return std::string();
    if (at == 0) return path.substr(0, 1);
    return path.substr(0, at);
}

std::string PathExtension(const std::string& path) {
    std::string name = PathFileName(path);
    size_t at = name.find_last_of('.');
    if (at == std::string::npos || at == 0) return std::string();
    return ToLowerAscii(name.substr(at));
}

bool MatchesSuffix(const std::string& path, const std::vector<std::string>& suffixes) {
    if (suffixes.empty()) return true;
    std::string ext = PathExtension(path);
    for (const std::string& suffix : suffixes) {
        if (ext == suffix) return true;
    }
    return false;
}

std::vector<std::string> ListFilesRecursive(const std::string& root,
                                            const std::vector<std::string>& suffixes) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path base = fs::u8path(root);
    if (!fs::is_directory(base, ec)) return out;
    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;   // 目录被删/无权限：拿到多少算多少
        const fs::directory_entry& entry = *it;
        std::error_code item_ec;
        if (!entry.is_regular_file(item_ec) || item_ec) continue;
        std::string path = entry.path().u8string();
        if (MatchesSuffix(path, suffixes)) out.push_back(path);
    }
    return out;
}

std::vector<std::string> ListSubdirs(const std::string& root) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(root), fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code item_ec;
        if (it->is_directory(item_ec) && !item_ec) out.push_back(it->path().u8string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> ListFilesSorted(const std::string& root,
                                         const std::vector<std::string>& suffixes) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(root), fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code item_ec;
        if (!it->is_regular_file(item_ec) || item_ec) continue;
        std::string path = it->path().u8string();
        if (MatchesSuffix(path, suffixes)) out.push_back(path);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace cn
