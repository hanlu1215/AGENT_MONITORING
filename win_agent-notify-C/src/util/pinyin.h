// 汉字 -> 拼音，用来给语音缓存起个"看得懂"的文件名。
//
// 表是离线生成的（build/gen_pinyin.py + pypinyin），编译进程序，
// 所以运行时不需要 Python，也不需要任何拼音库。
#pragma once

#include <string>

namespace cn {

// 查一个汉字的拼音（无声调）。查不到返回 nullptr。
const char* LookupPinyin(unsigned code_point);

// 把一句话转成能当文件名用的拼音串：
//   汉字 -> 拼音音节（用 - 连接）
//   字母数字 -> 原样保留（转小写）
//   其它（空格、标点、符号）-> 分隔符 -
// 例："Codex 等待审批" -> "codex-deng-dai-shen-pi"
// 查不到的汉字退化成 u+码点（例如 u9f99），保证不会撞名。
std::string PinyinSlug(const std::string& text, size_t max_length = 60);

}  // namespace cn
