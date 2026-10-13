// 路径与文件：exe 目录、~ 与 %VAR%/$VAR 展开、原子写。
#pragma once

#include <string>
#include <vector>

namespace cn {

// exe 所在目录（= Python 版的 PROJECT_DIR：相对路径都以它为基准）
const std::string& ExeDir();
const std::string& ExePath();
std::string TempDir();

// 展开 ~（USERPROFILE / HOMEDRIVE+HOMEPATH）与 %VAR% / $VAR / ${VAR}
std::string ExpandPath(const std::string& value);
// 展开后，相对路径按 exe 目录解析
std::string ResolvePath(const std::string& value);

bool FileExists(const std::string& path);
bool DirExists(const std::string& path);
long long FileSize(const std::string& path);          // 不存在返回 -1
double FileMTime(const std::string& path);            // 不存在返回 0
bool EnsureDir(const std::string& path);
bool ReadFile(const std::string& path, std::string* out);
bool WriteFileAtomic(const std::string& path, const std::string& data);
bool RemoveFile(const std::string& path);

std::string PathJoin(const std::string& a, const std::string& b);
std::string PathFileName(const std::string& path);
std::string PathDirName(const std::string& path);
std::string PathExtension(const std::string& path);   // 含点，小写
// 递归列出目录下所有指定后缀的文件（不抛异常，读不到就返回已收集的）
std::vector<std::string> ListFilesRecursive(const std::string& root,
                                            const std::vector<std::string>& suffixes);
// 列出目录下的直接子目录
std::vector<std::string> ListSubdirs(const std::string& root);
// 列出目录下匹配后缀的直接子文件（按名字排序）
std::vector<std::string> ListFilesSorted(const std::string& root,
                                         const std::vector<std::string>& suffixes);
// 后缀是否匹配（大小写不敏感，后缀含点；suffixes 为空表示全要）
bool MatchesSuffix(const std::string& path, const std::vector<std::string>& suffixes);

}  // namespace cn
